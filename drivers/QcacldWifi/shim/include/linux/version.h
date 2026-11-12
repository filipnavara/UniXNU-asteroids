/* QcacldWifi: qcacld's version guards follow the runtime's Linux headers (cfg80211, netdev, skb from stable v7.0.14). */
#ifndef QCACLD_LINUX_VERSION_H
#define QCACLD_LINUX_VERSION_H
#define KERNEL_VERSION(a, b, c) (((a) << 16) + ((b) << 8) + ((c) > 255 ? 255 : (c)))
#define LINUX_VERSION_CODE KERNEL_VERSION(7, 0, 14)
#define LINUX_VERSION_MAJOR 7
#define LINUX_VERSION_PATCHLEVEL 0
#define LINUX_VERSION_SUBLEVEL 14
#endif
