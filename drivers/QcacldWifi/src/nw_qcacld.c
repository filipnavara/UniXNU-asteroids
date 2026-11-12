/* QcacldWifi: the module's entry points for the board (nw_abi.h), the counterpart of the Ath11kWifi module's nw_wifi.c.
 * Starts the runtime's cfg80211 and station/control path (nw_mainline, nw_cfg80211_control: wpa_supplicant over the native
 * control endpoint), then qcacld (hdd_module_init -> hdd_driver_load -> pld/icnss: src/nw_icnss.c). qcacld runs kthreads, so
 * every entry the board worker calls holds the module lock (lx_enter/lx_leave, see the runtime's thread model).
 * The board's ath11k diagnostics (register checks, firmware diag dumps, vendor register writes) are inert here. */
#include "lx_pci.h"
#include "lx_plat.h"
#include "nw_qrtr.h"
#include "nw_mainline.h"
#include "nw_board.h"
#include "nw_events.h"
#include "nw_control_endpoint.h"

int lx_init_cfg80211_init(void);
void lx_exit_cfg80211_exit(void);
int lx_init_hdd_module_init(void);    /* qcacld's module_init (the QcacldWifi shim makes it callable) */
void lx_exit_hdd_module_exit(void);
void nw_wifi_qrtr_down(void);

struct nw_wifi_stats nw_wifi_stats;
static bool cfg_started, wlan_started;
static struct ieee80211_hw *g_hw;   /* only "ready" bookkeeping: qcacld has no ieee80211_hw */

/* nw_mainline calls this once the primary netdev is up (with mac80211's hw pointer; qcacld's wiphy is not one). */
void nw_wifi_hw_ready(struct ieee80211_hw *hw) { g_hw = hw; nw_wifi_stats.stage = 4; }
/* mac80211's: nw_mainline asks for the hw behind the primary wiphy; a qcacld wiphy has none. */
struct ieee80211_hw *wiphy_to_ieee80211_hw(struct wiphy *wiphy) { (void)wiphy; return NULL; }

/* The board's platform, with the module's embedded files (out/gen/nw_embedded_files.c: the device's WCNSS_qcom_cfg.ini) served
 * before the board's firmware reader. */
struct nw_embedded_file { const char *name; const unsigned char *data; size_t size; };
extern const struct nw_embedded_file nw_embedded_files[];
static struct lx_platform g_plat;
static const struct lx_platform *g_board_plat;
static int fw_get(const char *path, const u8 **data, size_t *size) {
  const char *base = path; for (const char *c = path; *c; c++) if (*c == '/') base = c + 1;
  for (const struct nw_embedded_file *f = nw_embedded_files; f->name; f++)
    if (!strcmp(f->name, base)) { *data = f->data; *size = f->size; return 0; }
  return g_board_plat->fw_get ? g_board_plat->fw_get(path, data, size) : -ENOENT;
}
static const struct lx_platform *module_platform(const struct lx_platform *p) {
  g_board_plat = p; g_plat = *p; g_plat.fw_get = fw_get; return &g_plat;
}
void nw_wifi_set_platform(const struct lx_platform *p) { lx_plat = module_platform(p); }

int nw_wifi_start(const struct lx_platform *p, const struct nw_board *b) {
  lx_plat = module_platform(p); nw_board = b;
  lx_enter();
  nw_wifi_buffer_pool_configure(1024 * 1024);
  lx_root_init(); nw_qrtr_init();
  int r = nw_mainline_init(); if (r) { lx_log(3, "wifi: mainline init failed"); goto fail; }
  r = lx_init_cfg80211_init(); if (r) { lx_log(3, "wifi: cfg80211 init failed"); goto fail; }
  cfg_started = true; nw_wifi_stats.stage = 1;
  lx_log(6, "wifi: starting qcacld (hdd_module_init)");
  r = lx_init_hdd_module_init();
  if (r) { lx_log(3, "wifi: qcacld load failed: %d", r); goto fail; }
  wlan_started = true; nw_wifi_stats.stage = 2;
  lx_leave();
  return 0;
fail:
  nw_wifi_stats.last_err = r;
  lx_leave();
  nw_wifi_stop();
  return r;
}

/* Returns nonzero while traffic moves (frames arrived or wait to be sent): the board then calls again at once. */
int nw_wifi_step(void) {
  lx_enter();
  static unsigned last_frames;
  nw_wifi_control_poll(); lx_pump(); nw_mainline_run();
  const unsigned f = nw_wifi_stats.frames; const int moved = f != last_frames; last_frames = f;
  const int busy = moved || nw_mainline_pending() || nw_wifi_control_pending() || lx_next_wait_ms(1000) == 0;
  lx_leave();
  return busy;
}

void nw_wifi_stop(void) {
  lx_enter();
  nw_mainline_shutdown(); nw_wifi_control_shutdown();
  if (wlan_started) { lx_exit_hdd_module_exit(); wlan_started = false; }
  nw_wifi_qrtr_down();
  if (cfg_started) { lx_exit_cfg80211_exit(); cfg_started = false; }
  g_hw = NULL; nw_wifi_buffer_pool_configure(0);
  lx_leave();
}

void nw_wifi_poll(void) { lx_enter(); nw_mainline_poll(); nw_wifi_control_poll(); lx_leave(); }
void nw_wifi_irq_mode(int delivered) { lx_irq_polling(!delivered); }
void nw_wifi_irq_pending(uint32_t vectors) {
  if (!nw_board) return;
  lx_enter();
  while (vectors) { unsigned bit = __builtin_ctz(vectors); vectors &= vectors - 1; lx_irq_raise(nw_board->irq_base + bit); }
  lx_leave();
}
unsigned nw_wifi_wait_ms(unsigned maximum) {
  lx_enter();
  unsigned ms = nw_mainline_pending() || nw_wifi_control_pending() ? 0 : lx_next_wait_ms(maximum);
  lx_leave();
  return ms;
}
extern unsigned lx_irq_data_path_period_ms;
void nw_wifi_data_path_poll(unsigned ms) { lx_irq_data_path_period_ms = ms; }

void nw_wifi_tx_reasons(char *b, int n) { if (n) b[0] = 0; }
void nw_wifi_link_state(char *text, int size) {
  struct nw_wifi_status state; nw_wifi_status_get(&state);
  text[0] = 0;
  if (state.state != NW_STA_UP) return;
  snprintf(text, size, "freq %u rssi %d tx %u.%u Mbps", state.freq_mhz, state.signal_dbm, state.tx_rate_100kbps / 10, state.tx_rate_100kbps % 10);
}

/* ---- the board's ath11k diagnostics and tuning knobs: inert with qcacld ---- */
u64 nw_wifi_mhi_pa;                   /* the MHI state page (set from QMI device information by nw_icnss.c) */
u32 nw_wifi_mhi_size;
unsigned nw_wifi_wmi_mem = 1, nw_wifi_wmi_vendor, nw_wifi_fse, nw_wifi_idle_ps, nw_wifi_qmi_vendor, nw_wifi_dp_vendor;
unsigned nw_wifi_fw_log_early_on;
void nw_wifi_rx_state(char *b, int n) { if (n) b[0] = 0; }
void nw_fw_diag_dump(unsigned skip, unsigned count) { (void)skip; (void)count; }
void nw_fw_diag_dump_ids(unsigned lo, unsigned hi, unsigned max) { (void)lo; (void)hi; (void)max; }
int nw_fw_wmi_dump(int from, int lines) { (void)from; (void)lines; return 0; }
void nw_wifi_reg_check(const char *when) { (void)when; }
void nw_wifi_idle_check(const char *when) { (void)when; }
void nw_wifi_vendor_config(const char *when) { (void)when; }
void nw_wifi_vendor_regs(const char *when) { (void)when; }
void nw_wifi_reg_snapshot(int slot) { (void)slot; }
int nw_wifi_reg_log(int slot, int from, int lines) { (void)slot; (void)from; (void)lines; return 0; }
int nw_wifi_fw_log_verbose(void) { return -EOPNOTSUPP; }
void nw_wifi_rx_buf_probe(int compare) { (void)compare; }
