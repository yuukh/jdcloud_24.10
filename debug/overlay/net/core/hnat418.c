// SPDX-License-Identifier: GPL-2.0-only
/* Bounded, read-only skb observation. No hot-path allocation or printk.
 * Only the root-controlled test selector changes forwarding eligibility;
 * tracing, snapshotting and reading never modify a packet or a descriptor.
 */
#include <linux/debugfs.h>
#include <linux/hnat418.h>
#include <linux/hash.h>
#include <linux/inet.h>
#include <linux/ip.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/refcount.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/tcp.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
#include <linux/workqueue.h>
#include <asm/unaligned.h>
#include <net/ip.h>
#include <net/tcp.h>

#define H418_SLOTS 16384U
#define H418_MAX_CPUS 16
#define H418_VERSION 1

/* Fixed LE64/LE32/LE16 wire layout; payload and raw kernel addresses omitted. */
struct h418_record {
	__le64 ns, serial;
	__le32 cookie, seq, ack, payload, skb_len, mark, cb44;
	__le32 meta[3];
	__le32 a, b, c, ifindex, iif, gso_type;
	__be32 src, dst;
	__le32 sack[8];
	__le16 sport, dport, ip_id, gso_size, gso_segs, queue;
	__le16 headroom, netoff;
	u8 stage, flags, ttl, checksum, to_ppe, no_fdb, sack_count, dsack;
};

struct h418_header {
	u8 magic[8];
	__le32 version, record_size, slots, cpus;
	__le64 started_ns, frozen_ns;
	__be32 server, peer;
	__le32 port, hardware, dsacks, reason;
};

struct h418_cpu_header {
	__le32 cpu, reserved;
	__le64 count, overwritten, reserved2;
};

struct h418_ring {
	raw_spinlock_t lock;
	u64 count;
	struct h418_record *records;
};

struct h418_session {
	refcount_t refs;
	__be32 server, peer;
	u16 port;
	bool hardware, capture, frozen;
	u32 freeze_reason;
	unsigned long deadline;
	u64 started_ns, frozen_ns;
	atomic_t dsacks;
	struct delayed_work freeze_work;
	struct h418_ring *rings;
	unsigned int cpus;
};

struct h418_packet {
	struct iphdr ip;
	struct tcphdr tcp;
	int netoff, tcpoff, header_len;
	bool down;
};

DEFINE_STATIC_KEY_FALSE(h418_capture_key);
DEFINE_STATIC_KEY_FALSE(h418_session_key);
EXPORT_SYMBOL_GPL(h418_capture_key);
EXPORT_SYMBOL_GPL(h418_session_key);
static DEFINE_MUTEX(h418_mutex);
static struct h418_session __rcu *h418_current;

static bool h418_decode(const struct sk_buff *skb, struct h418_packet *p)
{
	const struct iphdr *ip;
	const struct tcphdr *tcp;
	int offset;

	if (!skb)
		return false;
	offset = skb_network_offset(skb);
	/* A receive skb can have its IP header immediately before skb->data. */
	if (offset < -(int)skb_headroom(skb) ||
	    offset > (int)skb_headlen(skb) - (int)sizeof(p->ip))
		return false;
	ip = skb_header_pointer(skb, offset, sizeof(p->ip), &p->ip);
	if (!ip || ip->version != 4 || ip->ihl < 5 ||
	    ip->protocol != IPPROTO_TCP || ip_is_fragment(ip))
		return false;
	if (ip != &p->ip)
		p->ip = *ip;
	p->netoff = offset;
	p->tcpoff = offset + p->ip.ihl * 4;
	if (p->tcpoff > (int)skb->len - (int)sizeof(p->tcp))
		return false;
	tcp = skb_header_pointer(skb, p->tcpoff, sizeof(p->tcp), &p->tcp);
	if (!tcp || tcp->doff < 5)
		return false;
	if (tcp != &p->tcp)
		p->tcp = *tcp;
	p->header_len = p->ip.ihl * 4 + p->tcp.doff * 4;
	return p->netoff + p->header_len <= (int)skb->len;
}

static bool h418_match(const struct h418_session *s, struct h418_packet *p)
{
	p->down = p->ip.saddr == s->server && p->ip.daddr == s->peer &&
		  ntohs(p->tcp.source) == s->port;
	return p->down || (p->ip.saddr == s->peer && p->ip.daddr == s->server &&
			   ntohs(p->tcp.dest) == s->port);
}

bool __h418_hardware_test(const struct sk_buff *skb)
{
	struct h418_session *s;
	struct h418_packet p;
	bool result = false;

	rcu_read_lock();
	s = rcu_dereference(h418_current);
	if (s && s->hardware && time_before(jiffies, s->deadline) &&
	    h418_decode(skb, &p) && h418_match(s, &p) && p.down)
		result = true;
	rcu_read_unlock();
	return result;
}
EXPORT_SYMBOL_GPL(__h418_hardware_test);

static void h418_sack(const struct sk_buff *skb, const struct h418_packet *p,
		      struct h418_record *r)
{
	u8 buf[40];
	const u8 *options;
	int length = p->tcp.doff * 4 - sizeof(p->tcp), i = 0, j;
	u32 left, right, ack = ntohl(p->tcp.ack_seq);

	if (!length)
		return;
	options = skb_header_pointer(skb, p->tcpoff + sizeof(p->tcp), length, buf);
	if (!options)
		return;
	while (i < length) {
		u8 kind = options[i], size;

		if (!kind)
			break;
		if (kind == TCPOPT_NOP) {
			i++;
			continue;
		}
		if (i + 2 > length)
			break;
		size = options[i + 1];
		if (size < 2 || i + size > length)
			break;
		if (kind == TCPOPT_SACK && size >= 10 && !((size - 2) % 8)) {
			r->sack_count = min_t(unsigned int, (size - 2) / 8, 4);
			for (j = 0; j < r->sack_count * 2; j++)
				r->sack[j] = cpu_to_le32(get_unaligned_be32(options + i + 2 + j * 4));
			left = le32_to_cpu(r->sack[0]);
			right = le32_to_cpu(r->sack[1]);
			r->dsack = !after(right, ack) ||
				(r->sack_count > 1 &&
				 !before(left, le32_to_cpu(r->sack[2])) &&
				 !after(right, le32_to_cpu(r->sack[3])));
			break;
		}
		i += size;
	}
}

static void h418_freeze_work(struct work_struct *work)
{
	struct h418_session *s = container_of(to_delayed_work(work),
					     struct h418_session, freeze_work);

	WRITE_ONCE(s->frozen_ns, ktime_get_ns());
	WRITE_ONCE(s->freeze_reason, 1); /* Sixteen DSACK ACKs, then 100 ms. */
	smp_store_release(&s->frozen, true);
}

void __h418_record(const struct sk_buff *skb, unsigned int stage,
		   u32 a, u32 b, u32 c)
{
	struct h418_record r = {};
	struct h418_session *s;
	struct h418_packet p;
	struct h418_ring *ring;
	unsigned long irqflags;
	unsigned int cpu;

	rcu_read_lock();
	s = rcu_dereference(h418_current);
	if (!s || !s->capture || smp_load_acquire(&s->frozen) ||
	    time_after_eq(jiffies, s->deadline) || !h418_decode(skb, &p) ||
	    !h418_match(s, &p))
		goto out;
	/* Only the TCP receive boundary observes upstream ACK/SACK once. */
	if (!p.down && stage != H418_TCP_ACK)
		goto out;
	if (stage == H418_TCP_ACK) {
		if (p.down)
			goto out;
		h418_sack(skb, &p, &r);
		if (!r.sack_count && !p.tcp.syn && !p.tcp.fin && !p.tcp.rst)
			goto out;
	}
	r.cookie = cpu_to_le32(hash_ptr(skb, 32));
	r.seq = cpu_to_le32(ntohl(p.tcp.seq));
	r.ack = cpu_to_le32(ntohl(p.tcp.ack_seq));
	r.payload = cpu_to_le32(skb->len - p.netoff - p.header_len);
	r.skb_len = cpu_to_le32(skb->len);
	r.mark = cpu_to_le32(skb->mark);
	r.cb44 = cpu_to_le32(get_unaligned((u32 *)&skb->cb[44]));
	if (skb_headroom(skb) >= 13) {
		r.meta[0] = cpu_to_le32(get_unaligned((u32 *)skb->head));
		r.meta[1] = cpu_to_le32(get_unaligned((u32 *)(skb->head + 4)));
		r.meta[2] = cpu_to_le32(get_unaligned((u32 *)(skb->head + 8)));
	}
	r.a = cpu_to_le32(a);
	r.b = cpu_to_le32(b);
	r.c = cpu_to_le32(c);
	r.ifindex = cpu_to_le32(skb->dev ? skb->dev->ifindex : 0);
	r.iif = cpu_to_le32(skb->skb_iif);
	r.gso_type = cpu_to_le32(skb_shinfo(skb)->gso_type);
	r.src = p.ip.saddr;
	r.dst = p.ip.daddr;
	r.sport = cpu_to_le16(ntohs(p.tcp.source));
	r.dport = cpu_to_le16(ntohs(p.tcp.dest));
	r.ip_id = cpu_to_le16(ntohs(p.ip.id));
	r.gso_size = cpu_to_le16(skb_shinfo(skb)->gso_size);
	r.gso_segs = cpu_to_le16(skb_shinfo(skb)->gso_segs);
	r.queue = cpu_to_le16(skb_get_queue_mapping(skb));
	r.headroom = cpu_to_le16(min_t(unsigned int, skb_headroom(skb), U16_MAX));
	r.netoff = cpu_to_le16((s16)p.netoff);
	r.stage = stage;
	r.flags = ((const u8 *)&p.tcp)[13];
	r.ttl = p.ip.ttl;
	r.checksum = skb->ip_summed;
	r.to_ppe = skb->offload_to_ppe;
	r.no_fdb = skb->offload_no_fdb;
	cpu = get_cpu();
	ring = &s->rings[cpu];
	raw_spin_lock_irqsave(&ring->lock, irqflags);
	r.ns = cpu_to_le64(ktime_get_ns());
	r.serial = cpu_to_le64(ring->count + 1);
	ring->records[ring->count % H418_SLOTS] = r;
	ring->count++;
	raw_spin_unlock_irqrestore(&ring->lock, irqflags);
	put_cpu();
	if (r.dsack && atomic_inc_return(&s->dsacks) == 16)
		schedule_delayed_work(&s->freeze_work, msecs_to_jiffies(100));
out:
	rcu_read_unlock();
}
EXPORT_SYMBOL_GPL(__h418_record);

static void h418_put(struct h418_session *s)
{
	unsigned int cpu;

	if (!s || !refcount_dec_and_test(&s->refs))
		return;
	if (s->rings) {
		for (cpu = 0; cpu < s->cpus; cpu++)
			kvfree(s->rings[cpu].records);
		kfree(s->rings);
	}
	kfree(s);
}

static void h418_stop(void)
{
	struct h418_session *s = rcu_dereference_protected(h418_current,
						 lockdep_is_held(&h418_mutex));

	if (!s)
		return;
	if (s->capture)
		static_branch_disable(&h418_capture_key);
	static_branch_disable(&h418_session_key);
	rcu_assign_pointer(h418_current, NULL);
	synchronize_rcu();
	cancel_delayed_work_sync(&s->freeze_work);
	h418_put(s);
}

static ssize_t h418_control_write(struct file *file, const char __user *user,
				 size_t len, loff_t *offset)
{
	char buf[128], server[16], peer[16], extra;
	unsigned int port, hardware, capture, seconds, cpu;
	struct h418_session *s;
	ssize_t ret = len;

	if (!capable(CAP_NET_ADMIN))
		return -EPERM;
	if (!len || len >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, user, len))
		return -EFAULT;
	buf[len] = 0;
	strim(buf);
	mutex_lock(&h418_mutex);
	s = rcu_dereference_protected(h418_current, lockdep_is_held(&h418_mutex));
	if (!strcmp(buf, "stop")) {
		h418_stop();
		goto out;
	}
	if (!strcmp(buf, "freeze")) {
		if (!s) {
			ret = -ENOENT;
			goto out;
		}
		cancel_delayed_work_sync(&s->freeze_work);
		if (!s->frozen) {
			s->frozen_ns = ktime_get_ns();
			s->freeze_reason = 2;
			smp_store_release(&s->frozen, true);
		}
		synchronize_rcu();
		goto out;
	}
	if (sscanf(buf, "arm %15s %15s %u %u %u %u %c", server, peer,
		   &port, &hardware, &capture, &seconds, &extra) != 6 ||
	    port < 5201 || port > 5204 || hardware > 1 || capture > 1 ||
	    seconds < 10 || seconds > 300 || nr_cpu_ids > H418_MAX_CPUS) {
		ret = -EINVAL;
		goto out;
	}
	/* Refuse an in-place mode change while a flow could still be active. */
	if (s) {
		ret = -EBUSY;
		goto out;
	}
	s = kzalloc(sizeof(*s), GFP_KERNEL);
	if (!s) {
		ret = -ENOMEM;
		goto out;
	}
	refcount_set(&s->refs, 1);
	INIT_DELAYED_WORK(&s->freeze_work, h418_freeze_work);
	if (!in4_pton(server, -1, (u8 *)&s->server, -1, NULL) ||
	    !in4_pton(peer, -1, (u8 *)&s->peer, -1, NULL) ||
	    !s->server || !s->peer || s->server == s->peer ||
	    ipv4_is_multicast(s->server) || ipv4_is_multicast(s->peer)) {
		ret = -EINVAL;
		goto fail;
	}
	s->port = port;
	s->hardware = hardware;
	s->capture = capture;
	s->cpus = nr_cpu_ids;
	s->deadline = jiffies + seconds * HZ;
	s->started_ns = ktime_get_ns();
	if (capture) {
		s->rings = kcalloc(s->cpus, sizeof(*s->rings), GFP_KERNEL);
		if (!s->rings) {
			ret = -ENOMEM;
			goto fail;
		}
		for (cpu = 0; cpu < s->cpus; cpu++) {
			raw_spin_lock_init(&s->rings[cpu].lock);
			s->rings[cpu].records = kvcalloc(H418_SLOTS, sizeof(struct h418_record), GFP_KERNEL);
			if (!s->rings[cpu].records) {
				ret = -ENOMEM;
				goto fail;
			}
		}
	}
	rcu_assign_pointer(h418_current, s);
	static_branch_enable(&h418_session_key);
	if (capture)
		static_branch_enable(&h418_capture_key);
	goto out;
fail:
	h418_put(s);
out:
	mutex_unlock(&h418_mutex);
	return ret;
}

static int h418_status_show(struct seq_file *m, void *v)
{
	struct h418_session *s;
	unsigned int cpu;

	mutex_lock(&h418_mutex);
	s = rcu_dereference_protected(h418_current, lockdep_is_held(&h418_mutex));
	seq_printf(m, "format=%u record_size=%zu slots=%u\n", H418_VERSION,
		   sizeof(struct h418_record), H418_SLOTS);
	if (!s) {
		seq_puts(m, "active=0\n");
	} else {
		seq_printf(m, "active=1 server=%pI4 peer=%pI4 port=%u hardware=%u capture=%u frozen=%u expired=%u dsacks=%d reason=%u\n",
			   &s->server, &s->peer, s->port, s->hardware, s->capture,
			   smp_load_acquire(&s->frozen), time_after_eq(jiffies, s->deadline),
			   atomic_read(&s->dsacks), READ_ONCE(s->freeze_reason));
		if (s->rings)
			for (cpu = 0; cpu < s->cpus; cpu++) {
				u64 count = READ_ONCE(s->rings[cpu].count);
				seq_printf(m, "cpu=%u recorded=%llu overwritten=%llu\n", cpu,
					   count, count > H418_SLOTS ? count - H418_SLOTS : 0);
			}
	}
	mutex_unlock(&h418_mutex);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(h418_status);

static int h418_records_open(struct inode *inode, struct file *file)
{
	struct h418_session *s;
	int ret = 0;

	mutex_lock(&h418_mutex);
	s = rcu_dereference_protected(h418_current, lockdep_is_held(&h418_mutex));
	if (!s || !s->capture || !smp_load_acquire(&s->frozen)) {
		ret = -EAGAIN;
	} else {
		/* Drain writers that were already running when freeze was set. */
		synchronize_rcu();
		refcount_inc(&s->refs);
		file->private_data = s;
	}
	mutex_unlock(&h418_mutex);
	return ret;
}

static ssize_t h418_records_read(struct file *file, char __user *buf,
				size_t len, loff_t *pos)
{
	struct h418_session *s = file->private_data;
	struct h418_header hdr = {
		.magic = "H418RING", .version = cpu_to_le32(H418_VERSION),
		.record_size = cpu_to_le32(sizeof(struct h418_record)),
		.slots = cpu_to_le32(H418_SLOTS), .cpus = cpu_to_le32(s->cpus),
		.started_ns = cpu_to_le64(s->started_ns),
		.frozen_ns = cpu_to_le64(s->frozen_ns),
		.server = s->server, .peer = s->peer, .port = cpu_to_le32(s->port),
		.hardware = cpu_to_le32(s->hardware),
		.dsacks = cpu_to_le32(atomic_read(&s->dsacks)),
		.reason = cpu_to_le32(s->freeze_reason),
	};
	struct h418_cpu_header ch = {};
	const size_t bytes = H418_SLOTS * sizeof(struct h418_record);
	const size_t block = sizeof(ch) + bytes;
	size_t location, available, part;
	const void *source;
	unsigned int cpu;

	if (*pos < 0)
		return -EINVAL;
	if (*pos < sizeof(hdr)) {
		source = (const u8 *)&hdr + *pos;
		available = sizeof(hdr) - *pos;
	} else {
		location = *pos - sizeof(hdr);
		cpu = location / block;
		if (cpu >= s->cpus)
			return 0;
		location %= block;
		if (location < sizeof(ch)) {
			u64 count = s->rings[cpu].count;
			ch.cpu = cpu_to_le32(cpu);
			ch.count = cpu_to_le64(count);
			ch.overwritten = cpu_to_le64(count > H418_SLOTS ? count - H418_SLOTS : 0);
			source = (const u8 *)&ch + location;
			available = sizeof(ch) - location;
		} else {
			location -= sizeof(ch);
			source = (const u8 *)s->rings[cpu].records + location;
			available = bytes - location;
		}
	}
	part = min(len, available);
	if (copy_to_user(buf, source, part))
		return -EFAULT;
	*pos += part;
	return part;
}

static int h418_records_release(struct inode *inode, struct file *file)
{
	h418_put(file->private_data);
	return 0;
}

static const struct file_operations h418_control_fops = {
	.owner = THIS_MODULE, .write = h418_control_write, .llseek = no_llseek,
};
static const struct file_operations h418_records_fops = {
	.owner = THIS_MODULE, .open = h418_records_open, .read = h418_records_read,
	.release = h418_records_release, .llseek = no_llseek,
};

static int __init h418_init(void)
{
	struct dentry *dir;

	BUILD_BUG_ON(sizeof(struct h418_header) != 64);
	BUILD_BUG_ON(sizeof(struct h418_cpu_header) != 32);
	dir = debugfs_create_dir("hnat418", NULL);
	if (IS_ERR(dir))
		return PTR_ERR(dir);
	debugfs_create_file("control", 0600, dir, NULL, &h418_control_fops);
	debugfs_create_file("status", 0400, dir, NULL, &h418_status_fops);
	debugfs_create_file("records", 0400, dir, NULL, &h418_records_fops);
	return 0;
}
late_initcall(h418_init);

#if IS_ENABLED(CONFIG_HNAT418_KUNIT)
#include "hnat418_test.c"
#endif
