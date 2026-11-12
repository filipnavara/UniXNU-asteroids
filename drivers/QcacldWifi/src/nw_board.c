/* QcacldWifi: the module's side of the board boundary (nw_abi.h) that does not depend on the driver: the board record, register
 * windows, the QRTR endpoint to the WPSS's router, and the values the board sets before the module starts. Same names as the
 * Ath11kWifi module's nw_ahb.c, so the board code drives either module. */
#include "nw_board.h"
#include "nw_qrtr.h"

const struct nw_board *nw_board;

/* Register windows (the chip's, from QMI's device information; the MHI state page) mapped by the board. */
void *lx_ioremap(phys_addr_t pa, size_t n) { return nw_board && nw_board->map_phys ? nw_board->map_phys(pa, n) : NULL; }

/* ---- the QRTR endpoint ---- */
static int qrtr_xmit(void *ctx, const void *pkt, size_t len) {
  (void)ctx; return nw_board && nw_board->qrtr_send ? nw_board->qrtr_send(pkt, len) : -ENETDOWN; }
static int g_endpoint;
/* Set (sends our HELLO) once the board reports the WPSS and its router link up. */
void nw_board_qrtr_start(void) { if (!g_endpoint) { g_endpoint = 1; nw_qrtr_set_endpoint(qrtr_xmit, NULL); } }
void nw_wifi_qrtr_rx(const void *packet, size_t len) { nw_qrtr_rx(packet, len); }
void nw_wifi_qrtr_down(void) { if (g_endpoint) { g_endpoint = 0; nw_qrtr_endpoint_down(); } }

/* Set by the board before the module starts (the Ath11kWifi module's names).
 * nw_wifi_ddr_range: the DMA window as the chip addresses it (start, size), the WLAN MSA region (start, size), and the IOVA window
 * of icnss2's IPA/doorbell range (start, size); 0 sizes: no ddr_range in the host capability. nw_wifi_ddr_type: the DDR type of
 * the host capability (7 LPDDR4, 8 LPDDR5; 0 none). */
u64 nw_wifi_ddr_range[6];
unsigned nw_wifi_ddr_type = 7;
int of_fdt_get_ddrtype(void) { return (int)nw_wifi_ddr_type; }

/* The open-loop voltage of the chip's CX rail from the QMI capability reply; the board votes its rail to it. */
u32 nw_wifi_cpr_mv;
void nw_wifi_cpr_report(u32 mv) { nw_wifi_cpr_mv = mv; if (mv && nw_board && nw_board->cpr_voltage) nw_board->cpr_voltage(mv); }
