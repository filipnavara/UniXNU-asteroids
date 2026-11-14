/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The Bluetooth UART of the 3a: QUPv3 wrapper 1 SE4 (stock DT qupv3_se11_4uart, "hsuart0"), wired to the WCN6750.
 * From UniXNU-sm8550-common kits/Sm8550Platform/Sm8550GeniUart.h and Sm8550Gcc.c (BSD-2-Clause-Patent), with the 3a's addresses
 * (gcc-milos.c, pinctrl-milos.c of Linux v6.18.60; the stock devicetree). */
#ifndef ASTEROIDS_GENI_UART_H
#define ASTEROIDS_GENI_UART_H
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define ASTEROIDS_BT_UART_BASE 0x00890000ULL   /* qcom,qup_uart@890000 */
#define ASTEROIDS_QUP1_BASE 0x008c0000ULL      /* qcom,qupv3_1_geni_se@8c0000 */
#define ASTEROIDS_BT_UART_INTID 618u           /* GIC SPI 586 */
#define ASTEROIDS_QUP1_SID 0x23u               /* qupv3_1 iommus = <&apps_smmu 0x23 0x0> */
/* GCC: the wrapper 1 vote register (core 0, s_ahb 1, m_ahb 2, core_2x 3, s0-s6 4-10, qspi 30), SE4's RCG and branch. */
#define ASTEROIDS_GCC_QUP1_VOTE 0x52010u
#define ASTEROIDS_GCC_QUP1_S4_RCG 0x1e3c8u
#define ASTEROIDS_GCC_QUP1_S4_CBCR 0x1e3bcu
struct asteroids_geni_clock_io {
    void *context;
    uint32_t (*read)(void *,uint32_t);
    void (*write)(void *,uint32_t,uint32_t);
    void (*delay_us)(void *,unsigned);
};
/* The UART engine is stopped before the source changes. GPLL0-even is 300 MHz (gcc-milos.c ftbl_gcc_qupv3_wrap0_s4_clk_src). */
int asteroids_geni_bt_uart_source(const struct asteroids_geni_clock_io *,uint32_t baud,uint32_t *hz);
/* Serialize shared GCC votes from worker context. Returns false on allocation failure. */
bool asteroids_gcc_update32(volatile uint32_t *,uint32_t clear,uint32_t set);
/* Own SE4 and the wrapper's core/AHB votes it newly acquired (the TFA I2C on SE6 leaves its own set). */
bool asteroids_geni_bt_uart_vote(volatile uint32_t *,bool enabled);
#ifdef __cplusplus
}
#endif
#endif
