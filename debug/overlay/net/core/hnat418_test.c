// SPDX-License-Identifier: GPL-2.0-only
/* Included by hnat418.c only in the QEMU test configuration. */
#include <kunit/test.h>
#if IS_ENABLED(CONFIG_NF_NAT_MASQUERADE)
#include <net/netfilter/nf_conntrack.h>
#include <net/netfilter/nf_conntrack_helper.h>
#include <net/netfilter/nf_nat_masquerade.h>
#endif

static struct sk_buff *h418_test_skb(bool down, bool dsack)
{
	struct sk_buff *skb = alloc_skb(256, GFP_KERNEL);
	struct iphdr *ip;
	struct tcphdr *tcp;
	u8 *options;

	if (!skb)
		return NULL;
	skb_reserve(skb, 64);
	skb_reset_network_header(skb);
	ip = skb_put_zero(skb, sizeof(*ip));
	ip->version = 4;
	ip->ihl = 5;
	ip->protocol = IPPROTO_TCP;
	ip->ttl = 64;
	ip->id = htons(77);
	ip->saddr = htonl(down ? 0xc0a80301 : 0xc0a80389);
	ip->daddr = htonl(down ? 0xc0a80389 : 0xc0a80301);
	skb_set_transport_header(skb, sizeof(*ip));
	tcp = skb_put_zero(skb, sizeof(*tcp));
	tcp->doff = dsack ? 8 : 5;
	tcp->source = htons(down ? 5201 : 32000);
	tcp->dest = htons(down ? 32000 : 5201);
	tcp->seq = htonl(1001);
	tcp->ack_seq = htonl(101);
	tcp->ack = 1;
	if (dsack) {
		options = skb_put_zero(skb, 12);
		options[0] = options[1] = TCPOPT_NOP;
		options[2] = TCPOPT_SACK;
		options[3] = 10;
		put_unaligned_be32(1, options + 4);
		put_unaligned_be32(101, options + 8);
	} else {
		memset(skb_put(skb, 32), 0x5a, 32);
	}
	ip->tot_len = htons(skb->len);
	skb->protocol = htons(ETH_P_IP);
	return skb;
}

static struct h418_session *h418_test_arm(u8 capture)
{
	struct h418_session *s = kzalloc(sizeof(*s), GFP_KERNEL);
	unsigned int cpu;

	if (!s)
		return NULL;
	refcount_set(&s->refs, 1);
	raw_spin_lock_init(&s->freeze_lock);
	INIT_DELAYED_WORK(&s->freeze_work, h418_freeze_work);
	s->server = htonl(0xc0a80301);
	s->peer = htonl(0xc0a80389);
	s->port = 5201;
	s->hardware = true;
	s->capture = capture;
	s->cpus = nr_cpu_ids;
	s->started_ns = ktime_get_ns();
	s->deadline = jiffies + 30 * HZ;
	if (capture) {
		s->rings = kcalloc(s->cpus, sizeof(*s->rings), GFP_KERNEL);
		if (!s->rings)
			goto fail;
		for (cpu = 0; cpu < s->cpus; cpu++) {
			raw_spin_lock_init(&s->rings[cpu].lock);
			s->rings[cpu].records = kvcalloc(H418_SLOTS, sizeof(struct h418_record), GFP_KERNEL);
			if (!s->rings[cpu].records)
				goto fail;
		}
	}
	mutex_lock(&h418_mutex);
	rcu_assign_pointer(h418_current, s);
	static_branch_enable(&h418_session_key);
	if (capture)
		static_branch_enable(&h418_capture_key);
	mutex_unlock(&h418_mutex);
	return s;
fail:
	h418_put(s);
	return NULL;
}

static void h418_test_disarm(void)
{
	mutex_lock(&h418_mutex);
	h418_stop();
	mutex_unlock(&h418_mutex);
}

static void h418_decode_offsets(struct kunit *test)
{
	struct sk_buff *skb = h418_test_skb(true, false);
	struct h418_packet p;

	KUNIT_ASSERT_NOT_NULL(test, skb);
	KUNIT_EXPECT_TRUE(test, h418_decode(skb, &p));
	KUNIT_EXPECT_EQ(test, p.netoff, 0);
	skb_pull(skb, sizeof(struct iphdr));
	KUNIT_EXPECT_TRUE(test, h418_decode(skb, &p));
	KUNIT_EXPECT_EQ(test, p.netoff, -(int)sizeof(struct iphdr));
	KUNIT_EXPECT_EQ(test, ntohl(p.tcp.seq), 1001U);
	kfree_skb(skb);
}

static void h418_reject_truncated(struct kunit *test)
{
	struct sk_buff *skb = h418_test_skb(true, false);
	struct h418_packet p;

	KUNIT_ASSERT_NOT_NULL(test, skb);
	skb_trim(skb, 39);
	KUNIT_EXPECT_FALSE(test, h418_decode(skb, &p));
	skb_trim(skb, 19);
	KUNIT_EXPECT_FALSE(test, h418_decode(skb, &p));
	KUNIT_EXPECT_FALSE(test, h418_decode(NULL, &p));
	kfree_skb(skb);
}

static void h418_early_rx_offsets(struct kunit *test)
{
	struct sk_buff *skb = h418_test_skb(true, false);
	struct h418_packet p;
	u8 before[72];

	KUNIT_ASSERT_NOT_NULL(test, skb);
	memset(skb->head, 0xa5, skb_headroom(skb));
	skb->network_header = 0; /* Driver has not handed this skb to GRO. */
	memcpy(before, skb->data, sizeof(before));
	KUNIT_EXPECT_FALSE(test, h418_decode(skb, &p));
	KUNIT_ASSERT_TRUE(test, h418_decode_rx(skb, &p));
	KUNIT_EXPECT_EQ(test, p.netoff, 0);
	KUNIT_EXPECT_EQ(test, ntohl(p.tcp.seq), 1001U);
	KUNIT_EXPECT_EQ(test, skb->network_header, (u16)0);
	KUNIT_EXPECT_EQ(test, memcmp(before, skb->data, sizeof(before)), 0);
	kfree_skb(skb);
}

static void h418_early_rx_vlan(struct kunit *test)
{
	struct sk_buff *skb = h418_test_skb(true, false);
	struct h418_packet p;
	struct vlan_hdr *vlan;
	int depth;

	KUNIT_ASSERT_NOT_NULL(test, skb);
	skb->network_header = 0;
	for (depth = 1; depth <= 3; depth++) {
		vlan = (struct vlan_hdr *)skb_push(skb, VLAN_HLEN);
		vlan->h_vlan_TCI = htons(1234);
		vlan->h_vlan_encapsulated_proto = skb->protocol;
		skb->protocol = htons(ETH_P_8021Q);
		if (depth <= 2) {
			KUNIT_EXPECT_TRUE(test, h418_decode_rx(skb, &p));
			KUNIT_EXPECT_EQ(test, p.netoff, depth * VLAN_HLEN);
		} else {
			KUNIT_EXPECT_FALSE(test, h418_decode_rx(skb, &p));
		}
	}
	skb_trim(skb, 2);
	KUNIT_EXPECT_FALSE(test, h418_decode_rx(skb, &p));
	KUNIT_EXPECT_FALSE(test, h418_decode_rx(NULL, &p));
	kfree_skb(skb);
}

static void h418_early_rx_record(struct kunit *test)
{
	struct sk_buff *skb = h418_test_skb(true, false);
	struct h418_session *s = h418_test_arm(true);
	u64 total = 0;
	unsigned int cpu;

	KUNIT_ASSERT_NOT_NULL(test, skb);
	KUNIT_ASSERT_NOT_NULL(test, s);
	skb->network_header = 0;
	skb_put_zero(skb, 8); /* Padding must not inflate the recorded payload. */
	__h418_record(skb, H418_PPE_RX, 14, 0, 0);
	for (cpu = 0; cpu < s->cpus; cpu++) {
		struct h418_ring *ring = &s->rings[cpu];

		total += ring->count;
		if (ring->count)
			KUNIT_EXPECT_EQ(test, le32_to_cpu(ring->records[0].payload), 32U);
	}
	KUNIT_EXPECT_EQ(test, total, 1ULL);
	KUNIT_EXPECT_EQ(test, skb->network_header, (u16)0);
	h418_test_disarm();
	kfree_skb(skb);
}

static void h418_reject_fragments(struct kunit *test)
{
	struct sk_buff *skb = h418_test_skb(true, false);
	struct h418_packet p;

	KUNIT_ASSERT_NOT_NULL(test, skb);
	ip_hdr(skb)->frag_off = htons(IP_MF);
	KUNIT_EXPECT_FALSE(test, h418_decode(skb, &p));
	ip_hdr(skb)->frag_off = 0;
	ip_hdr(skb)->ihl = 4;
	KUNIT_EXPECT_FALSE(test, h418_decode(skb, &p));
	ip_hdr(skb)->ihl = 5;
	tcp_hdr(skb)->doff = 15;
	KUNIT_EXPECT_FALSE(test, h418_decode(skb, &p));
	kfree_skb(skb);
}

static void h418_dsack_parser(struct kunit *test)
{
	struct sk_buff *skb = h418_test_skb(false, true);
	struct h418_packet p;
	struct h418_record r = {};

	KUNIT_ASSERT_NOT_NULL(test, skb);
	KUNIT_ASSERT_TRUE(test, h418_decode(skb, &p));
	h418_sack(skb, &p, &r);
	KUNIT_EXPECT_EQ(test, r.sack_count, (u8)1);
	KUNIT_EXPECT_EQ(test, r.dsack, (u8)1);
	KUNIT_EXPECT_EQ(test, le32_to_cpu(r.sack[1]), 101U);
	memset(&r, 0, sizeof(r));
	put_unaligned_be32(201, skb->data + 44);
	put_unaligned_be32(301, skb->data + 48);
	h418_sack(skb, &p, &r);
	KUNIT_EXPECT_EQ(test, r.dsack, (u8)0);
	kfree_skb(skb);
}

static void h418_dsack_validation(struct kunit *test)
{
	struct sk_buff *skb = h418_test_skb(false, true);
	struct h418_packet p;
	struct h418_record r = {};
	u32 ranges[][3] = {
		{101, 101, 101}, /* empty */
		{100, 90, 101}, /* reversed */
		{90, 110, 101}, /* crosses cumulative ACK */
	};
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, skb);
	for (i = 0; i < ARRAY_SIZE(ranges); i++) {
		put_unaligned_be32(ranges[i][0], skb->data + 44);
		put_unaligned_be32(ranges[i][1], skb->data + 48);
		tcp_hdr(skb)->ack_seq = htonl(ranges[i][2]);
		KUNIT_ASSERT_TRUE(test, h418_decode(skb, &p));
		memset(&r, 0, sizeof(r));
		h418_sack(skb, &p, &r);
		KUNIT_EXPECT_EQ(test, r.dsack, (u8)0);
	}
	put_unaligned_be32(0xfffffff0, skb->data + 44);
	put_unaligned_be32(0, skb->data + 48);
	tcp_hdr(skb)->ack_seq = htonl(16);
	KUNIT_ASSERT_TRUE(test, h418_decode(skb, &p));
	memset(&r, 0, sizeof(r));
	h418_sack(skb, &p, &r);
	KUNIT_EXPECT_EQ(test, r.dsack, (u8)1);
	tcp_hdr(skb)->ack = 0;
	KUNIT_ASSERT_TRUE(test, h418_decode(skb, &p));
	memset(&r, 0, sizeof(r));
	h418_sack(skb, &p, &r);
	KUNIT_EXPECT_EQ(test, r.dsack, (u8)0);
	kfree_skb(skb);
}

static void h418_nested_dsack(struct kunit *test)
{
	struct sk_buff *skb = h418_test_skb(false, true);
	struct h418_packet p;
	struct h418_record r = {};

	KUNIT_ASSERT_NOT_NULL(test, skb);
	skb_put_zero(skb, 8);
	tcp_hdr(skb)->doff = 10;
	ip_hdr(skb)->tot_len = htons(skb->len);
	skb->data[43] = 18;
	put_unaligned_be32(200, skb->data + 44);
	put_unaligned_be32(210, skb->data + 48);
	put_unaligned_be32(190, skb->data + 52);
	put_unaligned_be32(300, skb->data + 56);
	KUNIT_ASSERT_TRUE(test, h418_decode(skb, &p));
	h418_sack(skb, &p, &r);
	KUNIT_EXPECT_EQ(test, r.dsack, (u8)1);
	put_unaligned_be32(300, skb->data + 52);
	put_unaligned_be32(190, skb->data + 56);
	memset(&r, 0, sizeof(r));
	h418_sack(skb, &p, &r);
	KUNIT_EXPECT_EQ(test, r.dsack, (u8)0);
	kfree_skb(skb);
}

static void h418_malformed_sack(struct kunit *test)
{
	struct sk_buff *skb = h418_test_skb(false, true);
	struct h418_packet p;
	struct h418_record r = {};

	KUNIT_ASSERT_NOT_NULL(test, skb);
	KUNIT_ASSERT_TRUE(test, h418_decode(skb, &p));
	skb->data[43] = 255;
	h418_sack(skb, &p, &r);
	KUNIT_EXPECT_EQ(test, r.sack_count, (u8)0);
	kfree_skb(skb);
}

static void h418_quiet_and_scoping(struct kunit *test)
{
	struct sk_buff *skb = h418_test_skb(true, false);
	struct h418_session *s = h418_test_arm(false);

	KUNIT_ASSERT_NOT_NULL(test, skb);
	KUNIT_ASSERT_NOT_NULL(test, s);
	KUNIT_EXPECT_TRUE(test, h418_hardware_test(skb));
	/* An old call that passed the static key must not touch a quiet ring. */
	__h418_record(skb, H418_IP_OUT, 0, 0, 0);
	tcp_hdr(skb)->source = htons(5202);
	KUNIT_EXPECT_FALSE(test, h418_hardware_test(skb));
	tcp_hdr(skb)->source = htons(5201);
	ip_hdr(skb)->daddr = htonl(0xc0a80390);
	KUNIT_EXPECT_FALSE(test, h418_hardware_test(skb));
	ip_hdr(skb)->daddr = s->peer;
	s->deadline = jiffies - 1;
	KUNIT_EXPECT_FALSE(test, h418_hardware_test(skb));
	h418_test_disarm();
	KUNIT_EXPECT_FALSE(test, h418_hardware_test(skb));
	kfree_skb(skb);
}

static void h418_record_read_only(struct kunit *test)
{
	struct sk_buff *skb = h418_test_skb(true, false);
	struct h418_session *s = h418_test_arm(true);
	u8 before[72];
	u64 total = 0;
	unsigned int cpu;

	KUNIT_ASSERT_NOT_NULL(test, skb);
	KUNIT_ASSERT_NOT_NULL(test, s);
	memcpy(before, skb->data, sizeof(before));
	__h418_record(skb, H418_IP_OUT, 1, 2, 3);
	for (cpu = 0; cpu < s->cpus; cpu++)
		total += s->rings[cpu].count;
	KUNIT_EXPECT_EQ(test, total, 1ULL);
	KUNIT_EXPECT_EQ(test, memcmp(before, skb->data, sizeof(before)), 0);
	WRITE_ONCE(s->frozen, true);
	__h418_record(skb, H418_IP_OUT, 1, 2, 3);
	total = 0;
	for (cpu = 0; cpu < s->cpus; cpu++)
		total += s->rings[cpu].count;
	KUNIT_EXPECT_EQ(test, total, 1ULL);
	h418_test_disarm();
	kfree_skb(skb);
}

static void h418_automatic_freeze(struct kunit *test)
{
	struct sk_buff *ack = h418_test_skb(false, true);
	struct sk_buff *data = h418_test_skb(true, false);
	struct h418_session *s = h418_test_arm(true);
	int i;

	KUNIT_ASSERT_NOT_NULL(test, ack);
	KUNIT_ASSERT_NOT_NULL(test, data);
	KUNIT_ASSERT_NOT_NULL(test, s);
	for (i = 0; i < 16; i++)
		__h418_record(ack, H418_TCP_ACK, 0, 0, 0);
	flush_delayed_work(&s->freeze_work);
	KUNIT_EXPECT_TRUE(test, s->frozen);
	KUNIT_EXPECT_EQ(test, s->freeze_reason, 1U);
	KUNIT_EXPECT_TRUE(test, h418_hardware_test(data));
	h418_test_disarm();
	kfree_skb(ack);
	kfree_skb(data);
}

static void h418_recovery_freeze(struct kunit *test)
{
	struct sk_buff *ack = h418_test_skb(false, true);
	struct sk_buff *data = h418_test_skb(true, false);
	struct h418_session *s = h418_test_arm(2);
	int i;

	KUNIT_ASSERT_NOT_NULL(test, ack);
	KUNIT_ASSERT_NOT_NULL(test, data);
	KUNIT_ASSERT_NOT_NULL(test, s);
	for (i = 0; i < 32; i++)
		__h418_record(ack, H418_TCP_ACK, 1, 0, i);
	KUNIT_EXPECT_EQ(test, atomic_read(&s->dsacks), 32);
	KUNIT_EXPECT_EQ(test, atomic_read(&s->trigger_reason), 0);
	KUNIT_EXPECT_FALSE(test, delayed_work_pending(&s->freeze_work));
	/* This ordinary ACK carries no SACK: socket recovery still triggers. */
	tcp_hdr(ack)->doff = 5;
	__h418_record(ack, H418_TCP_ACK, 32, 0, 32);
	KUNIT_EXPECT_EQ(test, atomic_read(&s->trigger_reason), 3);
	flush_delayed_work(&s->freeze_work);
	KUNIT_EXPECT_TRUE(test, s->frozen);
	KUNIT_EXPECT_EQ(test, s->freeze_reason, 3U);
	KUNIT_EXPECT_TRUE(test, h418_hardware_test(data));
	h418_test_disarm();
	kfree_skb(ack);
	kfree_skb(data);
}

static void h418_reordering_freeze(struct kunit *test)
{
	struct sk_buff *ack = h418_test_skb(false, true);
	struct h418_session *s = h418_test_arm(2);

	KUNIT_ASSERT_NOT_NULL(test, ack);
	KUNIT_ASSERT_NOT_NULL(test, s);
	__h418_record(ack, H418_TCP_ACK, 0, 1, 0);
	flush_delayed_work(&s->freeze_work);
	KUNIT_EXPECT_EQ(test, s->freeze_reason, 3U);
	h418_test_disarm();
	kfree_skb(ack);
}

static void h418_manual_freeze_immutable(struct kunit *test)
{
	struct h418_session *s = h418_test_arm(1);
	u64 frozen_ns;

	KUNIT_ASSERT_NOT_NULL(test, s);
	h418_freeze(s, 2);
	frozen_ns = s->frozen_ns;
	/* Simulate a writer queueing work after manual freeze publication. */
	h418_trigger(s, 1);
	flush_delayed_work(&s->freeze_work);
	h418_manual_freeze(s);
	KUNIT_EXPECT_EQ(test, s->freeze_reason, 2U);
	KUNIT_EXPECT_EQ(test, s->frozen_ns, frozen_ns);
	KUNIT_EXPECT_FALSE(test, delayed_work_pending(&s->freeze_work));
	h418_test_disarm();
}

static void h418_reader_survives_stop(struct kunit *test)
{
	struct h418_session *s = h418_test_arm(true);
	struct file file = {};

	KUNIT_ASSERT_NOT_NULL(test, s);
	KUNIT_EXPECT_EQ(test, h418_records_open(NULL, &file), -EAGAIN);
	WRITE_ONCE(s->frozen, true);
	KUNIT_ASSERT_EQ(test, h418_records_open(NULL, &file), 0);
	KUNIT_EXPECT_EQ(test, refcount_read(&s->refs), 2);
	h418_test_disarm();
	KUNIT_EXPECT_EQ(test, refcount_read(&s->refs), 1);
	KUNIT_EXPECT_EQ(test, h418_records_release(NULL, &file), 0);
	h418_test_disarm();
}

static void h418_binary_abi(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, sizeof(struct h418_record), (size_t)144);
	KUNIT_EXPECT_EQ(test, sizeof(struct h418_header), (size_t)64);
	KUNIT_EXPECT_EQ(test, sizeof(struct h418_cpu_header), (size_t)32);
	KUNIT_EXPECT_EQ(test, offsetof(struct h418_record, sack), (size_t)88);
	KUNIT_EXPECT_EQ(test, offsetof(struct h418_record, stage), (size_t)136);
}

#if IS_ENABLED(CONFIG_NF_NAT_MASQUERADE)
/* Call the actual patched netfilter function, not a copy of its guard.
 * A confirmed connection must return without even needing route/device
 * context.  This is a lifetime contract fixture, not a concurrent NAT test.
 */
static void h418_confirmed_nat_case(struct kunit *test, bool with_helper)
{
	struct nf_conn *ct = kunit_kzalloc(test, sizeof(*ct), GFP_KERNEL);
	struct nf_conn saved;
	struct nf_nat_range2 range = {};
	struct nf_conn_help *help = NULL;
	struct sk_buff *skb;
	unsigned int verdict;

	KUNIT_ASSERT_NOT_NULL(test, ct);
	skb = alloc_skb(128, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, skb);
	if (with_helper) {
		help = nf_ct_helper_ext_add(ct, GFP_KERNEL);
		if (!help) {
			kfree_skb(skb);
			KUNIT_FAIL(test, "cannot allocate helper fixture");
			return;
		}
	}
	ct->tuplehash[IP_CT_DIR_ORIGINAL].tuple.src.u3.ip = htonl(0xc0000201);
	ct->tuplehash[IP_CT_DIR_ORIGINAL].tuple.dst.protonum = IPPROTO_UDP;
	set_bit(IPS_CONFIRMED_BIT, &ct->status);
	memcpy(&saved, ct, sizeof(saved));
	nf_ct_set(skb, ct, IP_CT_NEW);
	verdict = nf_nat_masquerade_ipv4(skb, NF_INET_POST_ROUTING, &range, NULL);
	KUNIT_EXPECT_EQ(test, verdict, (unsigned int)NF_ACCEPT);
	KUNIT_EXPECT_EQ(test, memcmp(ct, &saved, sizeof(saved)), 0);
	if (help)
		KUNIT_EXPECT_PTR_EQ(test, rcu_access_pointer(help->helper), NULL);
	/* This fixture never acquired a conntrack reference or inserted a hash
	 * entry.  Detach it before freeing the skb; free its extension directly.
	 */
	nf_ct_set(skb, NULL, 0);
	kfree_skb(skb);
	kfree(ct->ext);
	ct->ext = NULL;
}

static void h418_confirmed_nat_without_extension(struct kunit *test)
{
	h418_confirmed_nat_case(test, false);
}

static void h418_confirmed_nat_with_extension(struct kunit *test)
{
	h418_confirmed_nat_case(test, true);
}
#endif

static struct kunit_case h418_cases[] = {
	KUNIT_CASE(h418_decode_offsets),
	KUNIT_CASE(h418_reject_truncated),
	KUNIT_CASE(h418_early_rx_offsets),
	KUNIT_CASE(h418_early_rx_vlan),
	KUNIT_CASE(h418_early_rx_record),
	KUNIT_CASE(h418_reject_fragments),
	KUNIT_CASE(h418_dsack_parser),
	KUNIT_CASE(h418_dsack_validation),
	KUNIT_CASE(h418_nested_dsack),
	KUNIT_CASE(h418_malformed_sack),
	KUNIT_CASE(h418_quiet_and_scoping),
	KUNIT_CASE(h418_record_read_only),
	KUNIT_CASE(h418_automatic_freeze),
	KUNIT_CASE(h418_recovery_freeze),
	KUNIT_CASE(h418_reordering_freeze),
	KUNIT_CASE(h418_manual_freeze_immutable),
	KUNIT_CASE(h418_reader_survives_stop),
	KUNIT_CASE(h418_binary_abi),
#if IS_ENABLED(CONFIG_NF_NAT_MASQUERADE)
	KUNIT_CASE(h418_confirmed_nat_without_extension),
	KUNIT_CASE(h418_confirmed_nat_with_extension),
#endif
	{}
};
static struct kunit_suite h418_suite = {
	.name = "hnat418-debug", .test_cases = h418_cases,
};
kunit_test_suite(h418_suite);
