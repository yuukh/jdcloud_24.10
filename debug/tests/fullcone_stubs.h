/* Fixtures for the actual vendor nf_nat_masquerade_ipv4 function.
 * These test extension lifetime and bounded control flow, not a live NAT.
 */
#include <arpa/inet.h>
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint32_t __be32;
typedef uint16_t u_int16_t;
#define NF_ACCEPT 1u
#define NF_DROP 0u
#define NF_INET_POST_ROUTING 4u
#define IP_CT_DIR_ORIGINAL 0
#define NF_NAT_MANIP_SRC 1
#define NF_NAT_RANGE_MAP_IPS 1u
#define NF_NAT_RANGE_PROTO_SPECIFIED 2u
#define RT_SCOPE_UNIVERSE 0
#define GFP_ATOMIC 0
#define pr_debug(...) do {} while (0)
#define pr_info(...) do {} while (0)
#define WARN_ON(x) do { assert(!(x)); } while (0)

enum ip_conntrack_info { IP_CT_NEW, IP_CT_RELATED, IP_CT_RELATED_REPLY };
union inet_addr { __be32 ip; };
union proto { uint16_t all; struct { uint16_t port; } udp; };
struct endpoint { union inet_addr u3; union proto u; };
struct tuple { struct endpoint src, dst; };
struct nf_conntrack_helper { int tag; };
struct nf_conn_help { struct nf_conntrack_helper *helper; };
struct nf_conn_nat { int masq_index; };
struct nf_conn {
    bool confirmed, has_help;
    int protocol;
    struct { struct tuple tuple; } tuplehash[1];
    struct nf_conn_help help;
    struct nf_conn_nat nat;
};
struct nf_conntrack_expect { struct tuple tuple; };
struct nf_nat_range2 {
    unsigned int flags;
    union inet_addr min_addr, max_addr;
    union proto min_proto, max_proto;
};
struct iphdr { __be32 daddr; };
struct rtable { int dummy; };
struct sk_buff { struct nf_conn *ct; };
struct net_device { int ifindex; const char *name; };

static struct nf_conntrack_helper nf_conntrack_helper_bcm_nat, other_helper;
unsigned int nf_conntrack_nat_mode = 1;
static struct nf_conntrack_expect expectation;
static bool reuse_expect, ports_busy, fail_helper;
static unsigned int setup_verdict = NF_ACCEPT, setup_calls, helper_calls;
static unsigned int late_extensions, scans, nat_calls, lookup_calls;
static int nf_conntrack_expect_lock, lock_depth;
static __be32 chosen_address = 0x01020304;
static struct nf_nat_range2 chosen_range;

static bool nf_ct_is_confirmed(const struct nf_conn *ct) { return ct->confirmed; }
static struct nf_conn *nf_ct_get(struct sk_buff *skb, enum ip_conntrack_info *info)
{ *info = IP_CT_NEW; return skb->ct; }
static struct nf_conn_help *nfct_help(struct nf_conn *ct)
{ return ct->has_help ? &ct->help : NULL; }
static struct nf_conn_help *nf_ct_helper_ext_add(struct nf_conn *ct, int flags)
{
    assert(flags == GFP_ATOMIC);
    helper_calls++;
    if (ct->confirmed) late_extensions++;
    if (fail_helper) return NULL;
    ct->has_help = true;
    return &ct->help;
}
static struct nf_conn_nat *nf_ct_nat_ext_add(struct nf_conn *ct)
{ nat_calls++; return &ct->nat; }
static unsigned int nf_nat_setup_info(struct nf_conn *ct,
                                     const struct nf_nat_range2 *range, int type)
{
    assert(type == NF_NAT_MANIP_SRC);
    setup_calls++;
    /* Match the real API: NF_ACCEPT is also its confirmed/no-op verdict. */
    if (nf_ct_is_confirmed(ct)) return NF_ACCEPT;
    chosen_range = *range;
    return setup_verdict;
}
static int nf_ct_protonum(struct nf_conn *ct) { return ct->protocol; }
static struct rtable *skb_rtable(struct sk_buff *skb)
{ static struct rtable rt; (void)skb; return &rt; }
static struct iphdr *ip_hdr(struct sk_buff *skb)
{ static struct iphdr ip; (void)skb; return &ip; }
static __be32 rt_nexthop(const struct rtable *rt, __be32 dst)
{ (void)rt; return dst; }
static __be32 inet_select_addr(const struct net_device *dev, __be32 nh, int scope)
{ (void)dev; (void)nh; assert(scope == RT_SCOPE_UNIVERSE); return chosen_address; }
static void spin_lock_bh(int *lock)
{ (void)lock; assert(lock_depth == 0); lock_depth++; }
static void spin_unlock_bh(int *lock)
{ (void)lock; assert(lock_depth == 1); lock_depth--; }
static struct nf_conntrack_expect *find_fullcone_exp(struct nf_conn *ct)
{ (void)ct; assert(lock_depth == 1); lookup_calls++; return reuse_expect ? &expectation : NULL; }
static int find_exp(__be32 ip, uint16_t port, struct nf_conn *ct)
{
    (void)ip; (void)port; (void)ct;
    assert(lock_depth == 1);
    /* Stop the old u16 implementation after its first wrap, rather than
     * deliberately hanging the test host with an unbounded process.
     */
    if (++scans > 32768) { puts("OLD_PORT_SEARCH_WRAPPED_WITH_LOCK_HELD"); exit(77); }
    return ports_busy;
}
