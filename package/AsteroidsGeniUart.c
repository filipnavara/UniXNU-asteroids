/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The Bluetooth UART's clock: the RCG sequence of clk-rcg2.c for gcc_qupv3_wrap1_s4_clk_src (gcc-milos.c, Linux v6.18.60), and the
 * shared GCC vote register. From UniXNU-sm8550-common kits/Sm8550Platform/Sm8550GeniUart.c and Sm8550Gcc.c (BSD-2-Clause-Patent);
 * only the registers and vote bits differ. */
#include "AsteroidsGeniUart.h"
#include <sys/errno.h>
#include <IOKit/IOLocks.h>
int asteroids_geni_bt_uart_source(const struct asteroids_geni_clock_io *io,uint32_t baud,uint32_t *hz) {
    if(!io || !io->read || !io->write || !io->delay_us || !hz)return -EINVAL;
    uint32_t m,n,rate;
    switch(baud) {
        case 115200:m=384;n=15625;rate=7372800;break;
        case 3000000:m=4;n=25;rate=48000000;break;
        case 3200000:m=64;n=375;rate=51200000;break;
        case 4000000:m=16;n=75;rate=64000000;break;
        default:return -EINVAL;
    }
    const uint32_t root=ASTEROIDS_GCC_QUP1_S4_RCG;
    /* This SE is exclusive to the Bluetooth UART; manual RCG mode owns its source (DFSR bit 0 off). */
    uint32_t dfs=io->read(io->context,root+0x14);
    io->write(io->context,root+0x14,dfs&~1u);
    if(io->read(io->context,root+0x14)&1u)return -EIO;
    io->write(io->context,root+8,(io->read(io->context,root+8)&~0xffffu)|m);
    io->write(io->context,root+12,(io->read(io->context,root+12)&~0xffffu)|(~(n-m)&0xffffu));
    io->write(io->context,root+16,(io->read(io->context,root+16)&~0xffffu)|(~n&0xffffu));
    /* CFG: divider 1 (hid 1), source 6 (GPLL0-even, gcc_parent_map_4), dual-edge M/N mode. */
    uint32_t cfg=io->read(io->context,root+4);
    cfg=(cfg&~(0x1fu|0x700u|0x3000u|0x100000u))|1u|(6u<<8)|(2u<<12);
    io->write(io->context,root+4,cfg);
    io->write(io->context,root,io->read(io->context,root)|1u);
    for(unsigned i=0;i<1000;i++) {
        if(!(io->read(io->context,root)&1u)){*hz=rate;return 0;}
        io->delay_us(io->context,1);
    }
    return -ETIMEDOUT;
}
static IOLock *gcc_lock;
static IOLock *smmu_lock;
static uint32_t bt_uart_initial_bus;
static bool bt_uart_claimed;
static IOLock *get_lock(IOLock **slot) {
    IOLock *lock=__atomic_load_n(slot,__ATOMIC_ACQUIRE);
    if(!lock) {
        IOLock *candidate=IOLockAlloc();if(!candidate)return NULL;
        if(__atomic_compare_exchange_n(slot,&lock,candidate,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))lock=candidate;
        else IOLockFree(candidate);
    }
    return lock;
}
/* AppsSmmuLock.h's provider (sheng's names): the Bluetooth DMA's bypass slot is planned under it. Touch and Wi-Fi plan theirs
 * earlier in boot without it. */
bool sm8550_apps_smmu_lock(void) {
    IOLock *lock=get_lock(&smmu_lock);if(!lock)return false;
    IOLockLock(lock);return true;
}
void sm8550_apps_smmu_unlock(void) {IOLockUnlock(smmu_lock);}
bool asteroids_gcc_update32(volatile uint32_t *reg,uint32_t clear,uint32_t set) {
    if(!reg)return false;
    IOLock *lock=get_lock(&gcc_lock);if(!lock)return false;
    IOLockLock(lock);*reg=(*reg&~clear)|set;
    __asm__ volatile("dsb sy" ::: "memory");
    bool ok=(*reg&set)==set && (*reg&(clear&~set))==0;
    IOLockUnlock(lock);return ok;
}
#define BT_UART_BITS (0xfu|(1u<<8))                    /* core, s_ahb, m_ahb, core_2x and SE4 */
#define QUP1_OTHER_SES (0xf0u|(3u<<9)|(1u<<30))        /* SE0-SE3, SE5, SE6, QSPI */
bool asteroids_geni_bt_uart_vote(volatile uint32_t *reg,bool enabled) {
    if(!reg)return false;
    IOLock *lock=get_lock(&gcc_lock);if(!lock)return false;
    IOLockLock(lock);uint32_t value=*reg;
    if(!enabled && !bt_uart_claimed){IOLockUnlock(lock);return true;}
    if(enabled) {
        if(!bt_uart_claimed){bt_uart_initial_bus=value&0xfu;bt_uart_claimed=true;}
        value|=BT_UART_BITS;
    } else {
        value&=~(1u<<8);
        if(!(value&QUP1_OTHER_SES))value&=~(0xfu&~bt_uart_initial_bus);
    }
    *reg=value;__asm__ volatile("dsb sy" ::: "memory");
    bool ok=(*reg&BT_UART_BITS)==(value&BT_UART_BITS);IOLockUnlock(lock);return ok;
}
