/* The WCN6750's hybrid bus for ath11k_ahb on a board without a Linux device model: one platform device, the few devicetree properties the
 * WCN6750 path reads, its message interrupts as irq numbers base..base+n-1, the WPSS as an already running remote processor, the SMP2P
 * "wlan" state, and the QRTR endpoint to the WPSS's router (packets come from the board on the wireless worker). */
#include "lx_pci.h"
#include "lx_plat.h"
#include "nw_board.h"
#include "nw_qrtr.h"
#include "core.h"

const struct nw_board *nw_board;
struct atomic_notifier_head lx_panic_list;

/* ---- devicetree ---- */
#define NW_RPROC_PHANDLE 0x2e8u   /* any nonzero value: the WPSS is the only remote processor asked for */
int nw_of_read_u32(const char *name, u32 *out) {
  if (!strcmp(name, "qcom,rproc")) { *out = NW_RPROC_PHANDLE; return 0; }
  return -ENOENT; }
/* "interrupts" cells are <type number flags>: index 1 is the first interrupt's SPI number */
int nw_of_read_u32_index(const char *name, u32 index, u32 *out) {
  if (!strcmp(name, "interrupts") && nw_board && nw_board->msi_count && index % 3 == 1 && index / 3 < nw_board->msi_count) {
    *out = nw_board->msi_spi_first + index / 3; return 0; }
  return -ENOENT; }

/* ---- the platform device ---- */
static struct platform_device g_pdev;
static struct device_node g_node = { "wifi" };
static struct resource g_msi;
struct resource *nw_platform_resource(unsigned int type, unsigned int index) {
  if (type != IORESOURCE_MEM || index || !nw_board || !nw_board->msi_addr) return NULL;
  g_msi.start = nw_board->msi_addr; g_msi.end = nw_board->msi_addr + 3; return &g_msi; }
int nw_platform_irq(unsigned int index) {
  if (!nw_board || index >= nw_board->msi_count) return -ENXIO;
  return nw_board->irq_base + (int)index; }
const void *nw_match_data(void) { return (const void *)(uintptr_t)ATH11K_HW_WCN6750_HW10; }
static struct platform_driver *g_drv;
int nw_platform_driver_register(struct platform_driver *drv) {
  if (g_drv) return -EBUSY;
  device_initialize(&g_pdev.dev);
  g_pdev.dev.of_node = &g_node; g_pdev.dev.dma_mask = 0xffffffffull;
  dev_set_name(&g_pdev.dev, "wcn6750");
  g_drv = drv;
  int r = drv->probe(&g_pdev);
  if (r) { g_drv = NULL; lx_log(3, "wifi: ath11k_ahb probe failed: %d", r); }
  return r; }
void nw_platform_driver_unregister(struct platform_driver *drv) {
  if (g_drv != drv) return;
  if (drv->remove) drv->remove(&g_pdev);
  g_drv = NULL; }

void nw_ahb_qrtr_start(void);
/* ---- the remote processor: the board started it; boot waits for its router link ---- */
struct rproc { int unused; };
static struct rproc g_rproc;
struct rproc *nw_rproc_get(u32 phandle) { return phandle == NW_RPROC_PHANDLE ? &g_rproc : NULL; }
int nw_rproc_boot(struct rproc *r) {
  (void)r;
  if (!nw_board || !nw_board->rproc_up) return -ENODEV;
  if (!nw_board->rproc_up(30000)) { lx_log(3, "wifi: the WPSS is not running"); return -ETIMEDOUT; }
  nw_ahb_qrtr_start();
  return 0; }
void nw_rproc_shutdown(struct rproc *r) { (void)r; /* the WPSS stays up: the board owns it */ }

/* ---- SMP2P ---- */
struct qcom_smem_state { int bit; };
static struct qcom_smem_state g_wlan;
struct qcom_smem_state *nw_smem_state_get(const char *name, unsigned int *bit) {
  if (strcmp(name, "wlan-smp2p-out")) return ERR_PTR(-ENOENT);
  if (bit) *bit = 0;
  return &g_wlan; }
int nw_smem_state_update_bits(struct qcom_smem_state *s, u32 mask, u32 value) {
  (void)s; return nw_board && nw_board->smp2p_wlan ? nw_board->smp2p_wlan(mask, value) : -ENODEV; }

/* ---- the QRTR endpoint ---- */
static int qrtr_xmit(void *ctx, const void *pkt, size_t len) {
  (void)ctx; return nw_board && nw_board->qrtr_send ? nw_board->qrtr_send(pkt, len) : -ENETDOWN; }
static int g_endpoint;
/* The endpoint is set (which sends our HELLO) when ath11k first asks for the remote processor, i.e. after the board's link is up. */
void nw_ahb_qrtr_start(void) { if (!g_endpoint) { g_endpoint = 1; nw_qrtr_set_endpoint(qrtr_xmit, NULL); } }
void nw_wifi_qrtr_rx(const void *packet, size_t len) { nw_qrtr_rx(packet, len); }
void nw_wifi_qrtr_down(void) { if (g_endpoint) { g_endpoint = 0; nw_qrtr_endpoint_down(); } }

/* Register windows: the chip's (from QMI's device information, ath11k_qmi_request_device_info) mapped by the board. */
void *lx_ioremap(phys_addr_t pa, size_t n) { return nw_board && nw_board->map_phys ? nw_board->map_phys(pa, n) : NULL; }

/* The open-loop voltage of the chip's CX rail from the QMI capability reply (0 until it comes); the board votes its rail to it. */
u32 nw_wifi_cpr_mv;
void nw_wifi_cpr_report(u32 mv) { nw_wifi_cpr_mv = mv; if (nw_board && nw_board->cpr_voltage) nw_board->cpr_voltage(mv); }
/* The firmware's MHI state words (QMI device information, vendor TLVs 0x12/0x13; 0 until it comes): link state at +0, endpoint
 * awake at +4. The board reads them around its SoC wake handshake. */
u64 nw_wifi_mhi_pa;
/* The host's DMA memory as the firmware is told it (QMI host capability ddr_range, icnss2's): start, size of the board's DMA
 * window as the chip addresses it, the MSA region, the MSI doorbell's page. Set by the board before the module starts; 0 sends none. */
u64 nw_wifi_ddr_range[6];
/* Whether the firmware's WMI host memory requests are answered (1, as the vendor host) or only logged (0); set by the board. */
unsigned nw_wifi_wmi_mem = 1;
/* Which of the vendor host's WMI_INIT values replace ath11k's (bit mask, see tools/prepare-ath11k.sh); set by the board. */
unsigned nw_wifi_wmi_vendor;
/* The vendor host's flow search engine setup after the rings (bit mask: 1 FSE setup, 2 FISA config, see tools/prepare-ath11k.sh); set by the board. */
unsigned nw_wifi_fse;
/* IMPS for the WCN6750 although the staged core.c turns it off (see tools/prepare-ath11k.sh); set by the board. */
unsigned nw_wifi_idle_ps;
/* More of the vendor host's choices (bit mask, see tools/prepare-ath11k.sh) and the DDR type for the QMI host capability; set by the board. */
unsigned nw_wifi_qmi_vendor, nw_wifi_ddr_type = 7;
u32 nw_wifi_mhi_size;
