/* Start the original cfg80211/mac80211 stack and ath11k on the WCN6750's hybrid bus (ath11k_ahb), the WPSS's router as the QRTR endpoint. */
#include "lx_pci.h"
#include "lx_plat.h"
#include <net/mac80211.h>
#include "core.h"
#include "debug.h"
#include "mac.h"
#include "nw_qrtr.h"
#include "nw_mainline.h"
#include "nw_wifi.h"
#include "nw_board.h"
#include "nw_events.h"
#include "nw_control_endpoint.h"

int lx_register_ath11k_ahb_driver(void);
void lx_unregister_ath11k_ahb_driver(void);

struct nw_wifi_stats nw_wifi_stats;
static struct ieee80211_hw *g_hw;
void nw_wifi_hw_ready(struct ieee80211_hw *hw) { g_hw = hw; }
int lx_init_cfg80211_init(void);
int lx_init_ieee80211_init(void);
void lx_exit_cfg80211_exit(void);
void lx_exit_ieee80211_exit(void);
static bool cfg_started, mac_started, ahb_started;

/* The address of a radio whose firmware reports none (or a random default one): the board's file wlan_mac.bin (text, the address as twelve hex digits after the
 * last '='), else a locally administered address made up once. */
static int hexval(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
void nw_board_mac(u8 *a) {
  static u8 made[6]; static int have;
  const u8 *d; size_t n;
  if (!have && lx_plat->fw_get && !lx_plat->fw_get("wlan_mac.bin", &d, &n)) {
    size_t s = 0; for (size_t i = 0; i < n; i++) if (d[i] == '=') s = i + 1;
    u8 m[6]; size_t k = 0;
    for (size_t i = s; i < n && k < 12; i++) { int v = hexval((char)d[i]); if (v < 0) { if (k) break; continue; } m[k / 2] = (u8)(k & 1 ? m[k / 2] << 4 | v : v); k++; }
    if (k == 12 && is_valid_ether_addr(m)) { memcpy(made, m, 6); have = 1; lx_log(6, "wifi: address from wlan_mac.bin"); }
  }
  if (!have) {
    for (int i = 0; i < 6; i++) made[i] = (u8)(lx_random_u32() ^ lx_now_ms() >> (i * 3));
    made[0] = (made[0] & 0xfe) | 0x02; have = 1; lx_log(4, "wifi: the firmware and the board give no address: a made-up one is used");
  }
  memcpy(a, made, 6); }

void nw_wifi_irq_mode(int delivered) { lx_irq_polling(!delivered); }
void nw_wifi_poll(void) { nw_mainline_poll(); nw_wifi_control_poll(); }
void nw_wifi_irq_pending(uint32_t vectors) {
  if (!nw_board) return;
  while (vectors) {
    unsigned bit = __builtin_ctz(vectors); vectors &= vectors - 1;
    lx_irq_raise(nw_board->irq_base + bit);
  }
}
unsigned nw_wifi_wait_ms(unsigned maximum) {
  return nw_mainline_pending() || nw_wifi_control_pending() ? 0 : lx_next_wait_ms(maximum);
}
void nw_wifi_set_platform(const struct lx_platform *p) { lx_plat = p; }
/* The station's transmit and link counters (ath11k keeps per-peer rates in its station records; the mainline runtime's cached values serve). */
void nw_wifi_tx_reasons(char *b, int n) { if (n) b[0] = 0; }
void nw_wifi_link_state(char *text, int size) {
  struct nw_wifi_status state; nw_wifi_status_get(&state);
  text[0] = 0;
  if (!g_hw || state.state != NW_STA_UP) return;
  snprintf(text, size, "freq %u rssi %d tx %u.%u Mbps", state.freq_mhz, state.signal_dbm, state.tx_rate_100kbps / 10, state.tx_rate_100kbps % 10);
}
void nw_wifi_rx_state(char *b, int n) { if (n) b[0] = 0; }
void nw_wifi_ring_state(char *b, int n) { if (n) b[0] = 0; }
void nw_wifi_htt_stats(int type) { (void)type; }
int nw_wifi_start(const struct lx_platform *p, const struct nw_board *b) {
  lx_plat = p; nw_board = b; ath11k_debug_mask = 0; /* Keep info/warnings and counters; skip HTT/WMI debug formatting in normal traffic. */
  nw_wifi_buffer_pool_configure(1024 * 1024);
  lx_root_init(); nw_qrtr_init();
  int r = nw_mainline_init(); if (r) { lx_log(3, "wifi: mainline init failed"); goto fail; }
  r = lx_init_cfg80211_init(); if (r) { lx_log(3, "wifi: cfg80211 init failed"); goto fail; }
  cfg_started = true;
  r = lx_init_ieee80211_init(); if (r) { lx_log(3, "wifi: mac80211 init failed"); goto fail; }
  mac_started = true;
  nw_wifi_stats.stage = 1;
  lx_log(6, "wifi: probing ath11k_ahb");
  r = lx_register_ath11k_ahb_driver();
  if (r) goto fail;
  ahb_started = true; nw_wifi_stats.stage = 2; return 0;
fail:
  nw_wifi_stats.last_err = r; nw_wifi_stop(); lx_log(3, "wifi: driver start failed: %d", r); return r;
}
/* Returns nonzero while traffic moves (frames arrived or frames wait to be sent): the board then calls again at once instead of sleeping.
 * lx_pump's own result is no guide, every handler is offered a look on each call. */
int nw_wifi_step(void) {
  static unsigned last_frames; nw_wifi_control_poll(); lx_pump(); nw_mainline_run();
  static u64 next_signal;
  if (g_hw && lx_now_ms() >= next_signal) {
    next_signal = lx_now_ms() + 1000;
    char link[128]; nw_wifi_link_state(link, sizeof(link));
  }
  const unsigned f = nw_wifi_stats.frames; const int moved = f != last_frames; last_frames = f;
  return moved || nw_mainline_pending() || nw_wifi_control_pending() || lx_next_wait_ms(1000) == 0; }
void nw_wifi_stop(void) {
  nw_mainline_shutdown(); nw_wifi_control_shutdown();
  if (ahb_started) { lx_unregister_ath11k_ahb_driver(); ahb_started = false; }
  nw_wifi_qrtr_down();
  if (mac_started) { lx_exit_ieee80211_exit(); mac_started = false; }
  if (cfg_started) { lx_exit_cfg80211_exit(); cfg_started = false; }
  g_hw = NULL; nw_wifi_buffer_pool_configure(0);
}
