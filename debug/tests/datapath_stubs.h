/* Host-only skb fixture. Production predicate/parser bodies are inserted
 * unchanged by test_datapath.py; this is not a replacement network stack. */
#include <arpa/inet.h>
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <linux/ip.h>
#include <linux/tcp.h>

#define ETH_P_IP 0x0800
#define ETH_P_8021Q 0x8100
#define ETH_P_8021AD 0x88a8
#define VLAN_HLEN 4
#define FOE_INFO_LEN 13
struct vlan_hdr { uint16_t h_vlan_TCI, h_vlan_encapsulated_proto; };
struct sk_buff {
    unsigned char *head, *data;
    unsigned int len, data_len, skb_iif, network_header;
    bool offload_no_fdb, extge, tag;
    unsigned int sport;
    uint16_t protocol;
};
static unsigned int metadata_reads;
static unsigned int skb_headroom(const struct sk_buff *skb) { return skb->data - skb->head; }
static unsigned int skb_headlen(const struct sk_buff *skb) { return skb->len - skb->data_len; }
static int skb_network_offset(const struct sk_buff *skb) { return (int)skb->network_header - (int)skb_headroom(skb); }
static const void *skb_header_pointer(const struct sk_buff *skb, int offset, int len, void *buffer)
{
    if (offset < -(int)skb_headroom(skb) || offset > (int)skb->len - len)
        return NULL;
    if (offset + len <= (int)skb_headlen(skb))
        return skb->data + offset;
    /* The fixture keeps a contiguous backing copy of nonlinear data. */
    memcpy(buffer, skb->data + offset, len);
    return buffer;
}
static bool eth_type_vlan(uint16_t protocol)
{
    return protocol == htons(ETH_P_8021Q) || protocol == htons(ETH_P_8021AD);
}
static bool ip_is_fragment(const struct iphdr *ip) { return (ntohs(ip->frag_off) & 0x3fff) != 0; }
#define IS_SPACE_AVAILABLE_HEAD(s) (skb_headroom(s) >= FOE_INFO_LEN)
#define is_magic_tag_valid(s) (++metadata_reads, (s)->tag)
#define is_from_extge(s) (++metadata_reads, (s)->extge)
#define FROM_GE_PPD(s) ((s)->sport == 1)
#define FROM_GE_LAN(s) ((s)->sport == 2)
#define FROM_GE_WAN(s) ((s)->sport == 3)
#define FROM_WED(s) ((s)->sport == 4)
#define FROM_EXT(s) ((s)->sport == 5)
#define FROM_GE_VIRTUAL(s) ((s)->sport == 6)
