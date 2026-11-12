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
unsigned nw_wifi_threads_wanted(void) { return 0; }   /* one executor */
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
static int nw_fw_log_verbose_ar(struct ath11k *ar) {
  static const u16 modules[] = { WLAN_MODULE_WAL, WLAN_MODULE_WHAL, WLAN_MODULE_RX, WLAN_MODULE_DATA_TXRX, WLAN_MODULE_HTT, WLAN_MODULE_WMI };
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
  lx_log(ret ? 3 : 6, "wifi: firmware log verbose for WAL, WHAL, RX, DATA_TXRX, HTT, WMI: %d", ret);
  return ret;
}
int nw_wifi_fw_log_verbose(void) {
  if (!g_hw) return -ENODEV;
  return nw_fw_log_verbose_ar(g_hw->priv);
}
/* The same before the receive rings are set up (the copy of dp_rx.c calls it at the start of pdev 0's ath11k_dp_rx_pdev_alloc), so the
 * firmware logs how it takes the host's HTT ring setup, as the vendor host's startup log levels let it; set by the board (wifi-fwlog). */
unsigned nw_wifi_fw_log_early_on;
void nw_wifi_fw_log_early(struct ath11k_base *ab) {
  struct ath11k *ar = ab->pdevs[0].ar;
  if (!nw_wifi_fw_log_early_on || !ar) return;
  lx_log(6, "wifi: firmware log verbose before the ring setup: %d", nw_fw_log_verbose_ar(ar));
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
/* Every REO/WBM/TCL configuration register whose value differed between the vendor stack (idle, wlan0 up) and ours before the scan
 * (logs/regdiff-idle.txt; not the copy engines, which the firmware guards, nor addresses, ring pointers, MSI data, the scattered link
 * list or timestamps): the vendor's values, written in this order (worker only). 39 registers. */
static const struct { u32 off, val; } nw_vendor_cfg[] = {
  {0x0a34004, 0x0000001f},  /* WBM_R0_RELEASE_RING_ENABLE */
  {0x0a3400c, 0x00000003},  /* WBM_R0_LINK_DESC_RING_ENABLE */
  {0x0a34010, 0x0000001f},  /* WBM_R0_MISC_RING_ENABLE */
  {0x0a3421c, 0x00000000},  /* WBM_R0_SW_RELEASE_RING_CONSUMER_INT_SETUP_IX0 */
  {0x0a34884, 0x000000c3},  /* WBM_R0_WBM_IDLE_LINK_RING_MISC */
  {0x0a348a4, 0x00000000},  /* WBM_R0_WBM_IDLE_LINK_RING_CONSUMER_INT_SETUP_IX0 */
  {0x0a3492c, 0x00008008},  /* WBM_R0_WBM2SW0_RELEASE_RING_ID */
  {0x0a34948, 0x00200000},  /* WBM_R0_WBM2SW0_RELEASE_RING_PRODUCER_INT_SETUP */
  {0x0a349dc, 0x00008208},  /* WBM_R0_WBM2SW2_RELEASE_RING_ID */
  {0x0a349f8, 0x00200000},  /* WBM_R0_WBM2SW2_RELEASE_RING_PRODUCER_INT_SETUP */
  {0x0a34a34, 0x00008308},  /* WBM_R0_WBM2SW3_RELEASE_RING_ID */
  {0x0a34a50, 0x00010008},  /* WBM_R0_WBM2SW3_RELEASE_RING_PRODUCER_INT_SETUP */
  {0x0a34a8c, 0x00000000},  /* WBM_R0_WBM2SW4_RELEASE_RING_ID */
  {0x0a34a94, 0x00000080},  /* WBM_R0_WBM2SW4_RELEASE_RING_MISC */
  {0x0a34aa8, 0x00000000},  /* WBM_R0_WBM2SW4_RELEASE_RING_PRODUCER_INT_SETUP */
  {0x0a38000, 0x837ff10d},  /* REO_R0_GENERAL_ENABLE */
  {0x0a38004, 0x66666666},  /* REO_R0_DESTINATION_RING_CTRL_IX_0 */
  {0x0a3800c, 0x66666666},  /* REO_R0_DESTINATION_RING_CTRL_IX_2 */
  {0x0a38010, 0x66666666},  /* REO_R0_DESTINATION_RING_CTRL_IX_3 */
  {0x0a3816c, 0x00000000},  /* REO_R0_SW2REO_RING_CONSUMER_INT_SETUP_IX0 */
  {0x0a381f4, 0x00000110},  /* REO_R0_REO2SW1_RING_ID */
  {0x0a38210, 0x00400000},  /* REO_R0_REO2SW1_RING_PRODUCER_INT_SETUP */
  {0x0a3824c, 0x00000210},  /* REO_R0_REO2SW2_RING_ID */
  {0x0a38268, 0x00400000},  /* REO_R0_REO2SW2_RING_PRODUCER_INT_SETUP */
  {0x0a382a4, 0x00000310},  /* REO_R0_REO2SW3_RING_ID */
  {0x0a382c0, 0x00400000},  /* REO_R0_REO2SW3_RING_PRODUCER_INT_SETUP */
  {0x0a382fc, 0x00000410},  /* REO_R0_REO2SW4_RING_ID */
  {0x0a38318, 0x00400000},  /* REO_R0_REO2SW4_RING_PRODUCER_INT_SETUP */
  {0x0a38404, 0x00000910},  /* REO_R0_REO2TCL_RING_ID */
  {0x0a38420, 0x00010010},  /* REO_R0_REO2TCL_RING_PRODUCER_INT_SETUP */
  {0x0a38564, 0x000186a0},  /* REO_R0_AGING_THRESHOLD_IX_0 */
  {0x0a38568, 0x000186a0},  /* REO_R0_AGING_THRESHOLD_IX_1 */
  {0x0a3856c, 0x000186a0},  /* REO_R0_AGING_THRESHOLD_IX_2 */
  {0x0a385d8, 0x000c0000},  /* REO_R0_MISC_CTL */
  {0x0a444ac, 0x00fac688},  /* TCL_R0_PCP_TID_MAP */
  {0x0a446c4, 0x00000000},  /* TCL_R0_SW2TCL1_RING_CONSUMER_INT_SETUP_IX0 */
  {0x0a4471c, 0x00000000},  /* TCL_R0_SW2TCL2_RING_CONSUMER_INT_SETUP_IX0 */
  {0x0a44774, 0x00000000},  /* TCL_R0_SW2TCL3_RING_CONSUMER_INT_SETUP_IX0 */
  {0x0a447cc, 0x00000000},  /* TCL_R0_SW2TCL_CREDIT_RING_CONSUMER_INT_SETUP_IX0 */
};
void nw_wifi_vendor_config(const char *when) {
  if (!g_hw) return;
  struct ath11k *ar = g_hw->priv; struct ath11k_base *ab = ar->ab; int changed = 0, kept = 0;
  for (unsigned i = 0; i < ARRAY_SIZE(nw_vendor_cfg); i++) {
    if (ath11k_hif_read32(ab, nw_vendor_cfg[i].off) != nw_vendor_cfg[i].val) changed++;
    ath11k_hif_write32(ab, nw_vendor_cfg[i].off, nw_vendor_cfg[i].val);
    if (ath11k_hif_read32(ab, nw_vendor_cfg[i].off) == nw_vendor_cfg[i].val) kept++;
  }
  lx_log(6, "vendor config %s: %d registers, %d were different, %d read back", when, (int)ARRAY_SIZE(nw_vendor_cfg), changed, kept);
}
/* The data-path registers as the vendor stack leaves them (register diff 2026-11-12, logs/regdiff-idle.txt: vendor icnss2 + qcacld,
 * idle with wlan0 up, against ours before the scan): WBM does not feed RXDMA0 link descriptors (LINK_DESC_RING_ENABLE 0x03, ours 0x13
 * with WBM2RXDMA0_LINK_RING_ENABLE), the idle link ring has RING_ID_DISABLE, REO has no RXDMA2REO ring and BAR destination 0, every REO
 * destination indication and the fragment destination go to the firmware (6). ath11k writes none of the enables (the firmware does);
 * this writes the vendor's values and logs them before and after (worker only). */
void nw_wifi_vendor_regs(const char *when) {
  if (!g_hw) return;
  struct ath11k *ar = g_hw->priv; struct ath11k_base *ab = ar->ab;
  const u32 wbm = 0xa34000, reo = 0xa38000;
  u32 link = ath11k_hif_read32(ab, wbm + 0xc), misc = ath11k_hif_read32(ab, wbm + 0x884), gen = ath11k_hif_read32(ab, reo + 0x0),
      ix0 = ath11k_hif_read32(ab, reo + 0x4), ix2 = ath11k_hif_read32(ab, reo + 0xc), mctl = ath11k_hif_read32(ab, reo + 0x5d8);
  lx_log(6, "vendor regs %s before: link %x idle misc %x reo gen %x ix0 %x ix2 %x misc %x", when, link, misc, gen, ix0, ix2, mctl);
  ath11k_hif_write32(ab, wbm + 0xc, link & ~0x10u);
  ath11k_hif_write32(ab, wbm + 0x884, misc | 1u);
  ath11k_hif_write32(ab, reo + 0x0, gen & ~0xef0u);
  ath11k_hif_write32(ab, reo + 0x4, 0x66666666u);
  ath11k_hif_write32(ab, reo + 0x8, 0x66666666u);
  ath11k_hif_write32(ab, reo + 0xc, 0x66666666u);
  ath11k_hif_write32(ab, reo + 0x10, 0x66666666u);
  ath11k_hif_write32(ab, reo + 0x5d8, (mctl & ~0x1e0000u) | 6u << 17);
  lx_log(6, "vendor regs %s after: link %x idle misc %x reo gen %x ix0 %x ix2 %x misc %x", when, ath11k_hif_read32(ab, wbm + 0xc),
         ath11k_hif_read32(ab, wbm + 0x884), ath11k_hif_read32(ab, reo + 0x0), ath11k_hif_read32(ab, reo + 0x4),
         ath11k_hif_read32(ab, reo + 0xc), ath11k_hif_read32(ab, reo + 0x5d8));
}
/* The WCN6750 data-path registers (scripts/gen-wcn-reglist.py, nw_wcn_regs.h) for the register-state diff against the vendor stack
 * (tools/nwregdump, scripts/diff-wcn-regs.py), read through ath11k_hif_read32 (the static windows) as one snapshot per slot (worker
 * only); nw_wifi_reg_log logs a slot's values from an index, 8 a line, and returns where the next call continues (0 when done). */
#include "nw_wcn_regs.h"
static u32 *nw_reg_snap[2];
void nw_wifi_reg_snapshot(int slot) {
  if (!g_hw || slot < 0 || slot > 1) return;
  struct ath11k *ar = g_hw->priv; struct ath11k_base *ab = ar->ab;
  if (!nw_reg_snap[slot]) nw_reg_snap[slot] = kzalloc(NW_WCN_REGS * sizeof(u32), GFP_KERNEL);
  if (!nw_reg_snap[slot]) return;
  for (int i = 0; i < NW_WCN_REGS; i++) nw_reg_snap[slot][i] = ath11k_hif_read32(ab, nw_wcn_regs[i]);
  lx_log(6, "nwreg s%d: snapshot of %d registers", slot, NW_WCN_REGS);
}
int nw_wifi_reg_log(int slot, int from, int lines) {
  if (slot < 0 || slot > 1 || !nw_reg_snap[slot]) return 0;
  const u32 *v = nw_reg_snap[slot];
  for (int n = 0; n < lines && from < NW_WCN_REGS; n++, from += 8) {
    u32 w[8] = {};
    for (int k = 0; k < 8 && from + k < NW_WCN_REGS; k++) w[k] = v[from + k];
    lx_log(6, "nwreg s%d %d: %08x %08x %08x %08x %08x %08x %08x %08x", slot, from, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
  }
  return from < NW_WCN_REGS ? from : 0;
}
/* The WBM idle list (worker only): the link descriptor addresses the host listed in the WBM idle ring, sorted, to find one listed twice
 * (the firmware asserts in whalRxMpduPop when two receive entries name the same link descriptor) and any outside the link descriptor
 * banks; then the banks. */
static void nw_sift_u64(u64 *a, u32 r, u32 n) {
  for (;;) { u32 c = 2 * r + 1; if (c >= n) return; if (c + 1 < n && a[c + 1] > a[c]) c++; if (a[r] >= a[c]) return;
    u64 t = a[r]; a[r] = a[c]; a[c] = t; r = c; }
}
static void nw_sort_u64(u64 *a, u32 n) {  /* heap sort */
  for (u32 start = n / 2; start > 0;) nw_sift_u64(a, --start, n);
  for (u32 end = n; end > 1;) { end--; u64 t = a[0]; a[0] = a[end]; a[end] = t; nw_sift_u64(a, 0, end); }
}
void nw_wifi_idle_check(const char *when) {
  if (!g_hw) return;
  struct ath11k *ar = g_hw->priv; struct ath11k_base *ab = ar->ab; struct dp_srng *idle = &ab->dp.wbm_idle_ring;
  if (!idle->vaddr) return;
  struct hal_srng *s = &ab->hal.srng_list[idle->ring_id];
  const u32 esz = s->entry_size ? s->entry_size : 2, n = s->num_entries;
  const u32 *w = s->ring_base_vaddr;
  u64 *a = vmalloc((size_t)n * sizeof(u64));
  if (!a || !w) { lx_log(3, "wifi: idle list %s: no memory", when); if (a) vfree(a); return; }
  u32 zero = 0, outside = 0, used = 0; u64 first_out = 0;
  for (u32 i = 0; i < n; i++) {
    const u64 addr = (u64)w[i * esz] | (u64)(w[i * esz + 1] & 0xff) << 32;
    if (!addr) { zero++; continue; }
    int in = 0;
    for (int b = 0; b < DP_LINK_DESC_BANKS_MAX; b++) {
      const struct dp_link_desc_bank *k = &ab->dp.link_desc_banks[b];
      if (k->size && addr >= k->paddr && addr < k->paddr + k->size) { in = 1; break; }
    }
    if (!in && !outside++) first_out = addr;
    a[used++] = addr;
  }
  nw_sort_u64(a, used);
  u32 repeated = 0; u64 first_rep = 0;
  for (u32 i = 1; i < used; i++) if (a[i] == a[i - 1] && !repeated++) first_rep = a[i];
  lx_log(repeated || outside ? 3 : 6, "wifi: idle list %s: %u entries, %u empty, %u twice (first %llx)", when, n, zero, repeated,
         (unsigned long long)first_rep);
  lx_log(outside ? 3 : 6, "wifi: idle list: %u outside the banks (first %llx), %llx..%llx", outside, (unsigned long long)first_out,
         (unsigned long long)(used ? a[0] : 0), (unsigned long long)(used ? a[used - 1] : 0));
  for (int b = 0; b < DP_LINK_DESC_BANKS_MAX; b++) {
    const struct dp_link_desc_bank *k = &ab->dp.link_desc_banks[b];
    if (k->size) lx_log(6, "wifi: link desc bank %d: %llx + %x", b, (unsigned long long)k->paddr, k->size);
  }
  vfree(a);
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
