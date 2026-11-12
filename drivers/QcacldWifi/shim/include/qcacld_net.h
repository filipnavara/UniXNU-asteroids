/* QcacldWifi: Linux networking definitions qcacld uses beyond the runtime's (forced after qcacld_kernel.h).
 * Layouts follow Linux; the inet/inet6 device lists are filled by the board's address glue (empty until then). */
#ifndef QCACLD_NET_H
#define QCACLD_NET_H

/* ---- IPv4 / IPv6 / ICMPv6 / IGMP / MLD / ARP wire headers ---- */
#define IPPROTO_HOPOPTS 0
#define IPPROTO_ICMP 1
#define IPPROTO_IGMP 2
#define IPPROTO_TCP 6
#define IPPROTO_UDP 17
#define IPPROTO_IPV6 41
#define IPPROTO_ROUTING 43
#define IPPROTO_FRAGMENT 44
#define IPPROTO_ICMPV6 58
#define IPPROTO_NONE 59
#define IPPROTO_DSTOPTS 60
#define IP_MF 0x2000
#define IP_OFFSET 0x1FFF
#define IPV6_FLOWINFO_MASK 0x0FFFFFFFu
#define IPV6_FLOWLABEL_MASK 0x000FFFFFu
#define IPV6_FLOWINFO_PRIORITY 0x0ff00000u
#define IPV6_ADDR_ANY 0x0000U
#define IPV6_ADDR_UNICAST 0x0001U
#define IPV6_ADDR_MULTICAST 0x0002U
#define IPV6_ADDR_LOOPBACK 0x0010U
#define IPV6_ADDR_LINKLOCAL 0x0020U
#define IPV6_ADDR_SITELOCAL 0x0040U
#define IPV6_ADDR_SCOPE_NODELOCAL 0x01
#define IPV6_ADDR_SCOPE_LINKLOCAL 0x02
#define IPV6_ADDR_SCOPE_SITELOCAL 0x05
#define IPV6_ADDR_SCOPE_ORGLOCAL 0x08
#define IPV6_ADDR_SCOPE_GLOBAL 0x0e
#define IFA_F_TENTATIVE 0x40
#define IGMP_HOST_MEMBERSHIP_QUERY 0x11
#define IGMP_HOST_MEMBERSHIP_REPORT 0x12
#define IGMPV2_HOST_MEMBERSHIP_REPORT 0x16
#define IGMP_HOST_LEAVE_MESSAGE 0x17
#define IGMPV3_HOST_MEMBERSHIP_REPORT 0x22
#define IGMPV3_MODE_IS_INCLUDE 1
#define IGMPV3_MODE_IS_EXCLUDE 2
#define IGMPV3_CHANGE_TO_INCLUDE 3
#define IGMPV3_CHANGE_TO_EXCLUDE 4
#define IGMPV3_ALLOW_NEW_SOURCES 5
#define IGMPV3_BLOCK_OLD_SOURCES 6
#define ICMPV6_MGM_QUERY 130
#define ICMPV6_MGM_REPORT 131
#define ICMPV6_MGM_REDUCTION 132
#define ICMPV6_MLD2_REPORT 143
#define MLD2_MODE_IS_INCLUDE 1
#define MLD2_MODE_IS_EXCLUDE 2
#define MLD2_CHANGE_TO_INCLUDE 3
#define MLD2_CHANGE_TO_EXCLUDE 4
#define MLD2_ALLOW_NEW_SOURCES 5
#define MLD2_BLOCK_OLD_SOURCES 6
struct in_addr { __be32 s_addr; };
struct in6_addr { union { u8 u6_addr8[16]; __be16 u6_addr16[8]; __be32 u6_addr32[4]; } in6_u; };
#define s6_addr in6_u.u6_addr8
#define s6_addr16 in6_u.u6_addr16
#define s6_addr32 in6_u.u6_addr32
struct iphdr { u8 ihl:4, version:4; u8 tos; __be16 tot_len, id, frag_off; u8 ttl, protocol; __sum16 check; __be32 saddr, daddr; };
struct ipv6hdr { u8 priority:4, version:4; u8 flow_lbl[3]; __be16 payload_len; u8 nexthdr, hop_limit; struct in6_addr saddr, daddr; };
struct ipv6_opt_hdr { u8 nexthdr, hdrlen; } __attribute__((packed));
struct icmp6hdr { u8 icmp6_type, icmp6_code; __sum16 icmp6_cksum; union { __be32 un_data32[1]; __be16 un_data16[2]; u8 un_data8[4]; } icmp6_dataun; };
struct igmphdr { u8 type, code; __sum16 csum; __be32 group; };
struct igmpv3_grec { u8 grec_type, grec_auxwords; __be16 grec_nsrcs; __be32 grec_mca; __be32 grec_src[]; };
struct igmpv3_report { u8 type, resv1; __sum16 csum; __be16 resv2, ngrec; struct igmpv3_grec grec[]; };
struct mld2_grec { u8 grec_type, grec_auxwords; __be16 grec_nsrcs; struct in6_addr grec_mca; struct in6_addr grec_src[]; };
struct mld2_report { struct icmp6hdr mld2r_hdr; struct mld2_grec mld2r_grec[]; };
#define mld2r_ngrec mld2r_hdr.icmp6_dataun.un_data16[1]
#include <uapi/linux/if_arp.h>
static inline struct iphdr *ip_hdr(const struct sk_buff *skb) { return (struct iphdr *)(skb->head + skb->network_header); }
static inline struct ipv6hdr *ipv6_hdr(const struct sk_buff *skb) { return (struct ipv6hdr *)(skb->head + skb->network_header); }
static inline struct ethhdr *eth_hdr(const struct sk_buff *skb) { return (struct ethhdr *)(skb->head + skb->mac_header); }
static inline __be32 ip6_flowinfo(const struct ipv6hdr *h) { return *(const __be32 *)h & htonl(IPV6_FLOWINFO_MASK); }
static inline __be32 ip6_flowlabel(const struct ipv6hdr *h) { return *(const __be32 *)h & htonl(IPV6_FLOWLABEL_MASK); }
static inline u8 ip6_tclass(__be32 flowinfo) { return ntohl(flowinfo & htonl(IPV6_FLOWINFO_PRIORITY)) >> 20; }
static inline void ip6_flow_hdr(struct ipv6hdr *h, unsigned int tclass, __be32 flowlabel) {
  *(__be32 *)h = htonl(0x60000000 | (tclass << 20)) | flowlabel; }
int ipv6_skip_exthdr(const struct sk_buff *skb, int start, u8 *nexthdrp, __be16 *frag_offp);
static inline __be16 vlan_get_protocol(const struct sk_buff *skb) { return skb->protocol; }

/* ---- skb extras ---- */
#define CHECKSUM_COMPLETE 2
#define NET_RX_SUCCESS 0
#define NET_RX_DROP 1
enum pkt_hash_types { PKT_HASH_TYPE_NONE, PKT_HASH_TYPE_L2, PKT_HASH_TYPE_L3, PKT_HASH_TYPE_L4 };
static inline void skb_set_hash(struct sk_buff *skb, u32 hash, enum pkt_hash_types type) { skb->hash = hash; skb->l4_hash = type == PKT_HASH_TYPE_L4; }
static inline void skb_reset_tail_pointer(struct sk_buff *skb) { skb->tail = skb->data + skb->len; }
static inline unsigned int skb_frag_off(const skb_frag_t *f) { return f->offset; }
static inline void skb_frag_off_set(skb_frag_t *f, unsigned int off) { f->offset = off; }
static inline void *skb_frag_address_safe(const skb_frag_t *f) { return f->page ? skb_frag_address(f) : NULL; }
void skb_frag_ref(struct sk_buff *skb, int f);
dma_addr_t skb_frag_dma_map(struct device *dev, const skb_frag_t *frag, size_t offset, size_t size, enum dma_data_direction dir);
struct sk_buff *__netdev_alloc_skb(struct net_device *dev, unsigned int length, gfp_t gfp);
struct sk_buff *build_skb(void *data, unsigned int frag_size);
/* skb->users is the runtime's plain counter: the refcount_* calls qcacld makes on it map to atomics. */
#define refcount_inc(r) ((void)__atomic_fetch_add((r), 1, __ATOMIC_RELAXED))
#define refcount_dec(r) ((void)__atomic_fetch_sub((r), 1, __ATOMIC_RELEASE))
#define refcount_set(r, v) __atomic_store_n((r), (v), __ATOMIC_RELAXED)
#define refcount_read(r) __atomic_load_n((r), __ATOMIC_RELAXED)
void napi_gro_flush(struct napi_struct *napi, bool flush_old);
#define lx_napi_scheduled(n) READ_ONCE((n)->scheduled)
#define lx_tasklet_scheduled(t) READ_ONCE((t)->pending)
#define lx_tasklet_running(t) 0

/* ---- sockets (tx queue hints of the sending socket: none on this port) ---- */
#define NO_QUEUE_MAPPING 0xffff
#define sk_fullsock(sk) 0
#define sk_tx_queue_set(sk, q) ((void)0)

/* ---- netdev queues: trans_start/state/xmit_lock_owner read for logs only ---- */
#define rtnl_dereference(p) (p)

/* ---- inet / inet6 device address lists (ARP / NS offload) ---- */
struct in_ifaddr { struct in_ifaddr *ifa_next; struct in_device *ifa_dev; __be32 ifa_local, ifa_address, ifa_mask, ifa_broadcast;
  u8 ifa_scope, ifa_prefixlen; u32 ifa_flags; char ifa_label[16]; };
struct ip_mc_list { struct in_device *interface; __be32 multiaddr; struct ip_mc_list *next, *next_rcu; unsigned int sfmode; };
struct in_device { struct net_device *dev; struct in_ifaddr *ifa_list; struct ip_mc_list *mc_list; };
struct inet6_ifaddr { struct in6_addr addr; u32 prefix_len; u32 flags; int scope; struct inet6_dev *idev; struct list_head if_list; };
struct ifacaddr6 { struct in6_addr aca_addr; struct ifacaddr6 *aca_next; };
struct inet6_dev { struct net_device *dev; struct list_head addr_list; struct ifacaddr6 *ac_list; rwlock_t lock; };
#define in_dev_for_each_ifa_rtnl(ifa, in_dev) for (ifa = (in_dev)->ifa_list; ifa; ifa = ifa->ifa_next)
#define in_dev_for_each_ifa_rcu(ifa, in_dev) in_dev_for_each_ifa_rtnl(ifa, in_dev)
#define for_ifa(in_dev) { struct in_ifaddr *ifa; for (ifa = (in_dev)->ifa_list; ifa; ifa = ifa->ifa_next) {
#define endfor_ifa(in_dev) } }
struct in_device *lx_in_dev(const struct net_device *dev);
struct inet6_dev *lx_in6_dev(const struct net_device *dev);
#define __in_dev_get_rtnl(d) lx_in_dev(d)
#define __in_dev_get_rcu(d) lx_in_dev(d)
#define in_dev_get(d) lx_in_dev(d)
#define in_dev_put(i) ((void)0)
#define __in6_dev_get(d) lx_in6_dev(d)
int register_inetaddr_notifier(struct notifier_block *nb);
int unregister_inetaddr_notifier(struct notifier_block *nb);
int register_inet6addr_notifier(struct notifier_block *nb);
int unregister_inet6addr_notifier(struct notifier_block *nb);

/* ---- neighbours (NUD tracking) ---- */
#define NUD_INCOMPLETE 0x01
#define NUD_REACHABLE 0x02
#define NUD_STALE 0x04
#define NUD_DELAY 0x08
#define NUD_PROBE 0x10
#define NUD_FAILED 0x20
#define NUD_NOARP 0x40
#define NUD_PERMANENT 0x80
#define NUD_NONE 0x00
struct neighbour { struct net_device *dev; u8 nud_state; u8 primary_key[16]; u8 ha[32]; };
#define NETEVENT_NEIGH_UPDATE 1
int register_netevent_notifier(struct notifier_block *nb);
int unregister_netevent_notifier(struct notifier_block *nb);

/* ---- wireless extensions events (HDD still builds iwreq_data for wireless_send_event) ---- */
struct iw_point { void *pointer; u16 length, flags; };
struct iw_param { s32 value; u8 fixed, disabled; u16 flags; };
struct iw_freq { s32 m; s16 e; u8 i, flags; };
union iwreq_data { char name[16]; struct iw_point essid, data, encoding; struct iw_param nwid, bitrate, txpower, rts, frag, retry, power,
  sens, param; struct iw_freq freq; u32 mode; struct sockaddr ap_addr, addr; };
struct iw_michaelmicfailure { u32 flags; struct sockaddr src_addr; u8 tsc[8]; };
#define IW_MICFAILURE_KEY_ID 0x00000003
#define IW_MICFAILURE_GROUP 0x00000004
#define IW_MICFAILURE_PAIRWISE 0x00000008
#define IW_MICFAILURE_STAKEY 0x00000010
#define IW_MICFAILURE_COUNT 0x00000060
#define wireless_send_event(dev, cmd, wrqu, extra) ((void)0)
struct ifreq { char ifr_name[16]; union { void *ifru_data; struct sockaddr ifru_addr; int ifru_ivalue; } ifr_ifru; };
#define ifr_data ifr_ifru.ifru_data
#define NETLINK_USERSOCK 2

/* ---- crypto (FILS/PMF helpers in qdf_crypto: the board's crypto provides these) ---- */
#define CRYPTO_ALG_ASYNC 0x00000080
struct skcipher_request;
struct aead_request { struct crypto_aead *tfm; unsigned int assoclen, cryptlen; u8 *iv; struct scatterlist *src, *dst;
  void *__ctx[] __attribute__((aligned(16))); };
struct scatterlist;
struct crypto_skcipher *crypto_alloc_skcipher(const char *alg_name, u32 type, u32 mask);
void crypto_free_skcipher(struct crypto_skcipher *tfm);
int crypto_skcipher_setkey(struct crypto_skcipher *tfm, const u8 *key, unsigned int keylen);
struct skcipher_request *skcipher_request_alloc(struct crypto_skcipher *tfm, gfp_t gfp);
void skcipher_request_free(struct skcipher_request *req);
void skcipher_request_set_crypt(struct skcipher_request *req, struct scatterlist *src, struct scatterlist *dst, unsigned int cryptlen, void *iv);
int crypto_skcipher_encrypt(struct skcipher_request *req);
int crypto_skcipher_decrypt(struct skcipher_request *req);
struct crypto_aead *crypto_alloc_aead(const char *alg_name, u32 type, u32 mask);
void crypto_free_aead(struct crypto_aead *tfm);
int crypto_aead_setkey(struct crypto_aead *tfm, const u8 *key, unsigned int keylen);
int crypto_aead_setauthsize(struct crypto_aead *tfm, unsigned int authsize);
unsigned int crypto_aead_reqsize(struct crypto_aead *tfm);
void aead_request_set_tfm(struct aead_request *req, struct crypto_aead *tfm);
void aead_request_set_crypt(struct aead_request *req, struct scatterlist *src, struct scatterlist *dst, unsigned int cryptlen, u8 *iv);
void aead_request_set_ad(struct aead_request *req, unsigned int assoclen);
int crypto_aead_encrypt(struct aead_request *req);
#endif
