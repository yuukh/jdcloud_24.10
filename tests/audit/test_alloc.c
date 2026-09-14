/* SPDX-License-Identifier: GPL-2.0-only */
/* Host regression harness: the allocator functions are extracted verbatim
 * from the source under audit. Only the allocator and MMIO APIs are mocked.
 */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint32_t u32;
typedef uint8_t u8;
typedef uintptr_t dma_addr_t;
#define MAX_PPE_NUM 2
#define TABLE_1K 0
#ifdef TEST_RX_V2
#define DEF_ETRY_NUM 32768U
#define DEF_ETRY_NUM_CFG 5
#else
#define DEF_ETRY_NUM 16384U
#define DEF_ETRY_NUM_CFG 4
#endif
#define CFG_PPE_NUM (hnat_priv->ppe_num)
#define GFP_KERNEL 0
#define MTK_HNAT_V1 1
#define BIND 2
#define PPE_TB_BASE 0
#define PPE_MIB_TB_BASE 4
#define dev_info(...) ((void)0)
#define pr_info(...) ((void)0)

struct foe_entry {
    struct { unsigned int state; } bfib1;
    unsigned char payload[76];
};
struct mib_entry { unsigned char payload[16]; };
struct hnat_accounting { uint64_t bytes, packets; };
struct mtk_hnat_data { bool per_flow_accounting; int version; };
struct mtk_hnat {
    void *dev;
    unsigned int ppe_num, foe_etry_num, etry_num_cfg;
    struct mtk_hnat_data *data;
    struct foe_entry *foe_table_cpu[MAX_PPE_NUM];
    dma_addr_t foe_table_dev[MAX_PPE_NUM];
    struct mib_entry *foe_mib_cpu[MAX_PPE_NUM];
    dma_addr_t foe_mib_dev[MAX_PPE_NUM];
    struct hnat_accounting *acct[MAX_PPE_NUM];
    unsigned char *ppe_base[MAX_PPE_NUM];
};

static struct mtk_hnat instance;
static struct mtk_hnat *hnat_priv = &instance;
static struct mtk_hnat_data data;
static unsigned char registers[MAX_PPE_NUM][8];
static int debug_level;
static unsigned int tests, allocations, frees, live, cache_flushes;
static unsigned int step, fail_step, last_count, current_slot;
static int fail_slot;
static unsigned int fail_above;
static bool fail_all;

struct allocation { void *ptr; size_t size; bool dma; };
static struct allocation records[16];

static struct allocation *find_record(const void *ptr)
{
    for (unsigned int i = 0; i < 16; i++)
        if (records[i].ptr == ptr)
            return &records[i];
    assert(!"free or hardware access to unallocated memory");
    return NULL;
}

static void *allocate(size_t size, bool dma)
{
    unsigned int count = hnat_priv->foe_etry_num;
    assert(count >= 1024 && count <= DEF_ETRY_NUM);
    assert((count & (count - 1)) == 0);
    assert(size > 0);
    if (count != last_count) {
        last_count = count;
        current_slot = 0;
    }
    unsigned int slot = current_slot++;
    step++;
    if (fail_all || (fail_step && step == fail_step) ||
        ((fail_slot < 0 || slot == (unsigned int)fail_slot) && count > fail_above))
        return NULL;
    for (unsigned int i = 0; i < 16; i++) {
        if (!records[i].ptr) {
            void *ptr = calloc(1, size);
            assert(ptr);
            records[i] = (struct allocation){ ptr, size, dma };
            allocations++;
            live++;
            return ptr;
        }
    }
    assert(!"allocator leaked a previous table set");
    return NULL;
}

static void *dma_alloc_coherent(void *dev, size_t size, dma_addr_t *addr, int flags)
{
    (void)flags;
    assert(dev == hnat_priv->dev);
    void *ptr = allocate(size, true);
    if (ptr)
        *addr = (dma_addr_t)ptr;
    return ptr;
}

static void dma_free_coherent(void *dev, size_t size, void *ptr, dma_addr_t addr)
{
    assert(dev == hnat_priv->dev);
    assert(addr == (dma_addr_t)ptr);
    struct allocation *record = find_record(ptr);
    assert(record->dma && record->size == size);
    free(ptr);
    *record = (struct allocation){0};
    live--;
    frees++;
}

static void *kcalloc(size_t count, size_t size, int flags)
{
    (void)flags;
    assert(!size || count <= SIZE_MAX / size);
    return allocate(count * size, false);
}

#ifdef TEST_BASELINE
static void *kzalloc(size_t size, int flags)
{
    (void)flags;
    return allocate(size, false);
}
#endif

static void kfree(void *ptr)
{
    if (!ptr)
        return;
    struct allocation *record = find_record(ptr);
    assert(!record->dma);
    free(ptr);
    *record = (struct allocation){0};
    live--;
    frees++;
}

static void writel(dma_addr_t value, void *address)
{
    (void)value;
    (void)address;
}

static void exclude_boundary_entry(struct foe_entry *entry) { (void)entry; }

static int hnat_hw_init(u32 ppe_id)
{
    struct allocation *record = find_record(hnat_priv->foe_table_cpu[ppe_id]);
    size_t hardware_size = (1024U << hnat_priv->etry_num_cfg) * sizeof(struct foe_entry);
    if (record->size != hardware_size) {
        fprintf(stderr, "hardware table size %zu exceeds allocation %zu at PPE%u\n",
                hardware_size, record->size, ppe_id);
        exit(42);
    }
    return 0;
}

static int entry_mac_cmp(struct foe_entry *entry, u8 *mac)
{
    return entry->payload[0] == mac[0];
}
static void hnat_cache_ebl(int enable) { assert(enable == 1); cache_flushes++; }

#include "alloc_extracted.h"

static void reset(unsigned int ppes, bool accounting)
{
    assert(live == 0);
    memset(&instance, 0, sizeof(instance));
    memset(&data, 0, sizeof(data));
    instance.dev = &instance;
    instance.data = &data;
    instance.ppe_num = ppes;
    instance.foe_etry_num = DEF_ETRY_NUM;
    for (unsigned int i = 0; i < ppes; i++)
        instance.ppe_base[i] = registers[i];
    data.per_flow_accounting = accounting;
    data.version = 4;
    step = fail_step = current_slot = last_count = cache_flushes = 0;
    fail_all = false;
    fail_above = DEF_ETRY_NUM;
    fail_slot = -1;
}

#ifndef TEST_BASELINE
static void check_tables(unsigned int expected_count)
{
    assert(hnat_priv->foe_etry_num == expected_count);
    assert((1024U << hnat_priv->etry_num_cfg) == expected_count);
    for (unsigned int i = 0; i < CFG_PPE_NUM; i++) {
        assert(find_record(hnat_priv->foe_table_cpu[i])->size ==
               expected_count * sizeof(struct foe_entry));
        if (data.per_flow_accounting) {
            assert(find_record(hnat_priv->foe_mib_cpu[i])->size ==
                   expected_count * sizeof(struct mib_entry));
            assert(find_record(hnat_priv->acct[i])->size ==
                   expected_count * sizeof(struct hnat_accounting));
        } else {
            assert(!hnat_priv->foe_mib_cpu[i] && !hnat_priv->acct[i]);
        }
        assert(hnat_start(i) == 0);
    }
    assert(hnat_start(CFG_PPE_NUM) == -EINVAL);
}

static void release_tables(void)
{
    for (unsigned int i = 0; i < CFG_PPE_NUM; i++) {
        hnat_free_tables(i);
        assert(!hnat_priv->foe_table_cpu[i] && !hnat_priv->foe_mib_cpu[i]);
        assert(!hnat_priv->acct[i]);
        assert(!hnat_priv->foe_table_dev[i] && !hnat_priv->foe_mib_dev[i]);
        hnat_free_tables(i); /* Partial-probe cleanup is safe to repeat. */
    }
    assert(live == 0 && allocations == frees);
}

static void test_allocation_matrix(void)
{
    for (unsigned int ppes = 1; ppes <= MAX_PPE_NUM; ppes++) {
        for (unsigned int accounting = 0; accounting <= 1; accounting++) {
            unsigned int slots = ppes * (accounting ? 3 : 1);
            reset(ppes, accounting);
            assert(hnat_alloc_tables() == 0);
            check_tables(DEF_ETRY_NUM);
            release_tables();
            tests++;

            for (unsigned int slot = 0; slot < slots; slot++) {
                reset(ppes, accounting);
                fail_step = slot + 1;
                assert(hnat_alloc_tables() == 0);
                check_tables(DEF_ETRY_NUM / 2);
                release_tables();
                tests++;

                for (unsigned int count = 1024; count < DEF_ETRY_NUM; count *= 2) {
                    reset(ppes, accounting);
                    fail_above = count;
                    fail_slot = (int)slot;
                    assert(hnat_alloc_tables() == 0);
                    check_tables(count);
                    release_tables();
                    tests++;
                }

                reset(ppes, accounting);
                fail_above = 0;
                fail_slot = (int)slot;
                assert(hnat_alloc_tables() == -ENOMEM);
                assert(last_count == 1024);
                assert(step == (DEF_ETRY_NUM_CFG + 1) * (slot + 1));
                release_tables();
                tests++;
            }

            reset(ppes, accounting);
            fail_all = true;
            assert(hnat_alloc_tables() == -ENOMEM);
            assert(step == DEF_ETRY_NUM_CFG + 1);
            release_tables();
            tests++;
        }
    }
}

static void test_roaming_bounds(void)
{
    u8 mac[6] = {0x24, 0, 0, 0, 0, 0};
    reset(2, true);
    fail_above = 1024;
    assert(hnat_alloc_tables() == 0);
    for (unsigned int i = 0; i < CFG_PPE_NUM; i++) {
        struct foe_entry *entry = &hnat_priv->foe_table_cpu[i][1023];
        entry->bfib1.state = BIND;
        entry->payload[0] = mac[0];
    }
    assert(entry_delete_by_mac(mac) == 2);
    assert(cache_flushes == 2);
    assert(entry_delete_by_mac(mac) == 0);
    release_tables();
    tests++;
}
#endif

int main(void)
{
#ifdef TEST_BASELINE
    reset(2, true);
    fail_step = 1; /* PPE0 shrinks successfully, then PPE1 must use its geometry. */
    assert(hnat_start(0) == 0);
    assert(hnat_start(1) == 0); /* Original implementation exits 42 here. */
    return 1;
#else
    test_allocation_matrix();
    test_roaming_bounds();
    printf("allocation tests: %u passed; allocations=%u frees=%u live=%u\n",
           tests, allocations, frees, live);
    return 0;
#endif
}
