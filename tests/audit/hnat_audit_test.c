// SPDX-License-Identifier: GPL-2.0-only
/* Exercise real skb, GSO, VLAN, netlink and kthread APIs. Hardware egress,
 * table deletion and explicit error injection are the only mocked operations.
 * Production functions are included or extracted verbatim by run_kunit.py.
 */
#include <kunit/test.h>
#include <linux/delay.h>
#include <linux/etherdevice.h>
#include <linux/if_vlan.h>
#include <linux/kthread.h>
#include <linux/netfilter.h>
#include <linux/rtnetlink.h>
#include <net/dst_metadata.h>
#include <net/gro.h>
#include <net/gso.h>
#include <net/ip.h>
#include <net/ipv6.h>
#include <net/netlink.h>
#include <net/sock.h>
#include <net/tcp.h>
#include "../drivers/net/ethernet/mediatek/mtk_hnat/nf_hnat_mtk.h"
#include "../drivers/net/ethernet/mediatek/mtk_hnat/hnat.h"

struct audit_context {
	struct mtk_hnat hnat;
	struct mtk_eth eth;
	struct mtk_soc_data soc;
	struct net_device *dev;
	struct sk_buff *owned[32];
	struct sk_buff *transmitted;
	unsigned int count, socket_releases;
	bool fail_vlan, fail_socket, fail_bind, fail_thread;
	int xmit_result;
	atomic_t mac_calls;
	u8 last_mac[ETH_ALEN];
};

static struct audit_context *context;
static struct socket *_hnat_roam_sock;
static struct task_struct *_hnat_roam_task;

static int test_vlan_insert(struct sk_buff *skb, __be16 proto, u16 tci)
{
	return context->fail_vlan ? -ENOMEM : __vlan_insert_tag(skb, proto, tci);
}

static int test_queue_xmit(struct sk_buff *skb)
{
	context->transmitted = skb;
	return context->xmit_result;
}

#define hnat_priv (&context->hnat)
#define __vlan_insert_tag test_vlan_insert
#define dev_queue_xmit test_queue_xmit
#include "../drivers/net/ethernet/mediatek/mtk_hnat/hnat_cpu.h"
#undef hnat_priv
#undef __vlan_insert_tag
#undef dev_queue_xmit

static int test_delete_mac(u8 *mac)
{
	memcpy(context->last_mac, mac, ETH_ALEN);
	atomic_inc(&context->mac_calls);
	return 0;
}

static int test_create_socket(struct net *net, int family, int type,
			      int protocol, struct socket **result)
{
	if (context->fail_socket)
		return -ENOMEM;
	return sock_create_kern(net, family, type, protocol, result);
}

static int test_bind_socket(struct socket *sock, struct sockaddr *addr, int len)
{
	if (context->fail_bind)
		return -EADDRINUSE;
	return kernel_bind(sock, addr, len);
}

static struct task_struct *test_run_thread(int (*fn)(void *), void *data,
					 const char *name)
{
	if (context->fail_thread)
		return ERR_PTR(-ENOMEM);
	return kthread_run(fn, data, "%s", name);
}

static void test_release_socket(struct socket *sock)
{
	context->socket_releases++;
	sock_release(sock);
}

#define entry_delete_by_mac test_delete_mac
#define sock_create_kern test_create_socket
#define kernel_bind test_bind_socket
#undef kthread_run
#define kthread_run test_run_thread
#define sock_release test_release_socket
#include "hnat_roam_extracted.h"
#undef entry_delete_by_mac
#undef sock_create_kern
#undef kernel_bind
#undef kthread_run
#undef sock_release

#include "hnat_audit_extracted.h"

static void own(struct sk_buff *skb)
{
	context->owned[context->count++] = skb;
}

static struct sk_buff *packet(bool ipv6)
{
	struct sk_buff *skb = alloc_skb(512, GFP_KERNEL);
	struct ethhdr *eth;
	struct tcphdr *tcp;

	if (!skb)
		return NULL;
	own(skb);
	memset(skb->head, 0x5a, 96);
	skb_reserve(skb, 96);
	skb_reset_mac_header(skb);
	eth = skb_put_zero(skb, ETH_HLEN);
	eth->h_dest[0] = 2;
	eth->h_source[0] = 4;
	eth->h_proto = htons(ipv6 ? ETH_P_IPV6 : ETH_P_IP);
	skb->protocol = eth->h_proto;
	skb_set_network_header(skb, ETH_HLEN);
	if (ipv6) {
		struct ipv6hdr *ip6h = skb_put_zero(skb, sizeof(*ip6h));

		ip6h->version = 6;
		ip6h->nexthdr = IPPROTO_TCP;
		ip6h->payload_len = htons(sizeof(*tcp) + 16);
	} else {
		struct iphdr *iph = skb_put_zero(skb, sizeof(*iph));

		iph->version = 4;
		iph->ihl = 5;
		iph->protocol = IPPROTO_TCP;
		iph->tot_len = htons(sizeof(*iph) + sizeof(*tcp) + 16);
	}
	skb_set_transport_header(skb, skb->len);
	tcp = skb_put_zero(skb, sizeof(*tcp));
	tcp->doff = 5;
	tcp->check = htons(0x1234);
	skb_put_zero(skb, 16);
	skb_pull(skb, ETH_HLEN);
	skb->dev = context->dev;
	return skb;
}

static int setup(struct kunit *test)
{
	struct mtk_mac *mac;

	context = kunit_kzalloc(test, sizeof(*context), GFP_KERNEL);
	if (!context)
		return -ENOMEM;
	context->dev = alloc_etherdev(sizeof(*mac));
	if (!context->dev)
		return -ENOMEM;
	context->dev->flags |= IFF_UP;
	context->hnat.g_ppdev = context->dev;
	mac = netdev_priv(context->dev);
	mac->hw = &context->eth;
	context->eth.soc = &context->soc;
	atomic_set(&context->mac_calls, 0);
	return 0;
}

static void teardown(struct kunit *test)
{
	unsigned int i;

	hnat_roaming_disable();
	for (i = 0; i < context->count; i++)
		kfree_skb(context->owned[i]);
	free_netdev(context->dev);
}

static void ipv4_hardware_path(struct kunit *test)
{
	struct sk_buff *skb = packet(false);

	KUNIT_ASSERT_NOT_NULL(test, skb);
	skb->mark = 0x42;
	KUNIT_EXPECT_EQ(test, do_hnat_cpu_to_ge(skb), (unsigned int)NF_STOLEN);
	KUNIT_EXPECT_PTR_EQ(test, context->transmitted, skb);
	KUNIT_EXPECT_EQ(test, skb->mark, 0x42U);
	KUNIT_EXPECT_EQ(test, skb_vlan_tag_get(skb), 1234);
	KUNIT_EXPECT_EQ(test, skb_network_offset(skb), ETH_HLEN);
	KUNIT_EXPECT_TRUE(test, is_to_ppe(skb));
}

static void ipv6_hardware_path(struct kunit *test)
{
	struct sk_buff *skb = packet(true);

	KUNIT_ASSERT_NOT_NULL(test, skb);
	KUNIT_EXPECT_EQ(test, do_hnat_cpu_to_ge(skb), (unsigned int)NF_STOLEN);
	KUNIT_EXPECT_EQ(test, skb->protocol, htons(ETH_P_IPV6));
}

static void udp_hardware_path(struct kunit *test)
{
	struct sk_buff *skb = packet(false);

	KUNIT_ASSERT_NOT_NULL(test, skb);
	ip_hdr(skb)->protocol = IPPROTO_UDP;
	KUNIT_EXPECT_EQ(test, do_hnat_cpu_to_ge(skb), (unsigned int)NF_STOLEN);
}

static void bypass_is_unchanged(struct kunit *test)
{
	struct sk_buff *skb = packet(false);
	unsigned char head[96], *data;
	unsigned int len;

	KUNIT_ASSERT_NOT_NULL(test, skb);
	skb->mark = HNAT_EXCEPTION_TAG;
	data = skb->data;
	len = skb->len;
	memcpy(head, skb->head, sizeof(head));
	KUNIT_EXPECT_EQ(test, do_hnat_cpu_to_ge(skb), (unsigned int)NF_ACCEPT);
	KUNIT_EXPECT_PTR_EQ(test, skb->data, data);
	KUNIT_EXPECT_EQ(test, skb->len, len);
	KUNIT_EXPECT_EQ(test, memcmp(head, skb->head, sizeof(head)), 0);
	KUNIT_EXPECT_PTR_EQ(test, context->transmitted, NULL);
}

static void metadata_requires_real_provenance(struct kunit *test)
{
	struct sk_buff *skb = packet(false);

	KUNIT_ASSERT_NOT_NULL(test, skb);
	skb_hnat_iface(skb) = FOE_MAGIC_GE_LAN;
	skb_hnat_magic_tag(skb) = HNAT_MAGIC_TAG;
	KUNIT_EXPECT_FALSE(test, hnat_cpu_has_valid_info(skb));
	skb->skb_iif = 2;
	KUNIT_EXPECT_TRUE(test, hnat_cpu_has_valid_info(skb));
	skb_hnat_magic_tag(skb) = 0;
	KUNIT_EXPECT_FALSE(test, hnat_cpu_has_valid_info(skb));
	skb->skb_iif = 0;
	skb->offload_no_fdb = 1;
	skb_hnat_magic_tag(skb) = HNAT_MAGIC_TAG;
	KUNIT_EXPECT_TRUE(test, hnat_cpu_has_valid_info(skb));
}

static void insufficient_metadata_headroom(struct kunit *test)
{
	struct sk_buff *skb = alloc_skb(32, GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, skb);
	own(skb);
	skb->skb_iif = 2;
	skb_put_zero(skb, 32);
	KUNIT_EXPECT_FALSE(test, hnat_cpu_has_valid_info(skb));
}

static void down_device_bypasses(struct kunit *test)
{
	struct sk_buff *skb = packet(false);

	KUNIT_ASSERT_NOT_NULL(test, skb);
	context->dev->flags &= ~IFF_UP;
	KUNIT_EXPECT_EQ(test, do_hnat_cpu_to_ge(skb), (unsigned int)NF_ACCEPT);
	context->hnat.g_ppdev = NULL;
	KUNIT_EXPECT_EQ(test, do_hnat_cpu_to_ge(skb), (unsigned int)NF_ACCEPT);
}

static void vendor_packet_eligibility_is_preserved(struct kunit *test)
{
	struct sk_buff *fragment = packet(false);
	struct sk_buff *options = packet(false);
	struct sk_buff *multicast = packet(false);

	KUNIT_ASSERT_NOT_NULL(test, fragment);
	KUNIT_ASSERT_NOT_NULL(test, options);
	KUNIT_ASSERT_NOT_NULL(test, multicast);
	ip_hdr(fragment)->frag_off = htons(IP_MF);
	KUNIT_EXPECT_EQ(test, do_hnat_cpu_to_ge(fragment), (unsigned int)NF_STOLEN);
	ip_hdr(options)->ihl = 6;
	KUNIT_EXPECT_EQ(test, do_hnat_cpu_to_ge(options), (unsigned int)NF_STOLEN);
	eth_hdr(multicast)->h_dest[0] = 1;
	context->transmitted = NULL;
	KUNIT_EXPECT_EQ(test, do_hnat_cpu_to_ge(multicast), (unsigned int)NF_ACCEPT);
	KUNIT_EXPECT_PTR_EQ(test, context->transmitted, NULL);
}

static void truncated_header_bypasses(struct kunit *test)
{
	struct sk_buff *skb = packet(false);

	KUNIT_ASSERT_NOT_NULL(test, skb);
	skb_trim(skb, 8);
	KUNIT_EXPECT_EQ(test, do_hnat_cpu_to_ge(skb), (unsigned int)NF_ACCEPT);
}

static void priority_vlan_zero_preserved(struct kunit *test)
{
	struct sk_buff *skb = packet(false);
	u16 tci = 5 << VLAN_PRIO_SHIFT;
	struct vlan_ethhdr *veth;

	KUNIT_ASSERT_NOT_NULL(test, skb);
	__vlan_hwaccel_put_tag(skb, htons(ETH_P_8021Q), tci);
	KUNIT_ASSERT_EQ(test, do_hnat_cpu_to_ge(skb), (unsigned int)NF_STOLEN);
	veth = (struct vlan_ethhdr *)skb->data;
	KUNIT_EXPECT_EQ(test, veth->h_vlan_TCI, htons(tci));
	KUNIT_EXPECT_EQ(test, veth->h_vlan_encapsulated_proto, htons(ETH_P_IP));
	KUNIT_EXPECT_EQ(test, skb->protocol, htons(ETH_P_8021Q));
	KUNIT_EXPECT_EQ(test, skb_vlan_tag_get(skb), 1234);
	KUNIT_EXPECT_EQ(test, skb_network_offset(skb), VLAN_ETH_HLEN);
}

static void service_vlan_preserved(struct kunit *test)
{
	struct sk_buff *skb = packet(false);

	KUNIT_ASSERT_NOT_NULL(test, skb);
	__vlan_hwaccel_put_tag(skb, htons(ETH_P_8021AD), 123);
	KUNIT_ASSERT_EQ(test, do_hnat_cpu_to_ge(skb), (unsigned int)NF_STOLEN);
	KUNIT_EXPECT_EQ(test, skb->protocol, htons(ETH_P_8021AD));
	KUNIT_EXPECT_EQ(test, ((struct vlan_ethhdr *)skb->data)->h_vlan_TCI, htons(123));
}

static void payload_clone_avoids_cow_and_retains_tso_sg(struct kunit *test)
{
	struct sk_buff *parent = packet(false), *clone;
	struct page *page;
	unsigned char metadata[FOE_INFO_LEN];

	KUNIT_ASSERT_NOT_NULL(test, parent);
	page = alloc_page(GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, page);
	skb_add_rx_frag(parent, 0, page, 0, 4096, PAGE_SIZE);
	skb_shinfo(parent)->gso_size = 1448;
	skb_shinfo(parent)->gso_type = SKB_GSO_TCPV4;
	skb_shinfo(parent)->gso_segs = 3;
	parent->ip_summed = CHECKSUM_PARTIAL;
	__skb_header_release(parent);
	clone = skb_clone(parent, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, clone);
	own(clone);
	KUNIT_ASSERT_TRUE(test, skb_cloned(clone));
	KUNIT_ASSERT_FALSE(test, skb_header_cloned(clone));
	memcpy(metadata, parent->head, sizeof(metadata));
	KUNIT_ASSERT_EQ(test, do_hnat_cpu_to_ge(clone), (unsigned int)NF_STOLEN);
	KUNIT_EXPECT_PTR_EQ(test, parent->head, clone->head);
	KUNIT_EXPECT_EQ(test, memcmp(metadata, parent->head, sizeof(metadata)), 0);
	KUNIT_EXPECT_EQ(test, skb_shinfo(clone)->gso_size, 1448);
	KUNIT_EXPECT_EQ(test, skb_shinfo(clone)->nr_frags, 1);
	KUNIT_EXPECT_PTR_EQ(test, skb_frag_page(&skb_shinfo(clone)->frags[0]), page);
	KUNIT_EXPECT_EQ(test, (unsigned int)clone->ip_summed,
			(unsigned int)CHECKSUM_PARTIAL);
}

static void redirect_survives_control_block_reuse(struct kunit *test)
{
	struct sk_buff *skb = packet(false);

	KUNIT_ASSERT_NOT_NULL(test, skb);
	HNAT_SKB_CB2(skb)->magic = 0x78681415;
	SKB_GSO_CB(skb)->csum_start = 44;
	KUNIT_EXPECT_NE(test, HNAT_SKB_CB2(skb)->magic, 0x78681415U);
	set_to_ppe(skb);
	memset(skb->cb, 0, sizeof(skb->cb));
	KUNIT_EXPECT_TRUE(test, is_to_ppe(skb));
}

static void software_gso_keeps_ppe_destination(struct kunit *test)
{
	struct sk_buff *skb = packet(false), *segs, *seg;
	struct page *page;
	unsigned int count = 0;

	KUNIT_ASSERT_NOT_NULL(test, skb);
	page = alloc_page(GFP_KERNEL | __GFP_ZERO);
	KUNIT_ASSERT_NOT_NULL(test, page);
	skb_add_rx_frag(skb, 0, page, 0, 4096, PAGE_SIZE);
	ip_hdr(skb)->tot_len = htons(skb->len);
	skb_shinfo(skb)->gso_size = 1448;
	skb_shinfo(skb)->gso_type = SKB_GSO_TCPV4;
	skb_shinfo(skb)->gso_segs = 3;
	skb->ip_summed = CHECKSUM_PARTIAL;
	skb->csum_start = skb_transport_header(skb) - skb->head;
	skb->csum_offset = offsetof(struct tcphdr, check);
	tcp_hdr(skb)->check = ~tcp_v4_check(skb->len - sizeof(struct iphdr),
					 ip_hdr(skb)->saddr, ip_hdr(skb)->daddr, 0);
	KUNIT_ASSERT_EQ(test, do_hnat_cpu_to_ge(skb), (unsigned int)NF_STOLEN);
	segs = skb_gso_segment(skb, NETIF_F_SG | NETIF_F_HW_CSUM);
	KUNIT_ASSERT_FALSE(test, IS_ERR_OR_NULL(segs));
	for (seg = segs; seg; seg = seg->next) {
		own(seg);
		KUNIT_EXPECT_TRUE(test, is_to_ppe(seg));
		KUNIT_EXPECT_FALSE(test, skb_is_gso(seg));
		count++;
	}
	KUNIT_EXPECT_EQ(test, count, 3U);
}

static void vlan_failure_keeps_ownership(struct kunit *test)
{
	struct sk_buff *skb = packet(false);

	KUNIT_ASSERT_NOT_NULL(test, skb);
	__vlan_hwaccel_put_tag(skb, htons(ETH_P_8021Q), 99);
	context->fail_vlan = true;
	KUNIT_EXPECT_EQ(test, do_hnat_cpu_to_ge(skb), (unsigned int)NF_DROP);
	KUNIT_EXPECT_EQ(test, refcount_read(&skb->users), 1);
	KUNIT_EXPECT_PTR_EQ(test, context->transmitted, NULL);
}

static void xmit_drop_is_still_consumed(struct kunit *test)
{
	struct sk_buff *skb = packet(false);

	KUNIT_ASSERT_NOT_NULL(test, skb);
	context->xmit_result = NET_XMIT_DROP;
	KUNIT_EXPECT_EQ(test, do_hnat_cpu_to_ge(skb), (unsigned int)NF_STOLEN);
	KUNIT_EXPECT_PTR_EQ(test, context->transmitted, skb);
}

static void no_fdb_copy_and_scrub(struct kunit *test)
{
	struct sk_buff *skb = packet(false), *copy;

	KUNIT_ASSERT_NOT_NULL(test, skb);
	skb->offload_no_fdb = 1;
	skb->offload_to_ppe = 1;
	skb->mark = 7;
	copy = skb_copy(skb, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, copy);
	own(copy);
	KUNIT_EXPECT_TRUE(test, copy->offload_no_fdb);
	KUNIT_EXPECT_TRUE(test, copy->offload_to_ppe);
	skb_scrub_packet(copy, false);
	KUNIT_EXPECT_FALSE(test, copy->offload_no_fdb);
	KUNIT_EXPECT_FALSE(test, copy->offload_to_ppe);
	KUNIT_EXPECT_EQ(test, copy->mark, 7U);
}

static void gro_separates_ingress_provenance(struct kunit *test)
{
	struct sk_buff *a = packet(false), *b = packet(false);
	LIST_HEAD(head);

	KUNIT_ASSERT_NOT_NULL(test, a);
	KUNIT_ASSERT_NOT_NULL(test, b);
	list_add(&a->list, &head);
	b->offload_no_fdb = 1;
	gro_list_prepare(&head, b);
	KUNIT_EXPECT_FALSE(test, NAPI_GRO_CB(a)->same_flow);
	a->offload_no_fdb = 1;
	gro_list_prepare(&head, b);
	KUNIT_EXPECT_TRUE(test, NAPI_GRO_CB(a)->same_flow);
	list_del_init(&a->list);
}

static void sg_descriptors_keep_ppe_destination(struct kunit *test)
{
	struct mtk_tx_dma_v2 desc;
	struct mtk_tx_dma_desc_info info = {
		.addr = 0x10000, .size = 4096, .qid = 3, .to_ppe = true,
		.gso = true, .csum = true,
	};
	int i;

	for (i = 0; i < 4; i++) {
		memset(&desc, 0, sizeof(desc));
		info.first = i == 0;
		info.last = i == 3;
		mtk_tx_set_dma_desc_v2(context->dev, &desc, &info);
		KUNIT_EXPECT_EQ(test, (desc.txd4 >> TX_DMA_FPORT_SHIFT_V2) & 0xf,
				(u32)PSE_PPE0_PORT);
		KUNIT_EXPECT_EQ(test, desc.txd4 & QID_BITS_V2(0x3f), QID_BITS_V2(3));
		KUNIT_EXPECT_EQ(test, !!(desc.txd5 & TX_DMA_TSO_V2), i == 0);
	}
	info.to_ppe = false;
	mtk_tx_set_dma_desc_v2(context->dev, &desc, &info);
	KUNIT_EXPECT_EQ(test, (desc.txd4 >> TX_DMA_FPORT_SHIFT_V2) & 0xf,
			(u32)PSE_GDM1_PORT);
}

static void netsys_v1_descriptor_port(struct kunit *test)
{
	struct mtk_tx_dma desc = {};
	struct mtk_tx_dma_desc_info info = { .size = 100, .to_ppe = true };

	mtk_tx_set_dma_desc_v1(context->dev, &desc, &info);
	KUNIT_EXPECT_EQ(test, (desc.txd4 >> TX_DMA_FPORT_SHIFT) & 0x7, 4U);
	info.to_ppe = false;
	mtk_tx_set_dma_desc_v1(context->dev, &desc, &info);
	KUNIT_EXPECT_EQ(test, (desc.txd4 >> TX_DMA_FPORT_SHIFT) & 0x7, 1U);
}

static struct sk_buff *fdb_message(int mac_len, int family)
{
	struct sk_buff *skb = alloc_skb(512, GFP_KERNEL);
	struct nlmsghdr *nlh;
	struct ndmsg *ndm;
	u8 mac[ETH_ALEN] = { 2, 4, 6, 8, 10, 12 };

	if (!skb)
		return NULL;
	own(skb);
	nlh = nlmsg_put(skb, 0, 0, RTM_NEWNEIGH, sizeof(*ndm), 0);
	ndm = nlmsg_data(nlh);
	memset(ndm, 0, sizeof(*ndm));
	ndm->ndm_family = family;
	if (nla_put(skb, NDA_LLADDR, mac_len, mac))
		return NULL;
	nlmsg_end(skb, nlh);
	return skb;
}

static void roaming_minimal_fdb_message(struct kunit *test)
{
	struct sk_buff *skb = fdb_message(ETH_ALEN, PF_BRIDGE);
	u8 expected[ETH_ALEN] = { 2, 4, 6, 8, 10, 12 };

	KUNIT_ASSERT_NOT_NULL(test, skb);
	hnat_roaming_notify(skb->data, skb->len);
	KUNIT_EXPECT_EQ(test, atomic_read(&context->mac_calls), 1);
	KUNIT_EXPECT_EQ(test, memcmp(context->last_mac, expected, ETH_ALEN), 0);
}

static void roaming_rejects_short_mac(struct kunit *test)
{
	int len;

	for (len = 0; len < ETH_ALEN; len++) {
		struct sk_buff *skb = fdb_message(len, PF_BRIDGE);

		KUNIT_ASSERT_NOT_NULL(test, skb);
		hnat_roaming_notify(skb->data, skb->len);
	}
	KUNIT_EXPECT_EQ(test, atomic_read(&context->mac_calls), 0);
}

static void roaming_rejects_truncated_headers(struct kunit *test)
{
	struct sk_buff *skb = fdb_message(ETH_ALEN, PF_BRIDGE);
	struct nlmsghdr *nlh;
	int len;

	KUNIT_ASSERT_NOT_NULL(test, skb);
	nlh = (struct nlmsghdr *)skb->data;
	for (len = 0; len < skb->len; len++)
		hnat_roaming_notify(skb->data, len);
	for (len = NLMSG_HDRLEN; len < NLMSG_LENGTH(sizeof(struct ndmsg)); len++) {
		nlh->nlmsg_len = len;
		hnat_roaming_notify(skb->data, len);
	}
	KUNIT_EXPECT_EQ(test, atomic_read(&context->mac_calls), 0);
}

static void roaming_ignores_non_bridge_and_delete(struct kunit *test)
{
	struct sk_buff *a = fdb_message(ETH_ALEN, AF_INET);
	struct sk_buff *b = fdb_message(ETH_ALEN, PF_BRIDGE);

	KUNIT_ASSERT_NOT_NULL(test, a);
	KUNIT_ASSERT_NOT_NULL(test, b);
	((struct nlmsghdr *)b->data)->nlmsg_type = RTM_DELNEIGH;
	hnat_roaming_notify(a->data, a->len);
	hnat_roaming_notify(b->data, b->len);
	KUNIT_EXPECT_EQ(test, atomic_read(&context->mac_calls), 0);
}

static void roaming_handles_multiple_messages(struct kunit *test)
{
	struct sk_buff *a = fdb_message(ETH_ALEN, PF_BRIDGE);
	struct sk_buff *b = fdb_message(ETH_ALEN, PF_BRIDGE);

	KUNIT_ASSERT_NOT_NULL(test, a);
	KUNIT_ASSERT_NOT_NULL(test, b);
	skb_put_data(a, b->data, b->len);
	hnat_roaming_notify(a->data, a->len);
	KUNIT_EXPECT_EQ(test, atomic_read(&context->mac_calls), 2);
}

static void roaming_empty_queue_stops_and_restarts(struct kunit *test)
{
	int i, wait;

	for (i = 0; i < 8; i++) {
		KUNIT_ASSERT_EQ(test, hnat_roaming_enable(), 0);
		KUNIT_ASSERT_NOT_NULL(test, _hnat_roam_sock);
		KUNIT_ASSERT_NOT_NULL(test, _hnat_roam_task);
		for (wait = 0; wait < 100; wait++) {
			if (waitqueue_active(sk_sleep(_hnat_roam_sock->sk)))
				break;
			msleep(1);
		}
		KUNIT_EXPECT_LT(test, wait, 100);
		hnat_roaming_disable();
		KUNIT_EXPECT_PTR_EQ(test, _hnat_roam_sock, NULL);
		KUNIT_EXPECT_PTR_EQ(test, _hnat_roam_task, NULL);
		KUNIT_EXPECT_EQ(test, context->socket_releases, (unsigned int)i + 1);
	}
	hnat_roaming_disable();
	KUNIT_EXPECT_EQ(test, context->socket_releases, 8U);
}

static void roaming_failure_lifetimes(struct kunit *test)
{
	context->fail_socket = true;
	KUNIT_EXPECT_EQ(test, hnat_roaming_enable(), -ENOMEM);
	KUNIT_EXPECT_EQ(test, context->socket_releases, 0U);
	context->fail_socket = false;
	context->fail_bind = true;
	KUNIT_EXPECT_EQ(test, hnat_roaming_enable(), -EADDRINUSE);
	KUNIT_EXPECT_EQ(test, context->socket_releases, 1U);
	context->fail_bind = false;
	context->fail_thread = true;
	KUNIT_EXPECT_EQ(test, hnat_roaming_enable(), -ENOMEM);
	KUNIT_EXPECT_EQ(test, context->socket_releases, 2U);
	KUNIT_EXPECT_PTR_EQ(test, _hnat_roam_sock, NULL);
	KUNIT_EXPECT_PTR_EQ(test, _hnat_roam_task, NULL);
	hnat_roaming_disable();
	KUNIT_EXPECT_EQ(test, context->socket_releases, 2U);
	context->fail_thread = false;
	KUNIT_ASSERT_EQ(test, hnat_roaming_enable(), 0);
	hnat_roaming_disable();
	KUNIT_EXPECT_EQ(test, context->socket_releases, 3U);
}

static struct kunit_case cases[] = {
	KUNIT_CASE(ipv4_hardware_path),
	KUNIT_CASE(ipv6_hardware_path),
	KUNIT_CASE(udp_hardware_path),
	KUNIT_CASE(bypass_is_unchanged),
	KUNIT_CASE(metadata_requires_real_provenance),
	KUNIT_CASE(insufficient_metadata_headroom),
	KUNIT_CASE(down_device_bypasses),
	KUNIT_CASE(vendor_packet_eligibility_is_preserved),
	KUNIT_CASE(truncated_header_bypasses),
	KUNIT_CASE(priority_vlan_zero_preserved),
	KUNIT_CASE(service_vlan_preserved),
	KUNIT_CASE(payload_clone_avoids_cow_and_retains_tso_sg),
	KUNIT_CASE(redirect_survives_control_block_reuse),
	KUNIT_CASE(software_gso_keeps_ppe_destination),
	KUNIT_CASE(vlan_failure_keeps_ownership),
	KUNIT_CASE(xmit_drop_is_still_consumed),
	KUNIT_CASE(no_fdb_copy_and_scrub),
	KUNIT_CASE(gro_separates_ingress_provenance),
	KUNIT_CASE(sg_descriptors_keep_ppe_destination),
	KUNIT_CASE(netsys_v1_descriptor_port),
	KUNIT_CASE(roaming_minimal_fdb_message),
	KUNIT_CASE(roaming_rejects_short_mac),
	KUNIT_CASE(roaming_rejects_truncated_headers),
	KUNIT_CASE(roaming_ignores_non_bridge_and_delete),
	KUNIT_CASE(roaming_handles_multiple_messages),
	KUNIT_CASE(roaming_empty_queue_stops_and_restarts),
	KUNIT_CASE(roaming_failure_lifetimes),
	{}
};

static struct kunit_suite suite = {
	.name = "hnat-audit", .init = setup, .exit = teardown, .test_cases = cases,
};
kunit_test_suite(suite);
MODULE_LICENSE("GPL");
