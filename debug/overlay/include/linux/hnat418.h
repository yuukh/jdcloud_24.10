/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_HNAT418_H
#define _LINUX_HNAT418_H

#include <linux/jump_label.h>
#include <linux/skbuff.h>

/* The binary record is versioned. Never infer wire delivery from DMA_RELEASE. */
enum h418_stage {
	H418_IP_OUT = 1,
	H418_BR_DECISION,
	H418_CPU_PREP,
	H418_CPU_INJECT,
	H418_DEV_XMIT,
	H418_QDMA_MAP,
	H418_DMA_RELEASE,
	H418_PPE_RX,
	H418_FOE_LOOKUP,
	H418_WIFI_BIND,
	H418_TCP_ACK,
	H418_PREP_DROP,
	H418_QDMA_BUSY,
	H418_QDMA_ERROR,
	H418_STAGE_MAX,
};

#if IS_ENABLED(CONFIG_HNAT418_DEBUG)
DECLARE_STATIC_KEY_FALSE(h418_capture_key);
DECLARE_STATIC_KEY_FALSE(h418_session_key);
void __h418_record(const struct sk_buff *skb, unsigned int stage,
		   u32 a, u32 b, u32 c);
void __h418_tcp_ack(const struct sock *sk, const struct sk_buff *skb);
bool __h418_hardware_test(const struct sk_buff *skb);

static inline void h418_record(const struct sk_buff *skb, unsigned int stage,
			       u32 a, u32 b, u32 c)
{
	if (static_branch_unlikely(&h418_capture_key))
		__h418_record(skb, stage, a, b, c);
}

static inline void h418_tcp_ack(const struct sock *sk, const struct sk_buff *skb)
{
	if (static_branch_unlikely(&h418_capture_key))
		__h418_tcp_ack(sk, skb);
}

/* Outside the explicitly armed IPv4 test flow, keep main's local bypass. */
static inline bool h418_hardware_test(const struct sk_buff *skb)
{
	if (static_branch_unlikely(&h418_session_key))
		return __h418_hardware_test(skb);
	return false;
}
#else
static inline void h418_record(const struct sk_buff *skb, unsigned int stage,
			       u32 a, u32 b, u32 c) {}
static inline void h418_tcp_ack(const struct sock *sk, const struct sk_buff *skb) {}
static inline bool h418_hardware_test(const struct sk_buff *skb)
{
	return false;
}
#endif
#endif
