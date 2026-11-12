/* QcacldWifi: Linux kernel API that qcacld-3.0 uses beyond the Ath11kWifi runtime shim (forced include after lx_pci.h).
 * Simple ones are inline here; the rest are declared and implemented by the module's runtime (src/qcacld_rt.c, link stage). */
#ifndef QCACLD_KERNEL_H
#define QCACLD_KERNEL_H

/* ---- types ---- */
typedef long off_t;
typedef long long loff_t;
typedef unsigned short umode_t;
typedef u16 __sum16;
typedef u32 __wsum;
typedef struct { int event; } pm_message_t;
typedef struct cpumask cpumask_t;
#ifndef NAME_MAX
#define NAME_MAX 255

/* ---- tracepoints (WLAN_TRACEPOINTS off: only the empty trace_* inlines are generated) ---- */
#define PARAMS(args...) args
#define TP_PROTO(args...) args
#define TP_ARGS(args...) args
#define TP_STRUCT__entry(args...) args
#define TP_fast_assign(args...) args
#define TP_printk(fmt, args...) fmt, args
#endif
#ifndef NR_CPUS
#define NR_CPUS 8

/* ---- tracepoints (WLAN_TRACEPOINTS off: only the empty trace_* inlines are generated) ---- */
#define PARAMS(args...) args
#define TP_PROTO(args...) args
#define TP_ARGS(args...) args
#define TP_STRUCT__entry(args...) args
#define TP_fast_assign(args...) args
#define TP_printk(fmt, args...) fmt, args
#endif
#define PAGE_SHIFT 14
#define KERN_EMERG "0"
#define KERN_ALERT "1"
#define KERN_CRIT "2"
#define KERN_ERR "3"
#define KERN_WARNING "4"
#define KERN_NOTICE "5"
#define KERN_INFO "6"
#define KERN_DEBUG "7"
#define KERN_CONT ""
#define printk(fmt, ...) (lx_log(((fmt)[0] >= '0' && (fmt)[0] <= '7') ? (fmt)[0] - '0' : 6, (fmt) + (((fmt)[0] >= '0' && (fmt)[0] <= '7') ? 1 : 0), ##__VA_ARGS__), 0)
#define vprintk(fmt, ap) (lx_vlog(6, fmt, ap), 0)
void lx_vlog(int level, const char *fmt, __builtin_va_list ap);
#define pr_alert(...) lx_log(1, __VA_ARGS__)
#define pr_crit(...) lx_log(2, __VA_ARGS__)
void dump_stack(void);
void print_hex_dump(const char *level, const char *prefix, int type, int rowsize, int groupsize, const void *buf, size_t len, bool ascii);
#define DUMP_PREFIX_NONE 0
#define DUMP_PREFIX_ADDRESS 1
#define DUMP_PREFIX_OFFSET 2
#define prefetch(p) __builtin_prefetch(p)
#define object_is_on_stack(p) 0

/* ---- time ---- */
#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC 1
#define CLOCK_BOOTTIME 7
#define HRTIMER_MODE_PINNED 2
#define HRTIMER_MODE_SOFT 4
#define HRTIMER_MODE_REL_PINNED (HRTIMER_MODE_REL | HRTIMER_MODE_PINNED)
#define HRTIMER_MODE_ABS_PINNED (HRTIMER_MODE_ABS | HRTIMER_MODE_PINNED)
struct timespec64 { long long tv_sec; long tv_nsec; };
struct timeval { long tv_sec; long tv_usec; };
struct rtc_time { int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst; };
#define ns_to_ktime(ns) ((ktime_t)(ns))
#define ktime_add(a, b) ((a) + (b))
#define ktime_add_ns(k, ns) ((k) + (ktime_t)(ns))
#define ktime_add_us(k, us) ((k) + (ktime_t)(us) * 1000)
#define ktime_compare(a, b) ((a) < (b) ? -1 : (a) > (b) ? 1 : 0)
#define ktime_to_us(k) ((s64)(k) / 1000)
#define ktime_to_timespec64(k) ((struct timespec64){ (k) / 1000000000LL, (k) % 1000000000LL })
#define ktime_get_real() ktime_get()
#define ktime_get_boottime_ts64(ts) ktime_get_ts64(ts)
static inline void ktime_get_ts64(struct timespec64 *ts) { ktime_t k = ktime_get(); ts->tv_sec = k / 1000000000LL; ts->tv_nsec = k % 1000000000LL; }
static inline void ktime_get_real_ts64(struct timespec64 *ts) { ktime_get_ts64(ts); }
#define jiffies_to_nsecs(j) ((u64)jiffies_to_msecs(j) * 1000000ULL)
#define jiffies_to_usecs(j) ((u64)jiffies_to_msecs(j) * 1000ULL)
#define sched_clock() ((u64)ktime_get())
static inline void do_gettimeofday(struct timeval *tv) { ktime_t k = ktime_get(); tv->tv_sec = k / 1000000000LL; tv->tv_usec = (k % 1000000000LL) / 1000; }
void rtc_time64_to_tm(long long time, struct rtc_time *tm);
#define time64_to_tm(t, off, tm) rtc_time64_to_tm(t, (struct rtc_time *)(tm))
/* hrtimer pieces the runtime does not have */
#define hrtimer_init(t, clock, mode) hrtimer_setup(t, NULL, clock, mode)
#define hrtimer_active(t) hrtimer_is_queued(t)
#define hrtimer_callback_running(t) 0
ktime_t hrtimer_get_remaining(const struct hrtimer *timer);
#define hrtimer_cb_get_time(t) ktime_get()
u64 hrtimer_forward(struct hrtimer *timer, ktime_t now, ktime_t interval);
static inline void hrtimer_add_expires(struct hrtimer *t, ktime_t d) { t->expires += d; }
#define hrtimer_try_to_cancel(t) hrtimer_cancel(t)
/* timer_list extras */
#define TIMER_DEFERRABLE 0x1
#define TIMER_PINNED 0x2
#define TIMER_IRQSAFE 0x4
#define timer_setup_on_stack(t, f, fl) timer_setup(t, f, fl)
#define destroy_timer_on_stack(t) ((void)0)
#define add_timer(t) lx_timer_mod(t, (t)->expires)
#define add_timer_on(t, cpu) add_timer(t)
#ifndef timer_pending
#define timer_pending(t) delayed_work_pending(&(t)->d)

/* ---- tracepoints (WLAN_TRACEPOINTS off: only the empty trace_* inlines are generated) ---- */
#define PARAMS(args...) args
#define TP_PROTO(args...) args
#define TP_ARGS(args...) args
#define TP_STRUCT__entry(args...) args
#define TP_fast_assign(args...) args
#define TP_printk(fmt, args...) fmt, args
#endif

/* ---- workqueues ---- */
#define create_workqueue(name) alloc_workqueue(name, 0, 1)
#define create_singlethread_workqueue(name) alloc_ordered_workqueue(name, 0)

/* ---- bitops / math ---- */
static inline void change_bit(int n, volatile unsigned long *p) { __atomic_xor_fetch(p + n / 64, 1UL << (n % 64), __ATOMIC_SEQ_CST); }
static inline int test_and_change_bit(int n, volatile unsigned long *p) {
  unsigned long m = 1UL << (n % 64); return (__atomic_fetch_xor(p + n / 64, m, __ATOMIC_SEQ_CST) & m) != 0; }
static inline int __test_and_clear_bit(int n, unsigned long *p) { unsigned long m = 1UL << (n % 64); int o = (p[n / 64] & m) != 0; p[n / 64] &= ~m; return o; }
static inline int bitmap_and(unsigned long *dst, const unsigned long *a, const unsigned long *b, unsigned int bits) {
  unsigned long r = 0; for (unsigned i = 0; i < (bits + 63) / 64; i++) { dst[i] = a[i] & b[i]; r |= dst[i]; } return r != 0; }
static inline unsigned long __rounddown_pow_of_two(unsigned long n) { return n ? 1UL << (63 - __builtin_clzl(n)) : 0; }
static inline int hex_to_bin(unsigned char c) { if (c >= '0' && c <= '9') return c - '0'; c |= 0x20; return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; }
static inline int hex2bin(u8 *dst, const char *src, size_t count) {
  while (count--) { int hi = hex_to_bin(*src++), lo = hex_to_bin(*src++); if (hi < 0 || lo < 0) return -EINVAL; *dst++ = (hi << 4) | lo; } return 0; }

/* ---- context / locks ---- */
#define irqs_disabled() 0
#define in_softirq() 0
#define in_irq() 0
#define in_atomic() 0
#define preemptible() 1
#define rcu_preempt_depth() 0
#define spin_trylock_bh(l) spin_trylock(l)
#define spin_trylock_irqsave(l, f) ({ (void)(f); spin_trylock(l); })
struct semaphore { struct completion c; int count; spinlock_t lock; };
void sema_init(struct semaphore *sem, int val);
void down(struct semaphore *sem);
int down_interruptible(struct semaphore *sem);
int down_trylock(struct semaphore *sem);
int down_timeout(struct semaphore *sem, long jiffies_timeout);
void up(struct semaphore *sem);

/* ---- strings ---- */
size_t strlcpy(char *dst, const char *src, size_t size);
#define isspace(c) ((c) == ' ' || ((c) >= '\t' && (c) <= '\r'))
char *skip_spaces(const char *s);
char *strim(char *s);
size_t strnlen(const char *s, size_t n);
char *strsep(char **s, const char *ct);

/* ---- checksums and skb helpers ---- */
__sum16 ip_fast_csum(const void *iph, unsigned int ihl);
__sum16 csum_ipv6_magic(const void *saddr, const void *daddr, u32 len, u8 proto, __wsum sum);
__sum16 csum_tcpudp_magic(u32 saddr, u32 daddr, u32 len, u8 proto, __wsum sum);
__wsum csum_partial(const void *buf, int len, __wsum sum);
#define SKB_GSO_TCPV4 0x1
#define SKB_GSO_TCPV6 0x10
#define SKB_GSO_UDP_L4 0x40000
#define SKB_WITH_OVERHEAD(x) ((x) - 320)
static inline unsigned char *skb_transport_header(const struct sk_buff *skb) { return skb->head + skb->transport_header; }
static inline int skb_transport_offset(const struct sk_buff *skb) { return skb_transport_header(skb) - skb->data; }
static inline unsigned char *skb_end_pointer(const struct sk_buff *skb) { return skb->end; }
static inline unsigned int skb_end_offset(const struct sk_buff *skb) { return skb->end - skb->head; }
static inline bool skb_is_gso_v6(const struct sk_buff *skb) { return skb_shinfo(skb)->gso_type & SKB_GSO_TCPV6; }
static inline void skb_record_rx_queue(struct sk_buff *skb, u16 q) { skb->queue_mapping = q + 1; }
static inline void __net_timestamp(struct sk_buff *skb) { skb->tstamp = ktime_get(); }
static inline ktime_t skb_get_ktime(const struct sk_buff *skb) { return skb->tstamp; }
static inline ktime_t net_timedelta(ktime_t t) { return ktime_get() - t; }
struct tcphdr;
static inline struct tcphdr *tcp_hdr(const struct sk_buff *skb) { return (struct tcphdr *)skb_transport_header(skb); }
unsigned int tcp_hdrlen(const struct sk_buff *skb);
#define skb_walk_frags(skb, iter) for (iter = skb_shinfo(skb)->frag_list; iter; iter = iter->next)
struct sk_buff *skb_unshare(struct sk_buff *skb, gfp_t pri);
void skb_coalesce_rx_frag(struct sk_buff *skb, int i, int size, unsigned int truesize);
void *netdev_alloc_frag(unsigned int fragsz);
void skb_free_frag(void *addr);
struct page_frag_cache { void *va; unsigned offset, size; };
void *page_frag_alloc(struct page_frag_cache *nc, unsigned int fragsz, gfp_t gfp);
void page_frag_free(void *addr);
struct net_device *dev_get_by_name(struct net *net, const char *name);
static inline void dev_addr_mod(struct net_device *dev, unsigned int offset, const void *addr, size_t len) {
  memcpy(dev->dev_addr + offset, addr, len); }

/* ---- wireless extensions: constants only (no WEXT ioctls on XNU) ---- */
#define SIOCIWFIRST 0x8B00
#define SIOCGIWAP 0x8B15
#define SIOCGIWSCAN 0x8B19
#define SIOCDEVPRIVATE 0x89F0
#define IWEVCUSTOM 0x8C02
#define IWEVREGISTERED 0x8C03
#define IWEVEXPIRED 0x8C04
#define IWEVMICHAELMICFAILURE 0x8C06
#define IW_CUSTOM_MAX 256
#define IW_PRIV_SIZE_MASK 0x07FF
#define IW_AUTH_CIPHER_NONE 0x00000001
#define IW_MICFAILURE_GROUP 0x00000004
#define IW_MICFAILURE_PAIRWISE 0x00000008

/* ---- DMA scatter tables ---- */
struct sg_table { struct scatterlist *sgl; unsigned int nents, orig_nents; };
int dma_get_sgtable(struct device *dev, struct sg_table *sgt, void *cpu_addr, dma_addr_t dma_addr, size_t size);
void sg_free_table(struct sg_table *table);
#define for_each_sg(sglist, sg, nr, i) for ((i) = 0, (sg) = (sglist); (i) < (nr); (i)++, (sg)++)
phys_addr_t sg_phys(struct scatterlist *sg);
phys_addr_t virt_to_phys(const volatile void *addr);

/* ---- devres, irq, cpu topology ---- */
#define devm_kzalloc(dev, size, gfp) lx_alloc(size, 1)
#define devm_kfree(dev, p) lx_free(p)
#define IRQ_NO_BALANCING 0x400
#define irq_modify_status(irq, clr, set) ((void)0)
#define topology_cluster_id(cpu) 0
#define topology_physical_package_id(cpu) 0

/* ---- power management (no runtime PM, no wakeup sources on this port) ---- */
struct dev_pm_qos_request { int type; s32 value; struct device *dev; };
#define dev_pm_qos_add_request(dev, req, type, value) 0
#define dev_pm_qos_update_request(req, value) 0
#define dev_pm_qos_remove_request(req) 0
#define dev_pm_qos_request_active(req) 0
#define PM_QOS_CPU_LATENCY_DEFAULT_VALUE (2000 * 1000)
#define pm_runtime_enabled(dev) 0
#define pm_runtime_get(dev) 0
#define pm_runtime_put_autosuspend(dev) 0
#define pm_runtime_put_sync_suspend(dev) 0
#define pm_runtime_set_autosuspend_delay(dev, d) ((void)0)
#define pm_runtime_mark_last_busy(dev) ((void)0)
#define pm_runtime_resume(dev) 0
#define pm_request_resume(dev) 0

/* ---- notifier chains (atomic ones are the runtime's) ---- */
struct blocking_notifier_head { struct notifier_block *head; };
#define BLOCKING_NOTIFIER_HEAD(name) struct blocking_notifier_head name = { NULL }
#define BLOCKING_INIT_NOTIFIER_HEAD(h) ((h)->head = NULL)
#define ATOMIC_NOTIFIER_HEAD(name) struct atomic_notifier_head name = { 0 }
#define ATOMIC_INIT_NOTIFIER_HEAD(h) ((void)(h))
int blocking_notifier_chain_register(struct blocking_notifier_head *nh, struct notifier_block *nb);
int blocking_notifier_chain_unregister(struct blocking_notifier_head *nh, struct notifier_block *nb);
int blocking_notifier_call_chain(struct blocking_notifier_head *nh, unsigned long val, void *v);
#define atomic_notifier_call_chain(nh, val, v) 0

/* ---- scatterlist (the runtime keeps it opaque) ---- */
struct scatterlist { unsigned long page_link; unsigned int offset, length; dma_addr_t dma_address; unsigned int dma_length; };
#define sg_dma_address(sg) ((sg)->dma_address)
#define sg_dma_len(sg) ((sg)->dma_length)
void sg_init_one(struct scatterlist *sg, const void *buf, unsigned int buflen);
void sg_init_table(struct scatterlist *sgl, unsigned int nents);
void sg_set_buf(struct scatterlist *sg, const void *buf, unsigned int buflen);
void *sg_virt(struct scatterlist *sg);

/* ---- wakeup sources (no system suspend on this port: bookkeeping only) ---- */
struct wakeup_source { const char *name; struct device *dev; int active; unsigned long event_count, active_count, relax_count, expire_count, wakeup_count;
  ktime_t total_time, max_time, last_time, start_prevent_time, prevent_sleep_time; };
struct wakeup_source *wakeup_source_register(struct device *dev, const char *name);
void wakeup_source_unregister(struct wakeup_source *ws);
void __pm_stay_awake(struct wakeup_source *ws);
void __pm_relax(struct wakeup_source *ws);
void __pm_wakeup_event(struct wakeup_source *ws, unsigned int msec);
#define wakeup_source_add(ws) ((void)0)
#define wakeup_source_remove(ws) ((void)0)
#define wakeup_source_prepare(ws, n) ((ws)->name = (n))
#define wakeup_source_drop(ws) ((void)0)
#define pm_wakeup_ws_event(ws, msec, hard) __pm_wakeup_event(ws, msec)

/* ---- IP/TCP headers and link statistics ---- */
struct tcphdr { u16 source, dest; u32 seq, ack_seq;
  u16 res1:4, doff:4, fin:1, syn:1, rst:1, psh:1, ack:1, urg:1, ece:1, cwr:1; u16 window; __sum16 check; u16 urg_ptr; };
struct udphdr { u16 source, dest, len; __sum16 check; };
struct rtnl_link_stats64 { u64 rx_packets, tx_packets, rx_bytes, tx_bytes, rx_errors, tx_errors, rx_dropped, tx_dropped, multicast,
  collisions, rx_length_errors, rx_over_errors, rx_crc_errors, rx_frame_errors, rx_fifo_errors, rx_missed_errors, tx_aborted_errors,
  tx_carrier_errors, tx_fifo_errors, tx_heartbeat_errors, tx_window_errors, rx_compressed, tx_compressed, rx_nohandler; };

/* ---- netlink attribute parsing (the runtime's netlink only encodes events; vendor commands are parsed here) ---- */
#include <net/netlink.h>
enum { NLA_UNSPEC, NLA_U8, NLA_U16, NLA_U32, NLA_U64, NLA_STRING, NLA_FLAG, NLA_MSECS, NLA_NESTED, NLA_NESTED_ARRAY,
  NLA_NUL_STRING, NLA_BINARY, NLA_S8, NLA_S16, NLA_S32, NLA_S64, NLA_BITFIELD32, NLA_REJECT, NLA_BE16, NLA_BE32 };
struct nla_policy { u8 type; u8 validation_type; u16 len; union { const struct nla_policy *nested_policy; struct { s16 min, max; }; }; };
#define NLA_POLICY_NESTED(policy) { .type = NLA_NESTED, .nested_policy = policy }
#define NLA_POLICY_MAX(tp, m) { .type = tp, .max = m }
#define NLA_POLICY_RANGE(tp, mn, mx) { .type = tp, .min = mn, .max = mx }
#define NLA_POLICY_MIN(tp, mn) { .type = tp, .min = mn }
#define NLA_POLICY_EXACT_LEN(l) { .type = NLA_BINARY, .len = l }
#define NLA_POLICY_MIN_LEN(l) { .type = NLA_BINARY, .len = l }
int nla_parse(struct nlattr **tb, int maxtype, const struct nlattr *head, int len, const struct nla_policy *policy,
              struct netlink_ext_ack *extack);
static inline int nla_parse_nested(struct nlattr **tb, int maxtype, const struct nlattr *nla, const struct nla_policy *policy,
                                   struct netlink_ext_ack *extack) { return nla_parse(tb, maxtype, nla_data(nla), nla_len(nla), policy, extack); }
#define nla_parse_nested_deprecated nla_parse_nested
#define nla_parse_deprecated nla_parse
ssize_t nla_strscpy(char *dst, const struct nlattr *nla, size_t dstsize);
int nla_memcpy(void *dest, const struct nlattr *src, int count);
#define nla_for_each_attr(pos, head, len, rem) \
  for (pos = (struct nlattr *)(head), rem = (len); rem >= (int)sizeof(*pos) && pos->nla_len >= sizeof(*pos) && pos->nla_len <= rem; \
       rem -= NLA_ALIGN(pos->nla_len), pos = (struct nlattr *)((char *)pos + NLA_ALIGN(pos->nla_len)))
#define nla_for_each_nested(pos, nla, rem) nla_for_each_attr(pos, nla_data(nla), nla_len(nla), rem)
static inline int nla_attr_size(int payload) { return NLA_HDRLEN + payload; }
static inline s16 nla_get_s16(const struct nlattr *a) { s16 v; memcpy(&v, nla_data(a), 2); return v; }
static inline s64 nla_get_s64(const struct nlattr *a) { s64 v; memcpy(&v, nla_data(a), 8); return v; }
static inline int nla_put_s16(struct sk_buff *m, int t, s16 v) { return nla_put(m, t, 2, &v); }
static inline int nla_put_in_addr(struct sk_buff *m, int t, u32 v) { return nla_put(m, t, 4, &v); }
static inline int nla_put_be32(struct sk_buff *m, int t, u32 v) { return nla_put(m, t, 4, &v); }
static inline struct nlattr *nla_next(const struct nlattr *nla, int *rem) {
  int l = NLA_ALIGN(nla->nla_len); *rem -= l; return (struct nlattr *)((char *)nla + l); }
static inline int nla_ok(const struct nlattr *nla, int rem) { return rem >= (int)sizeof(*nla) && nla->nla_len >= sizeof(*nla) && nla->nla_len <= rem; }
static inline void *nlmsg_data(const struct nlmsghdr *nlh) { return (unsigned char *)nlh + NLMSG_HDRLEN; }
static inline int nlmsg_len(const struct nlmsghdr *nlh) { return nlh->nlmsg_len - NLMSG_HDRLEN; }
static inline struct nlmsghdr *nlmsg_hdr(const struct sk_buff *skb) { return (struct nlmsghdr *)skb->data; }
struct nlmsghdr *nlmsg_put(struct sk_buff *skb, u32 portid, u32 seq, int type, int payload, int flags);
int netlink_broadcast(struct sock *ssk, struct sk_buff *skb, u32 portid, u32 group, gfp_t allocation);
int netlink_unicast(struct sock *ssk, struct sk_buff *skb, u32 portid, int nonblock);
struct netlink_kernel_cfg { unsigned int groups, flags; void (*input)(struct sk_buff *skb); };
struct sock *netlink_kernel_create(struct net *net, int unit, struct netlink_kernel_cfg *cfg);
void netlink_kernel_release(struct sock *sk);

/* ---- tracepoints (WLAN_TRACEPOINTS off: only the empty trace_* inlines are generated) ---- */
#define PARAMS(args...) args
#define TP_PROTO(args...) args
#define TP_ARGS(args...) args
#define TP_STRUCT__entry(args...) args
#define TP_fast_assign(args...) args
#define TP_printk(fmt, args...) fmt, args

/* ---- batch 3: errno, lists, strings, parsing ---- */
#ifndef ECONNABORTED
#define ECONNABORTED 103
#endif
#ifndef ETIME
#define ETIME 62
#endif
#ifndef ENONET
#define ENONET 64
#endif
typedef char *caddr_t;
enum hrtimer_mode { HRTIMER_MODE__QCACLD_UNUSED = -1 };
#define L1_CACHE_BYTES 64
#define round_up(x, y) ((((x) - 1) | ((__typeof__(x))((y) - 1))) + 1)
#define round_down(x, y) ((x) & ~((__typeof__(x))((y) - 1)))
#define _RET_IP_ ((unsigned long)__builtin_return_address(0))
#define __GFP_DIRECT_RECLAIM 0x400u
#define __GFP_KSWAPD_RECLAIM 0x800u
#define ksize(p) lx_ksize(p)
size_t lx_ksize(const void *p);
static inline unsigned long hash_long(unsigned long val, unsigned int bits) {
  return bits ? (unsigned long)(val * 0x61C8864680B583EBull) >> (64 - bits) : 0; }
static inline void list_cut_position(struct list_head *list, struct list_head *head, struct list_head *entry) {
  if (list_empty(head) || (head->next != entry && head == entry)) { INIT_LIST_HEAD(list); return; }
  if (entry == head) { INIT_LIST_HEAD(list); return; }
  struct list_head *first = head->next;
  list->next = first; first->prev = list; list->prev = entry;
  head->next = entry->next; entry->next->prev = head; entry->next = list; }
static inline int isxdigit(int c) { return (c >= '0' && c <= '9') || ((c | 0x20) >= 'a' && (c | 0x20) <= 'f'); }
static inline int strncasecmp(const char *a, const char *b, size_t n) {
  for (; n; n--, a++, b++) { int x = *a | ((*a >= 'A' && *a <= 'Z') ? 0x20 : 0), y = *b | ((*b >= 'A' && *b <= 'Z') ? 0x20 : 0);
    if (x != y || !*a) return x - y; } return 0; }
static inline char *strnchr(const char *s, size_t n, int c) { for (; n-- && *s; s++) if (*s == (char)c) return (char *)s; return NULL; }
static inline char *strchrnul(const char *s, int c) { while (*s && *s != (char)c) s++; return (char *)s; }
static inline char *strpbrk(const char *s, const char *set) {
  for (; *s; s++) for (const char *t = set; *t; t++) if (*s == *t) return (char *)s; return NULL; }
static inline char *strnstr(const char *s1, const char *s2, size_t len) {
  size_t l2 = strlen(s2); if (!l2) return (char *)s1;
  while (len >= l2) { len--; if (!memcmp(s1, s2, l2)) return (char *)s1; s1++; } return NULL; }
int vscnprintf(char *buf, size_t size, const char *fmt, va_list args);
int hex_dump_to_buffer(const void *buf, size_t len, int rowsize, int groupsize, char *linebuf, size_t linebuflen, bool ascii);
int lx_kstrtoll(const char *s, unsigned int base, long long *res);
#define LX_KSTRTOU(name, type, max) static inline int name(const char *s, unsigned int base, type *res) { \
  unsigned long v; int r = lx_kstrtoul(s, base, &v); if (r) return r; if (v > (max)) return -ERANGE; *res = (type)v; return 0; }
#define LX_KSTRTOS(name, type, min, max) static inline int name(const char *s, unsigned int base, type *res) { \
  long long v; int r = lx_kstrtoll(s, base, &v); if (r) return r; if (v < (min) || v > (max)) return -ERANGE; *res = (type)v; return 0; }
LX_KSTRTOU(kstrtou8, u8, 0xff)
LX_KSTRTOU(kstrtou16, u16, 0xffff)
LX_KSTRTOU(kstrtou32, u32, 0xffffffffu)
LX_KSTRTOU(kstrtouint, unsigned int, 0xffffffffu)
LX_KSTRTOS(kstrtos8, s8, -128, 127)
LX_KSTRTOS(kstrtos32, s32, -0x7fffffffll - 1, 0x7fffffff)
LX_KSTRTOS(kstrtoint, int, -0x7fffffffll - 1, 0x7fffffff)
static inline bool mac_pton(const char *s, u8 *mac) {
  for (int i = 0; i < 6; i++) { int h = hex_to_bin(s[i * 3]), l = hex_to_bin(s[i * 3 + 1]);
    if (h < 0 || l < 0 || (i < 5 && s[i * 3 + 2] != ':')) return false; mac[i] = (u8)(h << 4 | l); } return true; }
/* No user space on this path: the private ioctls are reached only from the board, with kernel buffers. */
#define copy_to_user(to, from, n) (memcpy((to), (from), (n)), 0ul)
#define copy_from_user(to, from, n) (memcpy((to), (from), (n)), 0ul)
#define in_compat_syscall() 0
#define __ratelimit(rs) 1
struct ratelimit_state { int interval, burst; };
#define DEFINE_RATELIMIT_STATE(name, i, b) struct ratelimit_state name = { i, b }

/* ---- threads (the runtime's kthreads) ---- */
struct task_struct { char comm[16]; int pid; void *lx_thread; };
struct task_struct *lx_current(void);
#define current lx_current()
struct task_struct *kthread_create_on_node(int (*fn)(void *), void *data, int node, const char *fmt, ...);
#define kthread_create(fn, data, fmt...) kthread_create_on_node(fn, data, -1, fmt)
int wake_up_process(struct task_struct *t);
long schedule_timeout_interruptible(long timeout);
long schedule_timeout_uninterruptible(long timeout);
__attribute__((noreturn)) void kthread_complete_and_exit(struct completion *c, long code);
#define set_user_nice(t, n) ((void)0)
#define sched_setscheduler(t, p, prm) 0
#define kthread_bind(t, cpu) ((void)0)
#define set_cpus_allowed_ptr(t, m) 0
#define task_pid_nr(t) ((t)->pid)

/* ---- cpus (one possible cpu set: the runtime runs threads, not per-cpu work) ---- */
#define num_possible_cpus() 1
#define num_online_cpus() 1
#define for_each_possible_cpu(cpu) for ((cpu) = 0; (cpu) < 1; (cpu)++)
#define for_each_online_cpu(cpu) for_each_possible_cpu(cpu)
#define for_each_present_cpu(cpu) for_each_possible_cpu(cpu)
#define for_each_cpu(cpu, mask) for ((cpu) = 0; (cpu) < 1; (cpu)++)
#define for_each_cpu_andnot(cpu, m1, m2) for_each_cpu(cpu, m1)
#define get_cpu_device(c) ((struct device *)NULL)

/* ---- rtnl / netdev queues (one tx queue; stop/wake map to the runtime's subqueue calls) ---- */
#define rtnl_trylock() (lx_rtnl_lock(), 1)
#define rtnl_is_locked() lx_rtnl_is_locked()
#define NET_NAME_UNKNOWN 0
#define ETH_P_CONTROL 0x0016
#define ETH_DATA_LEN 1500
#define IFF_BROADCAST 0x2
#define IFF_MULTICAST 0x1000
#define netif_stop_queue(d) netif_stop_subqueue(d, 0)
#define netif_wake_queue(d) netif_wake_subqueue(d, 0)
#define netif_start_queue(d) netif_wake_subqueue(d, 0)
#define netif_tx_start_all_queues(d) netif_wake_subqueue(d, 0)
#define netif_tx_wake_all_queues(d) netif_wake_subqueue(d, 0)
#define netif_tx_stop_all_queues(d) netif_stop_subqueue(d, 0)
#define netif_tx_disable(d) netif_stop_subqueue(d, 0)
#define netif_queue_stopped(d) test_bit(0, &(d)->stopped_queues)
struct netdev_queue { struct net_device *dev; unsigned int index; unsigned long trans_start, state; int xmit_lock_owner; };
static inline struct netdev_queue *netdev_get_tx_queue(struct net_device *d, unsigned int i) {
  static struct netdev_queue q[16]; q[i & 15].dev = d; q[i & 15].index = i; return &q[i & 15]; }
#define netif_tx_queue_stopped(q) test_bit((q)->index, &(q)->dev->stopped_queues)
#define netif_tx_stop_queue(q) netif_stop_subqueue((q)->dev, (q)->index)
#define netif_tx_wake_queue(q) netif_wake_subqueue((q)->dev, (q)->index)
#define netif_tx_start_queue(q) netif_wake_subqueue((q)->dev, (q)->index)
#define netif_tx_lock(d) ((void)0)
#define netif_tx_unlock(d) ((void)0)
#define __netif_tx_trylock(q) 1
#define __netif_tx_unlock(q) ((void)0)
#define txq_trans_update(q) ((void)0)
#define netif_trans_update(d) ((void)0)
#define netdev_update_features(d) ((void)0)
#define netdev_name(d) ((d)->name)
#define netdev_mc_count(d) ((d)->mc.count)
#define netdev_for_each_mc_addr(ha, d) list_for_each_entry(ha, &(d)->mc.list, list)
int register_netdev(struct net_device *dev);
void unregister_netdev(struct net_device *dev);
#define alloc_netdev(sz, name, assign, setup) alloc_netdev_mqs(sz, name, assign, setup, 1, 1)
#define napi_complete(n) napi_complete_done(n, 0)

/* ---- netlink message macros ---- */
#define NLMSG_ALIGNTO 4u
#define NLMSG_ALIGN(len) (((len) + NLMSG_ALIGNTO - 1) & ~(NLMSG_ALIGNTO - 1))
#define NLMSG_LENGTH(len) ((len) + NLMSG_HDRLEN)
#define NLMSG_SPACE(len) NLMSG_ALIGN(NLMSG_LENGTH(len))
#define NLMSG_DATA(nlh) ((void *)(((char *)(nlh)) + NLMSG_HDRLEN))
#define NLM_F_REQUEST 0x01
#define NLA_POLICY_ETH_ADDR { .type = NLA_BINARY, .len = ETH_ALEN }
#define NLA_POLICY_ETH_ADDR_COMPAT NLA_POLICY_ETH_ADDR

/* ---- TCP flags ---- */
#define TCPHDR_FIN 0x01
#define TCPHDR_SYN 0x02
#define TCPHDR_RST 0x04
#define TCPHDR_PSH 0x08
#define TCPHDR_ACK 0x10
#define TCPHDR_URG 0x20
#define TCPHDR_ECE 0x40
#define TCPHDR_CWR 0x80

/* ---- procfs / seq_file / sysfs / module params: not on this port (creation fails softly) ---- */
struct proc_dir_entry;
struct proc_ops { int (*proc_open)(struct inode *, struct file *); ssize_t (*proc_read)(struct file *, char *, size_t, loff_t *);
  ssize_t (*proc_write)(struct file *, const char *, size_t, loff_t *); loff_t (*proc_lseek)(struct file *, loff_t, int);
  int (*proc_release)(struct inode *, struct file *); };
#define proc_mkdir(name, parent) ((struct proc_dir_entry *)NULL)
#define proc_create_data(name, mode, parent, ops, data) ((struct proc_dir_entry *)NULL)
#define remove_proc_entry(name, parent) ((void)0)
#define pde_data(inode) ((void *)NULL)
#define file_inode(f) ((struct inode *)NULL)
struct kobject;
struct kobj_attribute { struct attribute attr; ssize_t (*show)(struct kobject *, struct kobj_attribute *, char *);
  ssize_t (*store)(struct kobject *, struct kobj_attribute *, const char *, size_t); };
struct attribute_group { const char *name; struct attribute **attrs; };
#define __ATTR(_name, _mode, _show, _store) { .attr = { .name = #_name }, .show = _show, .store = _store }
#define kobject_create_and_add(name, parent) ((struct kobject *)NULL)
#define kobject_del(k) ((void)0)
#define kobject_put(k) ((void)0)
#define sysfs_create_group(k, g) (-ENODEV)
#define sysfs_remove_group(k, g) ((void)0)
struct kernel_param;
int param_set_int(const char *val, const struct kernel_param *kp);
int param_get_int(char *buffer, const struct kernel_param *kp);
int param_set_copystring(const char *val, const struct kernel_param *kp);
int param_get_string(char *buffer, const struct kernel_param *kp);

/* ---- batch 5: cpumasks (one word; the board's threads run on any cpu) ---- */
static const struct cpumask lx_cpu0_mask = { { 1ul } };
#define cpu_possible_mask (&lx_cpu0_mask)
#define cpu_online_mask (&lx_cpu0_mask)
#define cpu_present_mask (&lx_cpu0_mask)
#define cpumask_bits(m) ((m)->bits)
#define cpumask_set_cpu(c, m) ((m)->bits[0] |= 1ul << ((c) & 63))
#define cpumask_clear_cpu(c, m) ((m)->bits[0] &= ~(1ul << ((c) & 63)))
#define cpumask_test_cpu(c, m) (!!((m)->bits[0] & (1ul << ((c) & 63))))
#define cpumask_clear(m) ((m)->bits[0] = 0)
#define cpumask_setall(m) ((m)->bits[0] = 1ul)
#define cpumask_copy(d, s) ((void)((d)->bits[0] = (s)->bits[0]))
#define cpumask_empty(m) ((m)->bits[0] == 0)
#define cpumask_weight(m) __builtin_popcountl((m)->bits[0])
#define cpumask_equal(a, b) ((a)->bits[0] == (b)->bits[0])
#define cpumask_subset(a, b) (((a)->bits[0] & ~(b)->bits[0]) == 0)
#define cpumask_intersects(a, b) (((a)->bits[0] & (b)->bits[0]) != 0)
#define cpumask_or(d, a, b) ((d)->bits[0] = (a)->bits[0] | (b)->bits[0])
#define cpumask_and(d, a, b) (((d)->bits[0] = (a)->bits[0] & (b)->bits[0]) != 0)
#define cpumask_andnot(d, a, b) (((d)->bits[0] = (a)->bits[0] & ~(b)->bits[0]) != 0)
#define cpumask_pr_args(m) 1, (m)->bits
#define cpumap_print_to_pagebuf(list, buf, m) snprintf(buf, 4096, "%lx\n", (m)->bits[0])
#define topology_core_id(cpu) 0
#define topology_sibling_cpumask(cpu) (&lx_cpu0_mask)
#define topology_core_cpumask(cpu) (&lx_cpu0_mask)
#define cpufreq_quick_get_max(cpu) 0u

/* ---- irqs ---- */
#define local_irq_save(f) ((f) = 0)
#define local_irq_restore(f) ((void)(f))
#define IRQF_TRIGGER_RISING 0x00000001
#define enable_irq_wake(irq) 0
#define disable_irq_wake(irq) 0
int devm_request_irq(struct device *dev, unsigned int irq, irqreturn_t (*handler)(int, void *), unsigned long flags,
                     const char *name, void *dev_id);
typedef void (*work_func_t)(struct work_struct *work);
#define tasklet_hi_schedule(t) tasklet_schedule(t)
#define INIT_DEFERRABLE_WORK(w, f) INIT_DELAYED_WORK(w, f)
#define PM_EVENT_SUSPEND 0x0002
#define pm_system_wakeup() ((void)0)

/* ---- threads ---- */
#define get_task_struct(t) ((void)(t))
#define put_task_struct(t) ((void)(t))
int kthread_stop(struct task_struct *t);
bool kthread_should_stop(void);
#define signal_pending(t) 0
#define msleep_interruptible(ms) (msleep(ms), 0ul)

/* ---- strings / time ---- */
static inline int isalnum(int c) { return (c >= '0' && c <= '9') || ((c | 0x20) >= 'a' && (c | 0x20) <= 'z'); }
static inline size_t strlcat(char *dst, const char *src, size_t size) {
  size_t d = strnlen(dst, size); if (d == size) return d + strlen(src); return d + strlcpy(dst + d, src, size - d); }
struct timezone { int tz_minuteswest, tz_dsttime; };
extern struct timezone sys_tz;

/* ---- files, seq_file, debugfs/procfs ops (the files are never created on this port) ---- */
struct inode { void *i_private; };
struct file { void *private_data; struct inode *f_inode; loff_t f_pos; unsigned int f_flags; };
struct poll_table_struct;
struct file_operations { struct module *owner; loff_t (*llseek)(struct file *, loff_t, int);
  ssize_t (*read)(struct file *, char *, size_t, loff_t *); ssize_t (*write)(struct file *, const char *, size_t, loff_t *);
  unsigned int (*poll)(struct file *, struct poll_table_struct *); long (*unlocked_ioctl)(struct file *, unsigned int, unsigned long);
  long (*compat_ioctl)(struct file *, unsigned int, unsigned long); int (*mmap)(struct file *, void *);
  int (*open)(struct inode *, struct file *); int (*release)(struct inode *, struct file *); };
struct seq_file { char *buf; size_t size, from, count, pad_until; loff_t index; void *private; };
int single_open(struct file *f, int (*show)(struct seq_file *, void *), void *data);
int single_release(struct inode *inode, struct file *f);
ssize_t seq_read(struct file *f, char *buf, size_t size, loff_t *ppos);
loff_t seq_lseek(struct file *f, loff_t offset, int whence);
loff_t default_llseek(struct file *f, loff_t offset, int whence);
void seq_vprintf(struct seq_file *m, const char *fmt, va_list args);
#define seq_printf(m, fmt, ...) ((void)(m))
#define seq_puts(m, s) ((void)(m))
static inline bool seq_has_overflowed(struct seq_file *m) { return m->count == m->size; }
ssize_t simple_read_from_buffer(void *to, size_t count, loff_t *ppos, const void *from, size_t available);

/* ---- module parameters (set at load by the board, not by insmod) ---- */
struct kernel_param;
struct kernel_param_ops { unsigned int flags; int (*set)(const char *val, const struct kernel_param *kp);
  int (*get)(char *buffer, const struct kernel_param *kp); void (*free)(void *arg); };
struct kparam_string { unsigned int maxlen; char *string; };
struct kernel_param { const char *name; struct module *mod; const struct kernel_param_ops *ops; u16 perm; s8 level; u8 flags;
  union { void *arg; const struct kparam_string *str; const void *arr; }; };
#define module_param_cb(name, ops, arg, perm)
#define module_param_call(name, set, get, arg, perm)
#define module_param_string(name, string, len, perm)
#define module_param_named_unsafe(...)
#define MODULE_PARM_DESC(...)

/* ---- platform (icnss2 qmi.c) ---- */
typedef u32 dev_t;
#ifndef SZ_256
#define SZ_256 0x100
#endif
#ifndef SZ_8K
#define SZ_8K 0x2000
#endif
#ifndef devm_ioremap
#define devm_ioremap(d, pa, n) lx_ioremap(pa, n)
#endif
int of_fdt_get_ddrtype(void);   /* the board's DDR type (icnss2 sends it in the host capability) */

/* ---- Linux's AES library names: the runtime's aes_encrypt is hostap's (ctx, plain, crypt); Linux's (key, out, in) callers in
 * qcacld get their own names (src/nw_kglue.c implements them on hostap's AES). */
#define aes_encrypt lx_linux_aes_encrypt
#define aes_decrypt lx_linux_aes_decrypt
#define aes_preparekey lx_linux_aes_preparekey
#define aes_prepareenckey lx_linux_aes_prepareenckey
#endif
