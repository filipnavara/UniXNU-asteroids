/* QcacldWifi: the kernel functions qcacld needs beyond what the Ath11kWifi runtime (lx_rt, cfg80211, qmi, hostap crypto) defines.
 * Small and on the runtime's primitives. Subsystems this port does not have (procfs/seq_file, netlink sockets to user space,
 * inet address notifiers, wakeup sources, preallocated pages) are inert: creation fails softly or registrations are kept only.
 * Threads (kthread_*, wake_up_process) wait for the module's thread model: they fail until it exists (see the end of the file). */
#include "lx_pci.h"
#include "lx_plat.h"
#include <crypto/aes.h>
#include <net/genetlink.h>
#include <net/cfg80211.h>
#include <linux/rtnetlink.h>

/* ---- runtime additions ---- */
static struct task_struct g_dispatcher_task = { .comm = "wlan-dispatch", .pid = 1 };
struct task_struct *lx_current(void) { return &g_dispatcher_task; }
struct in_device *lx_in_dev(const struct net_device *dev) { (void)dev; return NULL; }     /* no address glue yet: no ARP offload */
struct inet6_dev *lx_in6_dev(const struct net_device *dev) { (void)dev; return NULL; }    /* no NS offload */
size_t lx_ksize(const void *p) { (void)p; return 0; }   /* the runtime's allocations carry no size: qdf only logs it */
int lx_kstrtoll(const char *s, unsigned int base, long long *res) {
  bool neg = *s == '-'; unsigned long v;
  int r = lx_kstrtoul(neg || *s == '+' ? s + 1 : s, (int)base, &v);
  if (r) return r;
  if (v > (neg ? 0x8000000000000000ull : 0x7fffffffffffffffull)) return -ERANGE;
  *res = neg ? -(long long)v : (long long)v; return 0;
}
int lx_pci_read_config(struct pci_dev *pdev, int off, int size, u32 *v) { (void)pdev; (void)off; (void)size; *v = ~0u; return -ENODEV; }
int lx_pci_write_config(struct pci_dev *pdev, int off, int size, u32 v) { (void)pdev; (void)off; (void)size; (void)v; return -ENODEV; }
void lx_vlog(int level, const char *fmt, va_list ap) {
  char line[256]; vsnprintf(line, sizeof line, fmt, ap); lx_log(level, "%s", line);
}

/* ---- strings ---- */
size_t strlcpy(char *dst, const char *src, size_t size) {
  size_t n = strlen(src);
  if (size) { size_t c = n >= size ? size - 1 : n; memcpy(dst, src, c); dst[c] = 0; }
  return n;
}
size_t strnlen(const char *s, size_t n) { size_t i = 0; while (i < n && s[i]) i++; return i; }
int strncmp(const char *a, const char *b, size_t n) {
  for (; n; n--, a++, b++) { if (*a != *b) return (unsigned char)*a - (unsigned char)*b; if (!*a) return 0; } return 0;
}
char *skip_spaces(const char *s) { while (isspace(*s)) s++; return (char *)s; }
char *strim(char *s) {
  size_t n = strlen(s); if (!n) return s;
  char *e = s + n - 1; while (e >= s && isspace(*e)) e--; e[1] = 0;
  return skip_spaces(s);
}
char *strsep(char **s, const char *ct) {
  char *b = *s; if (!b) return NULL;
  char *e = strpbrk(b, ct); if (e) *e++ = 0;
  *s = e; return b;
}
int vscnprintf(char *buf, size_t size, const char *fmt, va_list args) {
  if (!size) return 0;
  int n = vsnprintf(buf, size, fmt, args);
  return n < 0 ? 0 : (size_t)n >= size ? (int)size - 1 : n;
}

/* ---- logging ---- */
void dump_stack(void) { lx_log(4, "wlan: dump_stack (no backtrace on this port)"); }
void print_hex_dump(const char *level, const char *prefix, int type, int rowsize, int groupsize, const void *buf, size_t len, bool ascii) {
  (void)type; (void)groupsize; (void)ascii;
  int lvl = level && level[0] >= '0' && level[0] <= '7' ? level[0] - '0' : 7;
  if (rowsize <= 0 || rowsize > 32) rowsize = 16;
  for (size_t off = 0; off < len; off += (size_t)rowsize) {
    char line[3 * 32 + 1]; size_t n = len - off < (size_t)rowsize ? len - off : (size_t)rowsize, k = 0;
    for (size_t i = 0; i < n; i++) k += (size_t)snprintf(line + k, sizeof line - k, "%02x ", ((const u8 *)buf)[off + i]);
    lx_log(lvl, "%s%04zx: %s", prefix ? prefix : "", off, line);
  }
}

/* Linux's hex_dump_to_buffer for groupsize 1 (the others are printed bytewise too): "xx xx ..  ascii". Returns the length the
 * full line would have. */
int hex_dump_to_buffer(const void *buf, size_t len, int rowsize, int groupsize, char *linebuf, size_t linebuflen, bool ascii) {
  (void)groupsize;
  if (rowsize != 16 && rowsize != 32) rowsize = 16;
  if (len > (size_t)rowsize) len = (size_t)rowsize;
  size_t k = 0, need = 0; const u8 *b = buf;
  for (size_t i = 0; i < len; i++) {
    need += 3;
    if (k + 3 < linebuflen) k += (size_t)snprintf(linebuf + k, linebuflen - k, i + 1 < len ? "%02x " : "%02x", b[i]);
  }
  if (ascii) {
    need += 2 + len;
    for (size_t i = 0; i < 2 + len && k + 1 < linebuflen; i++) linebuf[k++] = i < 2 ? ' ' : (b[i - 2] >= 0x20 && b[i - 2] < 0x7f ? (char)b[i - 2] : '.');
  }
  if (linebuflen) linebuf[k < linebuflen ? k : linebuflen - 1] = 0;
  return (int)need;
}

/* ---- time ---- */
struct timezone sys_tz;   /* UTC */
void rtc_time64_to_tm(long long t, struct rtc_time *tm) {
  long long days = t / 86400, rem = t % 86400;
  if (rem < 0) { rem += 86400; days--; }
  tm->tm_hour = (int)(rem / 3600); tm->tm_min = (int)(rem % 3600 / 60); tm->tm_sec = (int)(rem % 60);
  tm->tm_wday = (int)((4 + days) % 7); if (tm->tm_wday < 0) tm->tm_wday += 7;
  /* civil from days (H. Hinnant) */
  long long z = days + 719468, era = (z >= 0 ? z : z - 146096) / 146097;
  unsigned doe = (unsigned)(z - era * 146097), yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  long long y = (long long)yoe + era * 400; unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
  unsigned d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9;
  y += m <= 2;
  tm->tm_mday = (int)d; tm->tm_mon = (int)m - 1; tm->tm_year = (int)(y - 1900); tm->tm_isdst = 0;
  static const unsigned short cum[] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
  bool leap = (y % 4 == 0 && y % 100) || y % 400 == 0;
  tm->tm_yday = cum[m - 1] + (int)d - 1 + (leap && m > 2);
}
long schedule_timeout_interruptible(long timeout) {
  u64 end = lx_now_ms() + jiffies_to_msecs(timeout);
  while (lx_now_ms() < end) if (!lx_pump()) lx_idle(1);
  return 0;
}

/* ---- module parameters ---- */
int param_set_int(const char *val, const struct kernel_param *kp) { return kstrtoint(val, 0, (int *)kp->arg); }
int param_get_int(char *buffer, const struct kernel_param *kp) { return scnprintf(buffer, 4096, "%i\n", *(int *)kp->arg); }

/* ---- files (procfs/debugfs entries are never created on this port) ---- */
int single_open(struct file *f, int (*show)(struct seq_file *, void *), void *data) { (void)f; (void)show; (void)data; return -ENODEV; }
int single_release(struct inode *inode, struct file *f) { (void)inode; (void)f; return 0; }
ssize_t seq_read(struct file *f, char *buf, size_t size, loff_t *ppos) { (void)f; (void)buf; (void)size; (void)ppos; return 0; }
loff_t seq_lseek(struct file *f, loff_t offset, int whence) { (void)f; (void)offset; (void)whence; return -29; /* ESPIPE */ }
void seq_vprintf(struct seq_file *m, const char *fmt, va_list args) { (void)m; (void)fmt; (void)args; }

/* ---- notifiers and registration ---- */
int register_inetaddr_notifier(struct notifier_block *nb) { (void)nb; return 0; }
int unregister_inetaddr_notifier(struct notifier_block *nb) { (void)nb; return 0; }
int register_inet6addr_notifier(struct notifier_block *nb) { (void)nb; return 0; }
int unregister_inet6addr_notifier(struct notifier_block *nb) { (void)nb; return 0; }
int register_netevent_notifier(struct notifier_block *nb) { (void)nb; return 0; }
int unregister_netevent_notifier(struct notifier_block *nb) { (void)nb; return 0; }
int register_netdev(struct net_device *dev) { rtnl_lock(); int r = register_netdevice(dev); rtnl_unlock(); return r; }
void unregister_netdev(struct net_device *dev) { rtnl_lock(); unregister_netdevice(dev); rtnl_unlock(); }
int devm_request_irq(struct device *dev, unsigned int irq, irqreturn_t (*handler)(int, void *), unsigned long flags,
                     const char *name, void *dev_id) {
  (void)dev; return lx_request_irq(irq, handler, NULL, flags, name, dev_id);
}

/* ---- wakeup sources: bookkeeping only (no system suspend) ---- */
struct wakeup_source *wakeup_source_register(struct device *dev, const char *name) {
  struct wakeup_source *ws = kzalloc(sizeof *ws, GFP_KERNEL);
  if (ws) { ws->name = name; ws->dev = dev; }
  return ws;
}
void wakeup_source_unregister(struct wakeup_source *ws) { kfree(ws); }
void __pm_stay_awake(struct wakeup_source *ws) { if (ws) { ws->active = 1; ws->active_count++; } }
void __pm_relax(struct wakeup_source *ws) { if (ws) { ws->active = 0; ws->relax_count++; } }
void __pm_wakeup_event(struct wakeup_source *ws, unsigned int msec) { (void)msec; if (ws) ws->event_count++; }

/* ---- DMA, pages, scatterlists ---- */
phys_addr_t virt_to_phys(const volatile void *addr) {
  return lx_plat && lx_plat->dma_map ? lx_plat->dma_map((void *)addr, 1, 0) : 0;   /* bus address in the DMA window */
}
void sg_init_table(struct scatterlist *sgl, unsigned int nents) { memset(sgl, 0, sizeof *sgl * nents); }
void sg_set_buf(struct scatterlist *sg, const void *buf, unsigned int buflen) { sg->page_link = (unsigned long)buf; sg->offset = 0; sg->length = buflen; }
void sg_init_one(struct scatterlist *sg, const void *buf, unsigned int buflen) { sg_init_table(sg, 1); sg_set_buf(sg, buf, buflen); }
phys_addr_t sg_phys(struct scatterlist *sg) { return virt_to_phys((void *)(sg->page_link + sg->offset)); }
int dma_get_sgtable(struct device *dev, struct sg_table *sgt, void *cpu_addr, dma_addr_t dma_addr, size_t size) {
  (void)dev; (void)sgt; (void)cpu_addr; (void)dma_addr; (void)size; return -EOPNOTSUPP;   /* IPA only */
}
/* No page fragment caches: qdf falls back to plain skb allocation. */
void *page_frag_alloc(struct page_frag_cache *nc, unsigned int fragsz, gfp_t gfp) { (void)nc; (void)fragsz; (void)gfp; return NULL; }
void page_frag_free(void *addr) { (void)addr; }

/* ---- skb ---- */
struct sk_buff *__netdev_alloc_skb(struct net_device *dev, unsigned int length, gfp_t gfp) {
  struct sk_buff *skb = alloc_skb(length + NET_SKB_PAD, gfp);
  if (skb) { skb_reserve(skb, NET_SKB_PAD); skb->dev = dev; }
  return skb;
}
struct sk_buff *build_skb(void *data, unsigned int frag_size) { (void)data; (void)frag_size; return NULL; }   /* no page frags */
void skb_frag_ref(struct sk_buff *skb, int f) { get_page(skb_frag_page(&skb_shinfo(skb)->frags[f])); }
dma_addr_t skb_frag_dma_map(struct device *dev, const skb_frag_t *frag, size_t offset, size_t size, enum dma_data_direction dir) {
  (void)dev; return lx_plat->dma_map((u8 *)skb_frag_address(frag) + offset, size, (int)dir);
}
void skb_coalesce_rx_frag(struct sk_buff *skb, int i, int size, unsigned int truesize) {
  skb_shinfo(skb)->frags[i].size += (unsigned)size;
  skb->len += (u32)size; skb->data_len += (u32)size; skb->truesize += truesize;
}
struct sk_buff *skb_unshare(struct sk_buff *skb, gfp_t pri) {
  if (!skb->cloned) return skb;
  struct sk_buff *n = skb_copy(skb, pri);
  kfree_skb(skb);
  return n;
}
void napi_gro_flush(struct napi_struct *napi, bool flush_old) { (void)napi; (void)flush_old; }   /* napi_gro_receive delivers at once */
unsigned int tcp_hdrlen(const struct sk_buff *skb) { return tcp_hdr(skb)->doff * 4u; }

/* ---- checksums ---- */
static u32 csum_add_words(const u8 *p, size_t len, u32 sum) {
  for (; len > 1; len -= 2, p += 2) sum += (u32)p[0] << 8 | p[1];
  if (len) sum += (u32)p[0] << 8;
  return sum;
}
static u16 csum_fold32(u32 sum) { while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16); return (u16)~sum; }
__sum16 ip_fast_csum(const void *iph, unsigned int ihl) { return (__sum16)htons(csum_fold32(csum_add_words(iph, ihl * 4u, 0))); }
__sum16 csum_tcpudp_magic(__be32 saddr, __be32 daddr, u32 len, u8 proto, __wsum sum) {
  u32 s = (u32)sum; const u8 *a = (const u8 *)&saddr, *b = (const u8 *)&daddr;
  s = csum_add_words(a, 4, s); s = csum_add_words(b, 4, s); s += proto; s += len;
  return (__sum16)htons(csum_fold32(s));
}
int ipv6_skip_exthdr(const struct sk_buff *skb, int start, u8 *nexthdrp, __be16 *frag_offp) {
  u8 nh = *nexthdrp; *frag_offp = 0;
  while (nh == IPPROTO_HOPOPTS || nh == IPPROTO_ROUTING || nh == IPPROTO_FRAGMENT || nh == IPPROTO_DSTOPTS) {
    struct ipv6_opt_hdr h;
    if (skb_copy_bits(skb, start, &h, sizeof h)) return -1;
    int len;
    if (nh == IPPROTO_FRAGMENT) {
      __be16 fo; if (skb_copy_bits(skb, start + 2, &fo, sizeof fo)) return -1;
      *frag_offp = fo; if (ntohs(fo) & ~0x7) break;
      len = 8;
    } else len = (h.hdrlen + 1) << 3;
    nh = h.nexthdr; start += len;
  }
  *nexthdrp = nh; return start;
}

/* ---- netlink ---- */
static int nla_validate_one(const struct nlattr *nla, const struct nla_policy *p) {
  int len = nla_len(nla);
  switch (p->type) {
  case NLA_U8: case NLA_S8: return len < 1 ? -ERANGE : 0;
  case NLA_U16: case NLA_S16: case NLA_BE16: return len < 2 ? -ERANGE : 0;
  case NLA_U32: case NLA_S32: case NLA_BE32: return len < 4 ? -ERANGE : 0;
  case NLA_U64: case NLA_S64: case NLA_MSECS: return len < 8 ? -ERANGE : 0;
  case NLA_FLAG: return len ? -ERANGE : 0;
  case NLA_NUL_STRING: return (p->len && len > p->len + 1) || !len || ((const char *)nla_data(nla))[len - 1] ? -EINVAL : 0;
  case NLA_STRING: case NLA_BINARY: return p->len && len > p->len ? -ERANGE : 0;
  case NLA_REJECT: return -EINVAL;
  default: return 0;
  }
}
int nla_parse(struct nlattr **tb, int maxtype, const struct nlattr *head, int len, const struct nla_policy *policy,
              struct netlink_ext_ack *extack) {
  (void)extack;
  memset(tb, 0, sizeof *tb * (size_t)(maxtype + 1));
  const struct nlattr *nla; int rem;
  nla_for_each_attr(nla, head, len, rem) {
    int t = (int)nla_type(nla);
    if (t == 0 || t > maxtype) continue;
    if (policy) { int r = nla_validate_one(nla, &policy[t]); if (r) return r; }
    tb[t] = (struct nlattr *)nla;
  }
  return 0;
}
int nla_memcpy(void *dest, const struct nlattr *src, int count) {
  int n = nla_len(src) < count ? nla_len(src) : count;
  memcpy(dest, nla_data(src), (size_t)n);
  if (n < count) memset((u8 *)dest + n, 0, (size_t)(count - n));
  return n;
}
ssize_t nla_strscpy(char *dst, const struct nlattr *nla, size_t dstsize) {
  if (!dstsize) return -E2BIG;
  size_t srclen = (size_t)nla_len(nla); const char *src = nla_data(nla);
  if (srclen && !src[srclen - 1]) srclen--;
  ssize_t ret = (ssize_t)srclen; size_t n = srclen;
  if (n >= dstsize) { n = dstsize - 1; ret = -E2BIG; }
  memcpy(dst, src, n); dst[n] = 0; return ret;
}
struct nlmsghdr *nlmsg_put(struct sk_buff *skb, u32 portid, u32 seq, int type, int payload, int flags) {
  int size = (int)NLMSG_ALIGN(NLMSG_LENGTH(payload));
  if (skb_tailroom(skb) < size) return NULL;
  struct nlmsghdr *h = skb_put(skb, (unsigned)size);
  h->nlmsg_type = (u16)type; h->nlmsg_len = (u32)NLMSG_LENGTH(payload); h->nlmsg_flags = (u16)flags;
  h->nlmsg_pid = portid; h->nlmsg_seq = seq;
  if (size > (int)NLMSG_LENGTH(payload)) memset((u8 *)h + NLMSG_LENGTH(payload), 0, (size_t)(size - (int)NLMSG_LENGTH(payload)));
  return h;
}
/* No netlink sockets to user space on this port. */
struct sock *netlink_kernel_create(struct net *net, int unit, struct netlink_kernel_cfg *cfg) { (void)net; (void)unit; (void)cfg; return NULL; }
void netlink_kernel_release(struct sock *sk) { (void)sk; }

/* ---- cfg80211 vendor commands/events: no nl80211 user space on this port (the runtime's control path is native) ---- */
struct sk_buff *__cfg80211_alloc_reply_skb(struct wiphy *wiphy, enum nl80211_commands cmd, enum nl80211_attrs attr, int approxlen) {
  (void)wiphy; (void)cmd; (void)attr; return alloc_skb((unsigned)approxlen + 256, GFP_KERNEL);
}
struct sk_buff *__cfg80211_alloc_event_skb(struct wiphy *wiphy, struct wireless_dev *wdev, enum nl80211_commands cmd,
                                           enum nl80211_attrs attr, unsigned int portid, int vendor_event_idx,
                                           int approxlen, gfp_t gfp) {
  (void)wiphy; (void)wdev; (void)cmd; (void)attr; (void)portid; (void)vendor_event_idx; return alloc_skb((unsigned)approxlen + 256, gfp);
}
void __cfg80211_send_event_skb(struct sk_buff *skb, gfp_t gfp) { (void)gfp; kfree_skb(skb); }
int cfg80211_vendor_cmd_reply(struct sk_buff *skb) { kfree_skb(skb); return 0; }
unsigned int cfg80211_vendor_cmd_get_sender(struct wiphy *wiphy) { (void)wiphy; return 0; }
int cfg80211_external_auth_request(struct net_device *netdev, struct cfg80211_external_auth_params *params, gfp_t gfp) {
  (void)netdev; (void)params; (void)gfp; lx_log(4, "wlan: external auth request (SAE offload to user space) not supported"); return -EOPNOTSUPP;
}
void cfg80211_pmksa_candidate_notify(struct net_device *dev, int index, const u8 *bssid, bool preauth, gfp_t gfp) {
  (void)dev; (void)index; (void)bssid; (void)preauth; (void)gfp;
}
void cfg80211_update_owe_info_event(struct net_device *netdev, struct cfg80211_update_owe_info *owe_info, gfp_t gfp) {
  (void)netdev; (void)owe_info; (void)gfp;
}

/* ---- Linux AES library on hostap's AES (renamed in qcacld_kernel.h: the runtime's aes_encrypt is hostap's) ---- */
#undef aes_encrypt
void *aes_encrypt_init(const u8 *key, size_t len);
int aes_encrypt(void *ctx, const u8 *plain, u8 *crypt);
void aes_encrypt_deinit(void *ctx);
/* Only encryption: nothing in qcacld decrypts single AES blocks (the runtime has no hostap AES decrypt).
 * The key bytes are kept in the round-key area; each block call expands them (qcacld uses this for a few blocks per frame at most). */
/* struct aes_key starts with the fields of struct aes_enckey (an anonymous member in Linux's header) */
int lx_linux_aes_preparekey(struct aes_key *key, const u8 *in_key, size_t key_len) {
  if (key_len != 16 && key_len != 24 && key_len != 32) return -EINVAL;
  memset(key, 0, sizeof *key);
  struct aes_enckey *e = (struct aes_enckey *)key; e->len = (u32)key_len; memcpy(e->k.rndkeys, in_key, key_len); return 0;
}
int lx_linux_aes_prepareenckey(struct aes_enckey *key, const u8 *in_key, size_t key_len) {
  if (key_len != 16 && key_len != 24 && key_len != 32) return -EINVAL;
  memset(key, 0, sizeof *key); key->len = (u32)key_len; memcpy(key->k.rndkeys, in_key, key_len); return 0;
}
void lx_linux_aes_encrypt(aes_encrypt_arg key, u8 out[AES_BLOCK_SIZE], const u8 in[AES_BLOCK_SIZE]) {
  const struct aes_enckey *k = key.enc_key;
  void *ctx = aes_encrypt_init((const u8 *)k->k.rndkeys, k->len);
  if (!ctx) { memset(out, 0, AES_BLOCK_SIZE); return; }
  aes_encrypt(ctx, in, out); aes_encrypt_deinit(ctx);
}

/* ---- crypto API (skcipher/aead): qdf_crypto's FILS/GCMP helpers; not provided yet (callers fail with -ENOENT) ---- */
struct crypto_skcipher *crypto_alloc_skcipher(const char *alg_name, u32 type, u32 mask) {
  (void)type; (void)mask; lx_log(4, "wlan: crypto skcipher %s not available", alg_name); return ERR_PTR(-ENOENT);
}
void crypto_free_skcipher(struct crypto_skcipher *tfm) { (void)tfm; }
int crypto_skcipher_setkey(struct crypto_skcipher *tfm, const u8 *key, unsigned int keylen) { (void)tfm; (void)key; (void)keylen; return -ENOENT; }
struct skcipher_request *skcipher_request_alloc(struct crypto_skcipher *tfm, gfp_t gfp) { (void)tfm; (void)gfp; return NULL; }
void skcipher_request_free(struct skcipher_request *req) { (void)req; }
void skcipher_request_set_crypt(struct skcipher_request *req, struct scatterlist *src, struct scatterlist *dst, unsigned int cryptlen, void *iv) {
  (void)req; (void)src; (void)dst; (void)cryptlen; (void)iv;
}
int crypto_skcipher_encrypt(struct skcipher_request *req) { (void)req; return -ENOENT; }
int crypto_skcipher_decrypt(struct skcipher_request *req) { (void)req; return -ENOENT; }
struct crypto_aead *crypto_alloc_aead(const char *alg_name, u32 type, u32 mask) {
  (void)type; (void)mask; lx_log(4, "wlan: crypto aead %s not available", alg_name); return ERR_PTR(-ENOENT);
}
void crypto_free_aead(struct crypto_aead *tfm) { (void)tfm; }
int crypto_aead_setkey(struct crypto_aead *tfm, const u8 *key, unsigned int keylen) { (void)tfm; (void)key; (void)keylen; return -ENOENT; }
int crypto_aead_setauthsize(struct crypto_aead *tfm, unsigned int authsize) { (void)tfm; (void)authsize; return -ENOENT; }
unsigned int crypto_aead_reqsize(struct crypto_aead *tfm) { (void)tfm; return 0; }
void aead_request_set_tfm(struct aead_request *req, struct crypto_aead *tfm) { req->tfm = tfm; }
void aead_request_set_crypt(struct aead_request *req, struct scatterlist *src, struct scatterlist *dst, unsigned int cryptlen, u8 *iv) {
  req->src = src; req->dst = dst; req->cryptlen = cryptlen; req->iv = iv;
}
void aead_request_set_ad(struct aead_request *req, unsigned int assoclen) { req->assoclen = assoclen; }
int crypto_aead_encrypt(struct aead_request *req) { (void)req; return -ENOENT; }

/* ---- WMI: the DBR TLV ops are referenced although DIRECT_BUF_RX is off ---- */
struct wmi_unified;
void wmi_dbr_attach_tlv(struct wmi_unified *wmi_handle) { (void)wmi_handle; }

/* ---- threads: not yet (the module's thread model is the next step); creation fails so qcacld reports it ---- */
struct task_struct *kthread_create_on_node(int (*fn)(void *), void *data, int node, const char *fmt, ...) {
  (void)fn; (void)data; (void)node;
  lx_log(3, "wlan: kthread %s: no threads on this port yet", fmt);
  return ERR_PTR(-EOPNOTSUPP);
}
int wake_up_process(struct task_struct *t) { (void)t; return 0; }
int kthread_stop(struct task_struct *t) { (void)t; return 0; }
bool kthread_should_stop(void) { return true; }
void kthread_complete_and_exit(struct completion *c, long code) { (void)code; if (c) complete(c); for (;;) lx_idle(1000); }
