/* Start the original cfg80211/mac80211 stack and ath11k on the WCN6750's hybrid bus (ath11k_ahb), the WPSS's router as the QRTR endpoint. */
#include "lx_pci.h"
#include "lx_plat.h"
#include <net/mac80211.h>
#include "core.h"
#include "debug.h"
#include "mac.h"
#include "hif.h"
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
/* How often the polled data path groups run (ms; 0 every round with the copy engines): see lx_irq_raise_all. */
extern unsigned lx_irq_data_path_period_ms;
void nw_wifi_data_path_poll(unsigned ms) { lx_irq_data_path_period_ms = ms; }
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
/* The receive rings as the host sees them (worker only): free entries in the refill and monitor-status refill rings, entries waiting in
 * the REO destination, REO exception, WBM receive release and RXDMA error rings; -1 for a ring that was never set up. */
static int ring_count(struct ath11k_base *ab, struct dp_srng *r, int dst) {
  if (!r->vaddr) return -1;
  struct hal_srng *s = &ab->hal.srng_list[r->ring_id]; spin_lock_bh(&s->lock);
  int n = dst ? ath11k_hal_srng_dst_num_free(ab, s, true) : ath11k_hal_srng_src_num_free(ab, s, true); spin_unlock_bh(&s->lock); return n; }
void nw_wifi_rx_state(char *b, int n) {
  if (n) b[0] = 0;
  if (!g_hw) return;
  struct ath11k *ar = g_hw->priv; struct ath11k_base *ab = ar->ab; struct ath11k_dp *dp = &ab->dp;
  snprintf(b, n, "refill %d mon %d reo %d %d %d %d exc %d rel %d err %d",
           ring_count(ab, &ar->dp.rx_refill_buf_ring.refill_buf_ring, 0), ring_count(ab, &ar->dp.rx_mon_status_refill_ring[0].refill_buf_ring, 0),
           ring_count(ab, &dp->reo_dst_ring[0], 1), ring_count(ab, &dp->reo_dst_ring[1], 1), ring_count(ab, &dp->reo_dst_ring[2], 1),
           ring_count(ab, &dp->reo_dst_ring[3], 1), ring_count(ab, &dp->reo_except_ring, 1), ring_count(ab, &dp->rx_rel_ring, 1),
           ring_count(ab, &ar->dp.rxdma_err_dst_ring[0], 1)); }
void nw_wifi_ring_state(char *b, int n) { if (n) b[0] = 0; }
/* More of the firmware's own log (WMI_DBGLOG_CFG_CMDID, what ath11k's fw_dbglog debugfs file sends): the modules named below at the
 * verbose level and every module enabled. The firmware sends its log in ~1.5 KB reports, so more of it also means the report before
 * an assert is closer to the assert. Worker only, after the radio's pdev exists. */
int nw_wifi_fw_log_verbose(void) {
  if (!g_hw) return -ENODEV;
  struct ath11k *ar = g_hw->priv;
  static const u16 modules[] = { WLAN_MODULE_WAL, WLAN_MODULE_WHAL, WLAN_MODULE_RX, WLAN_MODULE_DATA_TXRX };
  u32 bitmap[MAX_MODULE_ID_BITMAP_WORDS]; int ret = 0;
  mutex_lock(&ar->conf_mutex);
  for (unsigned i = 0; i < ARRAY_SIZE(modules) && !ret; i++) {
    struct ath11k_fw_dbglog cfg = { .param = WMI_DEBUG_LOG_PARAM_LOG_LEVEL };
    cfg.log_level = ATH11K_FW_DBGLOG_VERBOSE; cfg.module_id = modules[i];
    memset(bitmap, 0, sizeof(bitmap));
    ret = ath11k_wmi_fw_dbglog_cfg(ar, bitmap, &cfg);
  }
  if (!ret) {
    struct ath11k_fw_dbglog cfg = { .param = WMI_DEBUG_LOG_PARAM_MOD_ENABLE_BITMAP, .value = MAX_MODULE_ID_BITMAP_WORDS };
    memset(bitmap, 0xff, sizeof(bitmap));
    ret = ath11k_wmi_fw_dbglog_cfg(ar, bitmap, &cfg);
  }
  mutex_unlock(&ar->conf_mutex);
  lx_log(ret ? 3 : 6, "wifi: firmware log verbose for WAL, WHAL, RX, DATA_TXRX: %d", ret);
  return ret;
}
/* Reads back what the host programmed through the register window (worker only): every initialized ring that lives in chip registers
 * (copy engines, TCL, REO, WBM; not the LMAC rings, which the firmware sets up) must show its ring base address in its first register.
 * Logs how many do not and the first few, then the WBM idle link descriptor ring (the list the chip's WBM hands receive DMA its link
 * descriptors from: the firmware counted WBM2RXDMA_LINK ring empty). */
void nw_wifi_reg_check(const char *when) {
  if (!g_hw) return;
  struct ath11k *ar = g_hw->priv; struct ath11k_base *ab = ar->ab; int rings = 0, bad = 0;
  struct { int id; u32 reg, want, got; } shown[8];
  for (int i = 0; i < HAL_SRNG_RING_ID_LMAC1_ID_START; i++) {
    struct hal_srng *s = &ab->hal.srng_list[i];
    if (!s->initialized) continue;
    const u32 reg = s->hwreg_base[HAL_SRNG_REG_GRP_R0], want = (u32)s->ring_base_paddr, got = ath11k_hif_read32(ab, reg);
    rings++;
    if (got != want) { if (bad < 8) { shown[bad].id = i; shown[bad].reg = reg; shown[bad].want = want; shown[bad].got = got; } bad++; }
  }
  lx_log(bad ? 3 : 6, "wifi: reg check %s: %d rings, %d with a wrong base register", when, rings, bad);
  for (int i = 0; i < bad && i < 8; i++)
    lx_log(3, "wifi: reg check ring %d: reg %x wrote %08x reads %08x", shown[i].id, shown[i].reg, shown[i].want, shown[i].got);
  struct dp_srng *idle = &ab->dp.wbm_idle_ring;
  if (idle->vaddr) {
    struct hal_srng *s = &ab->hal.srng_list[idle->ring_id];
    const u32 reg = s->hwreg_base[HAL_SRNG_REG_GRP_R0];
    lx_log(6, "wifi: wbm idle ring id %u: %d entries, sw hp %x cached tp %x, hw hp %x tp %x, base reg %x reads %08x (wrote %08x)", idle->ring_id,
           s->num_entries, s->u.src_ring.hp, s->u.src_ring.cached_tp, ath11k_hif_read32(ab, s->hwreg_base[HAL_SRNG_REG_GRP_R2]),
           ath11k_hif_read32(ab, s->hwreg_base[HAL_SRNG_REG_GRP_R2] + 4), reg, ath11k_hif_read32(ab, reg), (u32)s->ring_base_paddr);
  }
}
/* What the chip did with the receive buffers the host posted (worker only). compare 0 remembers a fingerprint of every posted buffer
 * (descriptor start, the attention and MPDU start tags, the copied 802.11 header); compare 1 counts the buffers that differ since and
 * logs the first few: descriptor words and the header's first 16 bytes (frame control, duration, address 1, start of address 2). Also
 * logs the shared pointer words of the first LMAC rings (refill, per-MAC buffer, ..: host-written wrp and firmware-written rdp) and
 * how much of the per-MAC buffer ring the firmware filled. */
#define NW_RX_PROBE_BUFS 4096
static u32 *rx_sig;
static u32 rx_buf_sig(const u8 *d) {
  u32 w[8]; memcpy(&w[0], d, 8); memcpy(&w[2], d + 72, 4); memcpy(&w[3], d + 132, 4); memcpy(&w[4], d + 256, 4); memcpy(&w[5], d + 264, 12);
  return w[0] ^ w[1] * 3 ^ w[2] * 5 ^ w[3] * 7 ^ w[4] * 11 ^ w[5] * 13 ^ w[6] * 17 ^ w[7] * 19; }
void nw_wifi_rx_buf_probe(int compare) {
  if (!g_hw) return;
  struct ath11k *ar = g_hw->priv; struct ath11k_base *ab = ar->ab; struct dp_rxdma_ring *ring = &ar->dp.rx_refill_buf_ring;
  if (!rx_sig) rx_sig = kzalloc(NW_RX_PROBE_BUFS * sizeof(u32), GFP_KERNEL);
  if (!rx_sig) return;
  struct { int id; u32 w[4]; u8 h[16]; } shown[6]; int posted = 0, changed = 0, id; struct sk_buff *skb;
  spin_lock_bh(&ring->idr_lock);
  idr_for_each_entry(&ring->bufs_idr, skb, id) {
    if (id < 0 || id >= NW_RX_PROBE_BUFS) continue;
    posted++;
    const u32 sig = rx_buf_sig(skb->data);
    if (compare && sig != rx_sig[id]) {
      if (changed < 6) { shown[changed].id = id; memcpy(shown[changed].w, skb->data, 8); memcpy(&shown[changed].w[2], skb->data + 72, 4);
        memcpy(&shown[changed].w[3], skb->data + 132, 4); memcpy(shown[changed].h, skb->data + 264, 16); }
      changed++;
    }
    rx_sig[id] = sig;
  }
  spin_unlock_bh(&ring->idr_lock);
  lx_log(6, "wifi: rx bufs %s: %d posted, %d changed", compare ? "compared" : "remembered", posted, changed);
  for (int i = 0; i < changed && i < 6; i++) {
    const u8 *h = shown[i].h;
    lx_log(6, "wifi: rx buf %d: %08x %08x att %08x mpdu %08x hdr %02x%02x %02x%02x %02x%02x%02x%02x%02x%02x %02x%02x%02x%02x%02x%02x", shown[i].id,
           shown[i].w[0], shown[i].w[1], shown[i].w[2], shown[i].w[3], h[0], h[1], h[2], h[3], h[4], h[5], h[6], h[7], h[8], h[9], h[10], h[11],
           h[12], h[13], h[14], h[15]);
  }
  const u32 *wrp = ab->hal.wrp.vaddr, *rdp = ab->hal.rdp.vaddr;
  if (wrp && rdp) {
    const u32 *r = rdp + HAL_SRNG_RING_ID_LMAC1_ID_START;
    lx_log(6, "wifi: lmac wrp %x %x %x %x %x %x %x %x", wrp[0], wrp[1], wrp[2], wrp[3], wrp[4], wrp[5], wrp[6], wrp[7]);
    lx_log(6, "wifi: lmac rdp %x %x %x %x %x %x %x %x", r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7]);
  }
  struct dp_srng *mac = &ar->dp.rx_mac_buf_ring[0];
  if (mac->vaddr) {
    const u32 *e = (const u32 *)mac->vaddr; int filled = 0; const int entries = mac->size / 8;
    for (int i = 0; i < entries; i++) if (e[2 * i]) filled++;
    lx_log(6, "wifi: mac buf ring: %d of %d entries filled, first %08x %08x, ring id %u", filled, entries, e[0], e[1], mac->ring_id);
  }
}
void nw_wifi_htt_stats(int type) { (void)type; }
int nw_wifi_start(const struct lx_platform *p, const struct nw_board *b) {
  lx_plat = p; nw_board = b; ath11k_debug_mask = b->debug_mask; /* 0: info/warnings and counters only, no HTT/WMI debug formatting in normal traffic. */
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
