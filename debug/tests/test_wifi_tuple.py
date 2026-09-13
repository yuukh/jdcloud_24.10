"""Run the actual vendor callback, before/after the patch, with skb fixtures.

This is an ASan/UBSan logic test, not a complete mt_wifi module or radio test.
Only generated files in /cache are changed; the pinned checkout is read-only.
"""
from pathlib import Path
import os
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).parents[1]))
from install import UPSTREAM_REV
from validate import patch_counts

ROOT = Path(__file__).parents[1]
SOURCE = 'mt_wifi/embedded/plug_in/whnat/woe_client_jedi.c'
STUBS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
struct sk_buff { unsigned int skb_iif; bool offload_no_fdb; unsigned int tags[2][5]; };
struct whnat_entry { int idx; struct { bool hw_tx_en; } cfg; };
struct wlan_tx_info { void *pkt; int ringidx, wcid, bssidx; };
struct _TX_BLK { struct sk_buff *pPacket; bool DropPkt; };
static unsigned int metadata_reads, calls;
static void wifi_tx_info_wrapper(unsigned char *p, struct wlan_tx_info *i)
{ i->pkt = ((struct _TX_BLK *)p)->pPacket; i->ringidx=1; i->wcid=2; i->bssidx=3; }
static int hook(struct sk_buff *skb, int port) { assert(skb && port == 1); calls++; return 1; }
static int (*ra_sw_nat_hook_tx)(struct sk_buff *, int) = hook;
#define WHNAT_DBG(...) do {} while (0)
#define WHNAT_WDMA_PORT 1
#define HIT_UNBIND_RATE_REACH 15
#define TRUE true
#define FOE_AI_HEAD(s) (++metadata_reads, (s)->tags[0][0])
#define FOE_AI_TAIL(s) (++metadata_reads, (s)->tags[1][0])
#define IS_SPACE_AVAILABLE_HEAD(s) true
#define IS_SPACE_AVAILABLE_TAIL(s) true
#define FOE_WDMA_ID_HEAD(s) (s)->tags[0][1]
#define FOE_RX_ID_HEAD(s) (s)->tags[0][2]
#define FOE_WC_ID_HEAD(s) (s)->tags[0][3]
#define FOE_BSS_ID_HEAD(s) (s)->tags[0][4]
#define FOE_WDMA_ID_TAIL(s) (s)->tags[1][1]
#define FOE_RX_ID_TAIL(s) (s)->tags[1][2]
#define FOE_WC_ID_TAIL(s) (s)->tags[1][3]
#define FOE_BSS_ID_TAIL(s) (s)->tags[1][4]
'''
CASES = r'''
int main(void)
{
    struct whnat_entry whnat = {.idx=9, .cfg={.hw_tx_en=true}};
    struct sk_buff skb = {.tags={{15}, {15}}}, before=skb;
    struct _TX_BLK tx = {.pPacket=&skb};
    wifi_tx_tuple_add(&whnat, (unsigned char *)&tx);
#ifdef BEFORE_FIX
    assert(metadata_reads && calls == 1);
    assert(memcmp(&skb, &before, sizeof(skb)) != 0);
#else
    assert(metadata_reads == 0 && calls == 0);
    assert(memcmp(&skb, &before, sizeof(skb)) == 0);
#endif
    for (int artificial=0; artificial<2; artificial++) {
        skb=before; skb.skb_iif=artificial ? 0 : 4; skb.offload_no_fdb=artificial;
        metadata_reads=calls=0;
        wifi_tx_tuple_add(&whnat, (unsigned char *)&tx);
        assert(metadata_reads && calls == 1 && !tx.DropPkt);
        assert(skb.tags[0][1] == 9 && skb.tags[1][4] == 3);
    }
    puts("WIFI_TUPLE_PROVENANCE_PASS");
    return 0;
}
'''


class WifiTupleTests(unittest.TestCase):
    def test_actual_callback_before_and_after(self):
        repo = Path(os.environ.get('HNAT418_UPSTREAM_REPO', str(ROOT.parents[1] / 'immortalwrt-mt798x-6.6')))
        original = subprocess.check_output(['git', '-C', str(repo), 'show',
            UPSTREAM_REV + ':package/mtk/drivers/mt_wifi/src/' + SOURCE], text=True)
        patch = ROOT / 'mt_wifi/999-hnat418-local-tuple-provenance.patch'
        self.assertEqual(patch_counts(patch), [])
        cache = Path('/cache/hnat418-debug/host-tests')
        cache.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=cache) as temp:
            directory = Path(temp)
            target = directory / SOURCE
            target.parent.mkdir(parents=True)
            target.write_text(original)
            subprocess.run(['patch', '--batch', '--fuzz=0', '-p1', '-d', str(directory),
                            '-i', str(patch)], check=True, capture_output=True)
            for before, text in ((True, original), (False, target.read_text())):
                start = text.index('void wifi_tx_tuple_add(')
                callback = text[start:text.index('\n}\n', start) + 3]
                (directory / 'test.c').write_text(STUBS + callback + CASES)
                command = ['gcc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-O1', '-g',
                           '-fsanitize=address,undefined', '-fno-omit-frame-pointer']
                if before:
                    command.append('-DBEFORE_FIX')
                subprocess.run(command + [str(directory / 'test.c'), '-o', str(directory / 'test')], check=True)
                result = subprocess.run([str(directory / 'test')], capture_output=True, text=True, timeout=15)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(result.stderr, '')
                self.assertIn('WIFI_TUPLE_PROVENANCE_PASS', result.stdout)


if __name__ == '__main__':
    unittest.main()
