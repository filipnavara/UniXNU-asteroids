/* QcacldWifi: the rest of the vendor platform API qcacld uses (cnss_utils, cnss_prealloc, cnss_genl), small on this port.
 * - MAC addresses: none from the platform (the vendor's comes from the modem's DMS service or provisioning); qcacld then reads
 *   wlan_mac.bin itself (hdd_update_mac_config) and otherwise takes the firmware's address.
 * - No preallocation pool: qdf allocates from the heap.
 * - cld80211: a family object for qcacld's netlink services; there are no netlink clients on this port, so command callbacks are
 *   only recorded and multicasts go nowhere (the runtime's genetlink drops them for an unregistered family). */
#include "lx_pci.h"
#include <net/genetlink.h>
#include <net/cnss_utils.h>
#include <net/cnss_prealloc.h>
#include <net/cnss_nl.h>

/* ---- cnss_utils ---- */
u8 *cnss_utils_get_wlan_mac_address(struct device *dev, uint32_t *num) { (void)dev; *num = 0; return NULL; }
u8 *cnss_utils_get_wlan_derived_mac_address(struct device *dev, uint32_t *num) { (void)dev; *num = 0; return NULL; }
int cnss_utils_get_wlan_unsafe_channel(struct device *dev, u16 *unsafe_ch_list, u16 *ch_count, u16 buf_len) {
  (void)dev; (void)unsafe_ch_list; (void)buf_len; *ch_count = 0; return 0;   /* no LTE coexistence input */
}
static int g_load_cnt;
void cnss_utils_increment_driver_load_cnt(struct device *dev) { (void)dev; g_load_cnt++; }
/* The DFS non-occupancy list survives driver restarts in the platform driver: kept here for the module's lifetime. */
static u8 g_dfs_nol[1024]; static u16 g_dfs_nol_len;
int cnss_utils_wlan_set_dfs_nol(struct device *dev, const void *info, u16 info_len) {
  (void)dev;
  if (!info || !info_len) return -EINVAL;
  if (info_len > sizeof g_dfs_nol) return -ENOMEM;
  memcpy(g_dfs_nol, info, info_len); g_dfs_nol_len = info_len; return 0;
}
int cnss_utils_wlan_get_dfs_nol(struct device *dev, void *info, u16 info_len) {
  (void)dev;
  if (!info || !info_len) return -EINVAL;
  if (!g_dfs_nol_len) return -ENOENT;
  u16 n = info_len < g_dfs_nol_len ? info_len : g_dfs_nol_len;
  memcpy(info, g_dfs_nol, n); return n;
}

/* ---- cnss_prealloc ---- */
void *wcnss_prealloc_get(size_t size) { (void)size; return NULL; }
int wcnss_prealloc_put(void *ptr) { (void)ptr; return 0; }

/* ---- cnss_genl (cld80211) ---- */
static struct genl_family g_cld80211 = { .version = 1 };   /* "cld80211"; id 0: never registered */
static struct { cld80211_cb cb; void *ctx; } g_cmd[CLD80211_MAX_COMMANDS];
struct genl_family *cld80211_get_genl_family(void) { return &g_cld80211; }
int register_cld_cmd_cb(u8 cmd_id, cld80211_cb cb, void *cb_ctx) {
  if (cmd_id >= CLD80211_MAX_COMMANDS || !cb) return -EINVAL;
  if (g_cmd[cmd_id].cb) return -EEXIST;
  g_cmd[cmd_id].cb = cb; g_cmd[cmd_id].ctx = cb_ctx; return 0;
}
int deregister_cld_cmd_cb(u8 cmd_id) {
  if (cmd_id >= CLD80211_MAX_COMMANDS) return -EINVAL;
  if (!g_cmd[cmd_id].cb) return -EINVAL;
  g_cmd[cmd_id].cb = NULL; g_cmd[cmd_id].ctx = NULL; return 0;
}
