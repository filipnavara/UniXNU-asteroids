/* QcacldWifi: what qcacld-3.0 expects from a Linux kernel build before any shim header (forced include, first). */
#ifndef QCACLD_PRELUDE_H
#define QCACLD_PRELUDE_H
#define __LITTLE_ENDIAN_BITFIELD 1
#define __LITTLE_ENDIAN 1234
#define __BIG_ENDIAN 4321
#define __BYTE_ORDER __LITTLE_ENDIAN
typedef unsigned char u_int8_t;
typedef unsigned short u_int16_t;
typedef unsigned int u_int32_t;
typedef unsigned long long u_int64_t;
typedef unsigned int u_int;
typedef unsigned int uint;

/* Fields of Linux's sk_buff / skb_shared_info / net_device that qcacld uses beyond the runtime's (lx_skb.h hooks). */
#define LX_SKB_EXTRA \
  unsigned long dev_scratch; unsigned int csum; unsigned char sw_hash, l4_hash, csum_valid, csum_level, peeked, nf_trace;
#define LX_SHINFO_EXTRA \
  unsigned int gso_type; unsigned short gso_segs; unsigned char tx_flags;
#define LX_NETDEV_EXTRA \
  unsigned long state; int irq; unsigned short dev_id; unsigned char name_assign_type; void *ml_priv; \
  struct net_device_stats *tstats; unsigned int watchdog_timeo; unsigned char perm_addr_len; \
  const void *header_ops; unsigned char broadcast[32]; struct in_device *ip_ptr;
#define LX_HAVE_INET6_STRUCTS 1
#define LX_NAPI_EXTRA int rx_count;
/* cfg80211 (Linux 7.0) features qcacld detects through kernel config on Android kernels */
#define CFG80211_MLO_KEY_OPERATION_SUPPORT 1
struct file; struct inode; struct seq_file;
#define LX_SOCK_EXTRA unsigned short sk_tx_queue_mapping; void *sk_dst_cache;
#define LX_NETSTATS_EXTRA \
  u64 rx_errors, tx_errors, multicast, collisions, rx_length_errors, rx_over_errors, rx_crc_errors, rx_frame_errors, \
  rx_fifo_errors, rx_missed_errors, tx_aborted_errors, tx_carrier_errors, tx_fifo_errors, tx_heartbeat_errors, \
  tx_window_errors, rx_compressed, tx_compressed;
struct ifreq; struct in_device; struct net_device; struct sk_buff; struct net_device_stats; struct rtnl_link_stats64;
#define LX_NDO_EXTRA \
  void (*ndo_tx_timeout)(struct net_device *, unsigned int); \
  struct net_device_stats *(*ndo_get_stats)(struct net_device *); \
  int (*ndo_siocdevprivate)(struct net_device *, struct ifreq *, void *, int); \
  int (*ndo_do_ioctl)(struct net_device *, struct ifreq *, int); \
  int (*ndo_change_mtu)(struct net_device *, int); \
  unsigned long long (*ndo_fix_features)(struct net_device *, unsigned long long); \
  int (*ndo_set_features)(struct net_device *, unsigned long long);
#endif
