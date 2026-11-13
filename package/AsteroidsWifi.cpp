// SPDX-License-Identifier: BSD-2-Clause-Patent
// asteroids: the WiFi module (drivers/Ath11kWifi, Linux ath11k on the WCN6750's hybrid bus) on the Nothing Phone (3a). The WCN6750's WLAN
// firmware runs on the WPSS, which WpssProbe.cpp starts on the ADSP thread; this file runs the module on its own thread ("wifi") and gives it
// what ath11k_ahb gets from Linux: the WPSS's IPC router link (QRTR packets both ways, queued between the two threads), the chip's 32
// message interrupts (GIC SPIs 0x300-0x31f: the chip writes their numbers to 0x17110040), a DMA window the chip reaches through a
// pass-through stream of the apps SMMU, register windows by physical address, firmware files, a clock and a log. sheng's WifiProbe.cpp
// (UniXNU-sheng 516a780) is the model for the platform callbacks and the DMA window's allocator; the PCIe, MHI and PMIC parts have no
// counterpart here.
//
// Devicetree facts (stock 3a devicetree, qcom,wcn6750 "icnss2"): reg msi_addr 0x17110040; interrupts SPI 0x300..0x31f rising edge;
// iommus <&apps_smmu 0x1400 0x1>; dma-coherent; the WLAN SMP2P "wlan" out state (WpssProbe.cpp).
//
// wifi-aj=1 joins the network of join.txt (test builds only, see joinStep). wifi=5 runs it (with the WPSS of wifi>=2). wifi-power=0 leaves the chip's RF supplies as they are. wifi-irq=0 leaves the interrupt lines unattached (the module then polls them every millisecond). wifi-scan=0 skips the self-test scan the board otherwise asks for once the interface is up
// (its networks go to the log). wifi-dma=MIB sizes the DMA window (default 48).
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IORegistryEntry.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOService.h>
#include <kern/thread.h>
#include <sys/errno.h>
#include <pexpert/pexpert.h>
#include <stdarg.h>
#include <sys/kern_control.h>
#include <sys/kpi_mbuf.h>
#include "nw_abi.h"
#include "BoardClasses.h"
#include <libkern/OSKextLib.h>
#include <mach/kmod.h>
#include "nw_control_endpoint.h"
#include "SmmuBypass.h"
#include "AsteroidsRemote.h"
#include "BootResetCore.h"
extern "C" {
#include "QcomRpmh.h"
}

extern "C" int armcpu_spi_attach(uint32_t, bool, void (*)(void *), void *, uint32_t *, uint64_t *) __attribute__((weak_import));
extern "C" int asteroids_wpss_qrtr_attach(void (*rx)(void *, const uint8_t *, uint32_t), void (*state)(void *, int), void *context);
extern "C" int asteroids_wpss_qrtr_up(void);
extern "C" int asteroids_wpss_qrtr_send(const void *packet, uint32_t length);
extern "C" int asteroids_wpss_smp2p_wlan(uint32_t mask, uint32_t value);
extern "C" int32_t asteroids_wpss_smp2p_send(unsigned entry, uint32_t message);
extern "C" int32_t asteroids_wpss_soc_wake_in(void);
extern "C" int asteroids_wpss_aop(const char *message);
extern "C" int32_t asteroids_wpss_aop_result(void);
extern "C" volatile unsigned nw_wifi_idle_ps;  // the module's: IMPS on (wifi-idle-ps, default 1)
extern "C" volatile unsigned nw_wifi_wmi_mem;  // the module's: answer the firmware's WMI host memory requests
extern "C" int asteroids_modem_file(const char *path, uint8_t **data, size_t *size);
extern "C" volatile int32_t native_wpss_stats[16];
extern "C" volatile uint32_t nw_wifi_cpr_mv;  // the module's: the CX voltage the firmware asked for (QMI capability reply)
extern "C" void _disable_preemption(void);
extern "C" void _enable_preemption(void);

// [0] stage (see Stage; negative: the step that failed), [1] last error, [2] interrupts, [3] vectors seen, [4] QRTR packets from the
// WPSS, [5] to it, [6] DMA KiB used, [7] peak, [8] firmware files served | refused << 16, [9] SMMU result (bit per stream | error << 8),
// [10] packets dropped (queue full), [11] scans asked, [12] networks in the last scan, [13] worker rounds / 1024, [14] the module's start
// result, [15] DMA allocation failures
extern "C" {volatile int32_t native_wlan_stats[16]; char native_wlan_bss[224];}
// The module's log: a ring of lines on the heap (the kernel's __DATA is tight), read by the boot log.
extern "C" {char *native_wlan_lines; volatile uint32_t native_wlan_lines_n;}
// The network side (WifiNet.cpp, sheng's): en0, the wireless framework's requests, DHCP. Its classes are registered late with the
// kernel's module (its static initializer, which the build takes out of __mod_init_func), then the interface is made once the
// station is up. It calls back here: native_wifi_logf, sheng_wifi_file (its join.txt), sheng_wifi_wake.
extern kmod_info_t g_kernel_kmod_info;
extern "C" void native_wifi_net_static_init(void) __asm__("__GLOBAL__sub_I_WifiNet.cpp");
extern "C" void sheng_wifi_net_start(const uint8_t mac[6]);

namespace {
enum Stage {StThread = 1, StWpss = 2, StSmmu = 3, StDma = 4, StIrqs = 5, StStarted = 6, StInterface = 7, StScanned = 8};
constexpr uint64_t kSmmu = 0x15000000, kSmmuBytes = 0xd0000;
constexpr uint16_t kStreams[2] = {0x1400, 0x1401};  // iommus <&apps_smmu 0x1400 0x1>: the mask covers both
constexpr uint64_t kMsiAddr = 0x17110040;
constexpr uint32_t kMsiSpi = 0x300, kMsiCount = 32;
constexpr int kIrqBase = 1000;
constexpr unsigned kLines = 256, kLineBytes = 112;
constexpr unsigned kRxQueue = 64;

void fail(int step, int error) {native_wlan_stats[1] = error; native_wlan_stats[0] = -step;}
void stage(int value) {native_wlan_stats[0] = value;}
uint64_t nowNs() {uint64_t ns = 0; absolutetime_to_nanoseconds(mach_absolute_time(), &ns); return ns;}

// ---- log ----
void pLog(int level, const char *line) {
    if (!native_wlan_lines) {IOLog("wifi: %s\n", line); return;}  // before the Wi-Fi worker (the sleep clock at the WPSS start)
    // the per-channel and per-frame debug lines would push the start-up out of the ring
    static uint32_t recent[16];
    uint32_t h = 2166136261u;
    for (const char *c = line; *c; ++c) h = (h ^ static_cast<uint8_t>(*c)) * 16777619u;
    h ^= static_cast<uint32_t>(level);
    for (uint32_t r : recent) if (r == h) return;
    memmove(&recent[1], &recent[0], sizeof recent - sizeof recent[0]);
    recent[0] = h;
    static uint64_t first;
    const uint64_t now = nowNs() / 1000000;
    if (!first) first = now;
    const uint32_t i = __atomic_fetch_add(&native_wlan_lines_n, 1u, __ATOMIC_RELAXED) % kLines;
    snprintf(native_wlan_lines + i * kLineBytes, kLineBytes, "%u %d %.100s", static_cast<uint32_t>(now - first), level, line);
    if (level <= 4) IOLog("wifi: %s\n", line);  // warnings and errors also in the kernel's message buffer
}
void logf(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void logMapping(const char *what, const volatile void *va);
void logf(int level, const char *fmt, ...) {
    char line[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    pLog(level, line);
}

// ---- the DMA window: one physically contiguous block, mapped write-combined, handed out in pieces (sheng's allocator) ----
constexpr unsigned kArenaBlocks = 16384;
struct Arena {uint8_t *va; uint64_t pa, iova; size_t bytes; struct Block {uint32_t off, size; bool used;} *blocks; unsigned count; size_t used, peak; unsigned failures;
              IOBufferMemoryDescriptor *memory; IOMemoryMap *map;} gArena;
// Where the window lives: below 4 GiB, in the modem's carveout (stock devicetree mpss_mem 0x8ac00000 + 230 MiB; UniXNU never starts the
// modem, and DarwinBoot gives XNU only the DRAM above 4 GiB). ath11k gives this chip a 32-bit DMA mask: with the window at its 36-bit
// physical address in XNU's memory the copy engines coped, but the receive DMA lost buffers and the firmware asserted in
// whalRecvDmaLeakRecovery at the first scan. Translating the WLAN stream instead faulted: its boot-loader bank (70) is not the kernel's
// to program (an SError at the first context register write). wifi-dma-pa=ADDR moves the window; wifi-dma-pa=0 allocates it in XNU's
// memory.
constexpr uint64_t kLowWindow = 0x8ac00000, kLowWindowBytes = 230ull << 20;
bool arenaInit(size_t bytes) {
    gArena.blocks = static_cast<Arena::Block *>(IOMallocData(kArenaBlocks * sizeof(Arena::Block)));
    if (!gArena.blocks) return false;
    uint64_t low = kLowWindow;
    PE_parse_boot_argn("wifi-dma-pa", &low, sizeof low);
    if (low && (low & 0x1fffff || (low == kLowWindow && bytes > kLowWindowBytes) || low + bytes > 0x100000000ull)) return false;
    if (low) {
        gArena.pa = low;
    } else {
        gArena.memory = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, (IOOptionBits)kIODirectionInOut | (IOOptionBits)kIOMemoryPhysicallyContiguous,
                                                                         bytes, 0x0000000fffe00000ull);
        if (!gArena.memory || gArena.memory->prepare() != kIOReturnSuccess) return false;
        gArena.pa = gArena.memory->getPhysicalSegment(0, nullptr, kIOMemoryMapperNone);
    }
    gArena.iova = gArena.pa;  // the chip's address of the window (the SMMU passes the streams through)
    // A second, non-cacheable mapping of the same pages: the chip's view and the CPU's then agree without cache maintenance. Write-combined,
    // not kIOMapInhibitCache: on xnu-12377 that is Device memory, where bzero's wide and DC ZVA stores take alignment faults (the first
    // boot panicked in bzero of this window); the board's zzzzzzzzza patch keeps write-combined DRAM mappings Normal non-cacheable.
    IODeviceMemory *device = IODeviceMemory::withRange(gArena.pa, bytes);
    gArena.map = device ? device->map(kIOMapAnywhere | kIOMapWriteCombineCache) : nullptr;
    if (device) device->release();
    if (!gArena.map) return false;
    gArena.va = reinterpret_cast<uint8_t *>(gArena.map->getVirtualAddress());
    gArena.bytes = bytes;
    gArena.blocks[0] = {0, (uint32_t)bytes, false};
    gArena.count = 1;
    bzero(gArena.va, bytes);
    return true;
}
void *arenaAlloc(size_t n, uint64_t *bus) {
    n = (n + 63) & ~(size_t)63;
    if (n >= 4096) n = (n + 4095) & ~(size_t)4095;
    uint32_t align = 64;
    if (n >= 4096) {align = 4096; while (align < n && align < (1u << 20)) align <<= 1;}
    for (unsigned i = 0; i < gArena.count; ++i) {
        Arena::Block &b = gArena.blocks[i];
        if (b.used) continue;
        const uint32_t start = (b.off + align - 1) & ~(align - 1), pad = start - b.off;
        if (b.size < pad + n) continue;
        const bool tail = b.size > pad + n;
        const unsigned extra = (pad ? 1u : 0u) + (tail ? 1u : 0u);
        if (gArena.count + extra > kArenaBlocks) continue;
        const Arena::Block whole = b;
        memmove(&gArena.blocks[i + extra], &gArena.blocks[i], (gArena.count - i) * sizeof(Arena::Block));
        gArena.count += extra;
        unsigned at = i;
        if (pad) {gArena.blocks[at] = {whole.off, pad, false}; ++at;}
        gArena.blocks[at] = {start, (uint32_t)n, true};
        if (tail) gArena.blocks[at + 1] = {start + (uint32_t)n, whole.size - pad - (uint32_t)n, false};
        uint8_t *p = gArena.va + start;
        gArena.used += n;
        if (gArena.used > gArena.peak) gArena.peak = gArena.used;
        bzero(p, n);
        if (bus) *bus = gArena.iova + start;
        native_wlan_stats[6] = (int32_t)(gArena.used >> 10);
        native_wlan_stats[7] = (int32_t)(gArena.peak >> 10);
        return p;
    }
    native_wlan_stats[15] = (int32_t)++gArena.failures;
    logf(3, "wifi: DMA window full: want %zu used %zu of %zu", n, gArena.used, gArena.bytes);
    return nullptr;
}
void arenaFree(void *p) {
    if (!p || (uint8_t *)p < gArena.va || (uint8_t *)p >= gArena.va + gArena.bytes) return;
    const uint32_t off = (uint32_t)((uint8_t *)p - gArena.va);
    for (unsigned i = 0; i < gArena.count; ++i) {
        if (gArena.blocks[i].off != off || !gArena.blocks[i].used) continue;
        gArena.used -= gArena.blocks[i].size;
        gArena.blocks[i].used = false;
        if (i + 1 < gArena.count && !gArena.blocks[i + 1].used) {
            gArena.blocks[i].size += gArena.blocks[i + 1].size;
            memmove(&gArena.blocks[i + 1], &gArena.blocks[i + 2], (gArena.count - i - 2) * sizeof(Arena::Block));
            --gArena.count;
        }
        if (i > 0 && !gArena.blocks[i - 1].used) {
            gArena.blocks[i - 1].size += gArena.blocks[i].size;
            memmove(&gArena.blocks[i], &gArena.blocks[i + 1], (gArena.count - i - 1) * sizeof(Arena::Block));
            --gArena.count;
        }
        native_wlan_stats[6] = (int32_t)(gArena.used >> 10);
        return;
    }
}

// ---- the SMMU: the WLAN streams in pass-through (the bypass bank the storage, touch and audio streams share) ----
// The hypervisor (Gunyah) traps this SMMU's registers and emulates each access from the exception syndrome, which carries the
// register and size only for a plain load or store: a load with writeback (the compiler made findSlot's scan "ldr w9, [x8], #4")
// has no valid syndrome, and Gunyah answered it with an SError ("Unhandled implementation specific error": wifi42-44, 49, 50, each
// time in such a loop). So every access is one plain LDR/STR, as Linux's readl/writel.
uint32_t smmuRd(void *ctx, unsigned o) {
    uint32_t v;
    __asm__ volatile("ldr %w0, [%1]" : "=r"(v) : "r"(reinterpret_cast<uintptr_t>(ctx) + o) : "memory");
    return v;
}
void smmuWr(void *ctx, unsigned o, uint32_t v) {
    __asm__ volatile("str %w0, [%1]" : : "r"(v), "r"(reinterpret_cast<uintptr_t>(ctx) + o) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}
// What maps each WLAN stream (every stream-match group that covers it) and the state of its context bank, for the log; the banks seen
// are kept for faultReport().
IOMemoryMap *gSmmuMap;
uint32_t gBanks[4], gBankCount, gPages;
void describeStreams(const struct smmu_io &io, const char *when) {
    const uint32_t idr0 = smmuRd(io.ctx, 0x20), idr1 = smmuRd(io.ctx, 0x24);
    unsigned count = idr0 & 255;
    if (count > 128) count = 128;
    gPages = 1u << (((idr1 >> 28) & 7) + 1);
    for (uint16_t stream : kStreams) {
        for (unsigned i = 0; i < count; ++i) {
            const uint32_t smr = smmuRd(io.ctx, 0x800 + 4 * i);
            if (!(smr & 0x80000000u)) continue;
            const uint32_t mask = (smr >> 16) & 0x7fff;
            if (((smr ^ stream) & 0xffff & ~mask) != 0) continue;
            const uint32_t s2cr = smmuRd(io.ctx, 0xc00 + 4 * i), bank = s2cr & 255, type = (s2cr >> 16) & 3;
            const unsigned cb = (gPages + bank) * 4096u;
            logf(6, "wifi: smmu %s: stream %x smr[%u]=%08x s2cr=%08x bank %u sctlr=%08x cbar=%08x cba2r=%08x ttbr0=%08x%08x", when, stream, i,
                 smr, s2cr, bank, type ? 0 : smmuRd(io.ctx, cb), smmuRd(io.ctx, 0x1000 + 4 * bank), smmuRd(io.ctx, 0x1800 + 4 * bank),
                 type ? 0 : smmuRd(io.ctx, cb + 0x24), type ? 0 : smmuRd(io.ctx, cb + 0x20));
            bool known = false;
            for (unsigned k = 0; k < gBankCount; ++k) known |= gBanks[k] == bank;
            if (!type && !known && gBankCount < 4) gBanks[gBankCount++] = bank;
        }
    }
}
// SMMU faults: the global fault status and each WLAN bank's fault status, address and syndrome, when they change.
void faultReport() {
    if (!gSmmuMap) return;
    void *ctx = reinterpret_cast<void *>(gSmmuMap->getVirtualAddress());
    static uint32_t last[5];
    const uint32_t gfsr = smmuRd(ctx, 0x48);
    if (gfsr != last[4]) {last[4] = gfsr; if (gfsr) logf(3, "wifi: smmu gfsr=%08x gfsynr0=%08x gfsynr1=%08x", gfsr, smmuRd(ctx, 0x50), smmuRd(ctx, 0x54));}
    for (unsigned k = 0; k < gBankCount; ++k) {
        const unsigned cb = (gPages + gBanks[k]) * 4096u;
        const uint32_t fsr = smmuRd(ctx, cb + 0x58);
        if (fsr == last[k]) continue;
        last[k] = fsr;
        if (fsr) logf(3, "wifi: smmu bank %u fsr=%08x far=%08x%08x fsynr0=%08x fsynr1=%08x", gBanks[k], fsr, smmuRd(ctx, cb + 0x64),
                      smmuRd(ctx, cb + 0x60), smmuRd(ctx, cb + 0x68), smmuRd(ctx, cb + 0x6c));
    }
}
bool setupSmmu() {
    IODeviceMemory *memory = IODeviceMemory::withRange(kSmmu, kSmmuBytes);
    IOMemoryMap *map = memory ? memory->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
    if (memory) memory->release();
    if (!map) return false;
    struct smmu_io io = {reinterpret_cast<void *>(map->getVirtualAddress()), smmuRd, smmuWr};
    describeStreams(io, "before");
    int32_t result = 0;
    unsigned bypassBank = ~0u;
    for (unsigned i = 0; i < 2; ++i) {
        struct smmu_plan plan = smmu_plan_bypass(&io, kStreams[i], gArena.pa, gArena.bytes);
        const char *e = plan.error ? plan.error : smmu_configure_bypass(&io, &plan, kStreams[i]);
        if (!e) {result |= 1 << i; bypassBank = plan.bank; continue;}
        // The boot loader leaves 0x1401, the stream the chip's DMA uses, in a translating bank of its own (bank 70, whose tables know
        // nothing of this window: translation faults at the window's addresses, ctl_resp never came). Its exact stream-match group is
        // pointed at the pass-through bank instead.
        if (plan.slot != 0xffffffffu && bypassBank != ~0u && !strcmp(e, "conflicting stream mapping") &&
            smmuRd(io.ctx, 0x800 + 4 * plan.slot) == (0x80000000u | kStreams[i])) {
            const uint32_t old = smmuRd(io.ctx, 0xc00 + 4 * plan.slot), oldBank = old & 255;
            smmuWr(io.ctx, 0xc00 + 4 * plan.slot, bypassBank);
            const uint32_t now = smmuRd(io.ctx, 0xc00 + 4 * plan.slot);
            const unsigned cb = (plan.pages + oldBank) * 4096u;
            smmuWr(io.ctx, cb + 0x58, smmuRd(io.ctx, cb + 0x58));  // its stale faults (write one to clear)
            logf(6, "wifi: SMMU stream %x: smr[%u] moved from s2cr %08x to %08x (pass-through bank %u)", kStreams[i], plan.slot, old, now, bypassBank);
            if (now == bypassBank) {result |= 1 << i; continue;}
        }
        logf(3, "wifi: SMMU stream %x: %s", kStreams[i], e);
    }
    native_wlan_stats[9] = result;
    describeStreams(io, "after");
    gSmmuMap = map;  // kept for the fault reports
    return (result & 1) != 0;
}

// ---- the chip's supplies: what icnss2 switches on before WLAN (stock devicetree, qcom,wcn6750: vdd-cx-mx smpb3 968-1040 mV,
// vdd-1.8-xo smpb1 1856-2040 mV, vdd-1.3-rfa smpb2 1256-1408 mV, all RPMh VRM; vdd-ipa-2p2 the fixed 2.2 V regulator pm3001a S1J, switched
// by PMR735B GPIO1, output-low by default). Without them the firmware asserted in whal_wmac_transition_to_fw_ctrl at the first vdev.
// The RPMh requests go through active TCS 2 with a fixed wait each (the ADSP thread owns TCS 1 and the RSC's done interrupt); the mode
// (7: HPM), the voltage in mV, then the enable, as sheng's WifiProbe.cpp sends them.
constexpr uint64_t kRscDrv = 0x17a20000, kRscTcs = 0xd00, kCmdDb = 0x81c60000, kCmdDbSize = 0x20000;
constexpr uint64_t kSpmiCore = 0xc400000, kSpmiObsrvr = 0xc440000, kSpmiChnls = 0xc500000, kSpmiCnfg = 0xc42d000;
struct Vrm {const char *name; uint32_t millivolts;};
constexpr Vrm kVrms[] = {{"smpb3", 968}, {"smpb1", 1856}, {"smpb2", 1256}};
volatile uint32_t *mapRegs(uint64_t base, uint64_t size, IOMemoryMap **keep) {
    IODeviceMemory *memory = IODeviceMemory::withRange(base, size);
    IOMemoryMap *map = memory ? memory->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
    if (memory) memory->release();
    *keep = map;
    return map ? reinterpret_cast<volatile uint32_t *>(map->getVirtualAddress()) : nullptr;
}
// Votes the VRMs named (count of them, voltages in mV); returns a bit per rail voted.
int voteVrms(const Vrm *vrms, unsigned count) {
    uint8_t *db = static_cast<uint8_t *>(IOMallocData(kCmdDbSize));
    IOMemoryMap *dbMap = nullptr, *rscMap = nullptr;
    volatile uint32_t *dbw = mapRegs(kCmdDb, kCmdDbSize, &dbMap), *r = mapRegs(kRscDrv, 0x10000, &rscMap);
    int voted = 0;
    if (db && dbw && r) {
        for (uint64_t i = 0; i < kCmdDbSize; i += 4) {const uint32_t w = dbw[i / 4]; memcpy(db + i, &w, 4);}
        const QcomRscLayout layout = qcom_rsc_layout(r[0]);
        constexpr unsigned tcs = 2;
        auto send = [&](uint32_t address, uint32_t value) {
            QcomRpmhWrite writes[16];
            const unsigned n = qcom_rpmh_active_request(&layout, tcs, &address, &value, 1, writes, 16);
            for (unsigned i = 0; i < n; ++i) r[(kRscTcs + writes[i].offset) / 4] = writes[i].value;
            __asm__ volatile("dsb sy" ::: "memory");
            IOSleep(2);
            const unsigned m = qcom_rpmh_finish(&layout, tcs, writes, 16);
            for (unsigned i = 0; i < m; ++i) r[(kRscTcs + writes[i].offset) / 4] = writes[i].value;
            __asm__ volatile("dsb sy" ::: "memory");
        };
        for (unsigned i = 0; i < count; ++i) {
            QcomRpmhResource res;
            if (qcom_cmd_db_find(db, kCmdDbSize, vrms[i].name, &res)) {logf(3, "wifi: rail %s not in the command database", vrms[i].name); continue;}
            send(res.address + 8, 7);
            send(res.address, vrms[i].millivolts);
            send(res.address + 4, 1);
            voted |= 1 << i;
            logf(6, "wifi: rail %s (%x) voted on at %u mV", vrms[i].name, res.address, vrms[i].millivolts);
        }
    }
    if (db) IOFreeData(db, kCmdDbSize);
    if (dbMap) dbMap->release();
    if (rscMap) rscMap->release();
    return voted;
}
// A PMIC5 GPIO over the SPMI arbiter: ppid = SID << 8 | peripheral (GPIOn at 0xb800 + (n-1) * 0x100 for PMK8550 / PMR735B's own base).
// Registers: 0x08 status, 0x40 mode (LV/MV: 0 input, 1 output), 0x41 voltage source, 0x42 pull (5 none), 0x44 output source (bits 3:0
// the function: 0 normal, 2 func1; bit 7 the level for normal), 0x46 enable (bit 7). Logs the registers before and after the writes
// (none: only read). Returns 0, or a negative error (no arbiter channel, a refused write).
struct PmicWrite {uint8_t reg, value;};
int pmicGpio(uint32_t ppid, const char *what, const PmicWrite *writes, unsigned count) {
    IOMemoryMap *core = nullptr, *cnfg = nullptr, *obs = nullptr, *chn = nullptr;
    br_arbiter arb = {mapRegs(kSpmiCore, 0x3000, &core), mapRegs(kSpmiCnfg, 0x4000, &cnfg), 0};
    int rc = -12;
    if (arb.core && arb.cnfg) {
        const uint32_t apids = arb.core[BR_FEATURES / 4] & BR_FEATURES_APIDS_MASK;
        volatile uint32_t *o = mapRegs(kSpmiObsrvr, ((uint64_t)BR_OBS_APID_STRIDE * apids + 0xfff) & ~0xfffull, &obs);
        volatile uint32_t *c = mapRegs(kSpmiChnls, (uint64_t)BR_CHNL_APID_STRIDE * apids, &chn);
        int apid = -1, owner = -1;
        for (uint32_t i = 0; i < apids && i < 1024; ++i) {
            const uint32_t map = arb.core[(BR_APID_MAP + 4 * i) / 4];
            if (!map || ((map >> 8) & 0xfff) != ppid) continue;
            const int o2 = (int)(arb.cnfg[(BR_CNFG_OWNER + 4 * i) / 4] & 7u);
            if (apid < 0 || o2 == arb.ee) {apid = (int)i; owner = o2;}
        }
        if (o && c && apid >= 0) {
            constexpr uint8_t regs[6] = {0x08, 0x40, 0x41, 0x42, 0x44, 0x46};
            uint8_t before[6] = {}, after[6] = {};
            for (unsigned i = 0; i < 6; ++i) br_read8(o, &arb, apid, regs[i], &before[i]);
            rc = 0;
            for (unsigned i = 0; i < count && !rc; ++i) rc = br_write8(c, apid, writes[i].reg, writes[i].value);
            if (count) IOSleep(2);
            for (unsigned i = 0; i < 6; ++i) br_read8(o, &arb, apid, regs[i], &after[i]);
            // two lines: the log keeps 100 characters of each
            logf(6, "wifi: %s: ppid %03x apid %d owner %d", what, ppid, apid, owner);
            logf(6, "wifi: ppid %03x sts/mode/vin/pull/out/en %02x %02x %02x %02x %02x %02x -> %02x %02x %02x %02x %02x %02x rc %d", ppid,
                 before[0], before[1], before[2], before[3], before[4], before[5], after[0], after[1], after[2], after[3], after[4], after[5], rc);
        } else {
            logf(3, "wifi: %s: no arbiter channel for ppid %03x (apid %d)", what, ppid, apid);
        }
    }
    IOMemoryMap *const maps[4] = {core, cnfg, obs, chn};
    for (IOMemoryMap *m : maps) if (m) m->release();
    return rc;
}
// PMR735B (SPMI SID 5) GPIO1 high: the enable of S1J (output, normal function, level high, enabled).
int enablePaSupply() {
    constexpr PmicWrite w[] = {{0x40, 0x01}, {0x44, 0x80}, {0x46, 0x80}};
    return pmicGpio(0x588, "S1J enable (pmr735b gpio1)", w, 3);
}
// PMK8550 (SPMI SID 0) GPIO5: the 32 kHz sleep clock to the WCN's LF_CLK_IN. The stock devicetree's pmk8550_gpios default pinctrl
// (alt_sleep_clk_default: gpio5 func1, output, input disabled, no pull, power source 0), which Android applies at boot and UEFI does
// not. Without it the Fairphone 6 (same SoC, WCN6755 on ath11k) asserted in whal_wmac_transition_to_fw_control:1734 like ours
// (Luca Weiss's fix). Before the WPSS starts: wifi-sleepclk=0 leaves it, 2 only reads it.
int sleepClock() {
    unsigned mode = 1;
    PE_parse_boot_argn("wifi-sleepclk", &mode, sizeof mode);
    if (!mode) return 0;
    constexpr PmicWrite w[] = {{0x40, 0x01}, {0x41, 0x00}, {0x42, 0x05}, {0x44, 0x02}, {0x46, 0x80}};
    return pmicGpio(0x0bc, mode == 2 ? "sleep clock (pmk8550 gpio5), read only" : "sleep clock (pmk8550 gpio5)", w, mode == 2 ? 0 : 5);
}

// ---- interrupts: the chip's message interrupts are GIC SPIs, one per vector ----
volatile uint32_t gPending, gWake;
bool gIrqs;
void wakeWorker() {__atomic_store_n(&gWake, 1u, __ATOMIC_RELEASE); thread_wakeup(event_t(&gWake));}
void msiIrq(void *context) {
    const uint32_t vector = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(context));
    __atomic_fetch_or(&gPending, 1u << vector, __ATOMIC_RELEASE);
    native_wlan_stats[2] = native_wlan_stats[2] + 1;
    native_wlan_stats[3] = native_wlan_stats[3] | (int32_t)(1u << vector);
    wakeWorker();
}
// Attaches the vectors the module has handlers for (ath11k on the WCN6750: copy engines 0-6, data groups 10-12, 14, 16-20; 16 of 32,
// which fit ArmCpu's 24 SPI slots next to the board's other drivers). Until all are attached the module keeps polling.
uint32_t gIrqMask;
int attachIrqs(uint32_t mask) {
    if (!armcpu_spi_attach) return -78;
    if (!mask) return -2;
    for (uint32_t v = 0; v < kMsiCount; ++v) {
        if (!(mask & (1u << v)) || (gIrqMask & (1u << v))) continue;
        int rc = 0;
        for (unsigned tries = 0;; ++tries) {
            rc = armcpu_spi_attach(32 + kMsiSpi + v, true, msiIrq, reinterpret_cast<void *>(uintptr_t(v)), nullptr, nullptr);
            if (rc != kIOReturnNotReady || tries == 40) break;
            IOSleep(250);
        }
        if (rc) return rc;
        gIrqMask |= 1u << v;
    }
    gIrqs = true;
    return 0;
}
void waitWork(unsigned ms) {
    if (__atomic_exchange_n(&gWake, 0u, __ATOMIC_ACQ_REL)) return;
    assert_wait_timeout(event_t(&gWake), THREAD_UNINT, ms, NSEC_PER_MSEC);
    if (__atomic_exchange_n(&gWake, 0u, __ATOMIC_ACQ_REL)) {clear_wait(current_thread(), THREAD_AWAKENED); return;}
    thread_block(THREAD_CONTINUE_NULL);
    __atomic_store_n(&gWake, 0u, __ATOMIC_RELEASE);
}

// ---- the router link: packets from the WPSS arrive on the ADSP thread and wait here for the worker ----
struct RxPacket {uint8_t *data; uint32_t length;};
RxPacket *gRx;
unsigned gRxHead, gRxCount;
IOLock *gRxLock;
volatile int gLinkUp, gLinkDown;
void linkRx(void *, const uint8_t *data, uint32_t length) {
    uint8_t *copy = length ? static_cast<uint8_t *>(IOMallocData(length)) : nullptr;
    if (!copy) {native_wlan_stats[10] = native_wlan_stats[10] + 1; return;}
    memcpy(copy, data, length);
    IOLockLock(gRxLock);
    const bool room = gRxCount < kRxQueue;
    if (room) {gRx[(gRxHead + gRxCount) % kRxQueue] = {copy, length}; gRxCount++;}
    IOLockUnlock(gRxLock);
    if (!room) {IOFreeData(copy, length); native_wlan_stats[10] = native_wlan_stats[10] + 1; return;}
    native_wlan_stats[4] = native_wlan_stats[4] + 1;
    wakeWorker();
}
void linkState(void *, int up) {
    if (up) gLinkUp = 1; else gLinkDown = 1;
    wakeWorker();
}
void drainRx() {
    for (;;) {
        IOLockLock(gRxLock);
        if (!gRxCount) {IOLockUnlock(gRxLock); break;}
        const RxPacket p = gRx[gRxHead];
        gRxHead = (gRxHead + 1) % kRxQueue;
        gRxCount--;
        IOLockUnlock(gRxLock);
        nw_wifi_qrtr_rx(p.data, p.length);
        IOFreeData(p.data, p.length);
    }
    if (gLinkDown) {gLinkDown = 0; nw_wifi_qrtr_down();}
}

// ---- platform callbacks ----
uint64_t pNowNs() {return nowNs();}
uint64_t pBootNs() {uint64_t ns = 0; absolutetime_to_nanoseconds(mach_continuous_time(), &ns); return ns;}
uint64_t pNow() {return nowNs() / 1000000;}
void pSleep(unsigned ms) {IOSleep(ms);}
void pBusyDelayUs(unsigned us) {IODelay(us);}
void pPoll() {
    nw_wifi_irq_mode(gIrqs);
    const uint32_t pending = __atomic_exchange_n(&gPending, 0u, __ATOMIC_ACQ_REL);
    if (pending) nw_wifi_irq_pending(pending);
    drainRx();
    nw_wifi_poll();
}
void pHalt() {for (;;) IOSleep(1000);}
void *hAlloc(size_t n) {
    uint8_t *p = static_cast<uint8_t *>(IOMallocData(n + 16));
    if (!p) return nullptr;
    *reinterpret_cast<size_t *>(p) = n + 16;
    return p + 16;
}
void hFree(void *q) {
    if (!q) return;
    uint8_t *p = static_cast<uint8_t *>(q) - 16;
    IOFreeData(p, *reinterpret_cast<size_t *>(p));
}
void *pDmaAlloc(size_t n, uint64_t *bus) {return arenaAlloc(n, bus);}
void pDmaFree(size_t, void *p, uint64_t) {arenaFree(p);}
uint64_t pDmaMap(void *p, size_t, int) {
    if ((uint8_t *)p < gArena.va || (uint8_t *)p >= gArena.va + gArena.bytes) return 0;
    return gArena.iova + ((uint8_t *)p - gArena.va);
}
void pDmaSync(uint64_t, size_t, int) {}

// The radio's address when the firmware reports none: made from /chosen/unique-chip-id (sheng's boardMac), the same at every boot.
bool boardMac(uint8_t m[6]) {
    IORegistryEntry *chosen = IORegistryEntry::fromPath("/chosen", gIODTPlane);
    OSData *id = chosen ? OSDynamicCast(OSData, chosen->getProperty("unique-chip-id")) : nullptr;
    uint64_t seed = 0;
    if (id && id->getLength() >= sizeof seed) memcpy(&seed, id->getBytesNoCopy(), sizeof seed);
    OSSafeReleaseNULL(chosen);
    if (!seed) return false;
    uint64_t h = 0xcbf29ce484222325ull ^ 0x6750;
    for (unsigned i = 0; i < 8; ++i) h = (h ^ ((seed >> (8 * i)) & 0xff)) * 0x100000001b3ull;
    for (unsigned i = 0; i < 6; ++i) m[i] = (uint8_t)(h >> (8 * i));
    m[0] = (uint8_t)((m[0] & 0xfe) | 0x02);
    return true;
}
// The system's own record of the Wi-Fi address, as a boot loader leaves it in the device tree (sheng 3e679fb): MobileGestalt's WifiAddress and
// WifiAddressData come from /chosen/mac-address-wifi0 and /arm-io/wlan local-mac-address. Without them Settings has no Wi-Fi address and
// the wireless daemon aborts every join that needs a private address. replace: en0's own address once the module reports it.
void publishMac(const uint8_t m[6], bool replace) {
    if (IORegistryEntry *chosen = IORegistryEntry::fromPath("/chosen", gIODTPlane)) {
        if (replace || !chosen->getProperty("mac-address-wifi0")) chosen->setProperty("mac-address-wifi0", const_cast<uint8_t *>(m), 6);
        chosen->release();
    }
    if (IORegistryEntry *wlan = IORegistryEntry::fromPath("/arm-io/wlan", gIODTPlane)) {
        if (replace || !wlan->getProperty("local-mac-address")) wlan->setProperty("local-mac-address", const_cast<uint8_t *>(m), 6);
        wlan->release();
    }
}

// Firmware files by name, each read once and kept for the boot. ath11k asks for board-2.bin, then bdwlan.b<board id> (the vendor's name,
// in image/qca6750/ of the firmware partition), board.bin and regdb.bin; ath11k sends an ELF as such. wifi-bdf=PATH names the board data file in
// the firmware partition; an owner-installed file below /private/var/firmware/ath11k/WCN6750/hw1.0 wins over both.
struct FwCache {char name[24]; uint8_t *data; size_t size; bool tried;} gFw[8];
int readVfs(const char *name, uint8_t **data, size_t *size);
int pFwGet(const char *path, const uint8_t **data, size_t *size) {
    const char *base = path;
    for (const char *c = path; *c; ++c) if (*c == '/') base = c + 1;
    if (!strcmp(base, "wlan_mac.bin")) {
        static char text[40];
        static size_t length;
        uint8_t m[6];
        if (!length && boardMac(m)) length = (size_t)snprintf(text, sizeof text, "address=%02x%02x%02x%02x%02x%02x\n", m[0], m[1], m[2], m[3], m[4], m[5]);
        if (!length) return -1;
        *data = reinterpret_cast<const uint8_t *>(text); *size = length;
        return 0;
    }
    FwCache *slot = nullptr;
    for (auto &f : gFw) if (f.tried && !strncmp(f.name, base, sizeof f.name)) {slot = &f; break;}
    if (!slot) for (auto &f : gFw) if (!f.tried) {slot = &f; strlcpy(f.name, base, sizeof f.name); f.tried = true; break;}
    if (!slot) return -1;
    if (!slot->data) {
        int error = readVfs(base, &slot->data, &slot->size);
        if (error) {
            char chosen[64] = "";
            const char *candidates[4] = {nullptr, nullptr, nullptr, nullptr};
            static char vendor[48];
            if (!strncmp(base, "bdwlan.", 7)) {  // the vendor's board data by board id (the module's copy of core.c asks for it first)
                if (PE_parse_boot_argn("wifi-bdf", chosen, sizeof chosen) && chosen[0]) candidates[0] = chosen;
                else {snprintf(vendor, sizeof vendor, "image/qca6750/%s", base); candidates[0] = vendor;}
            } else if (!strcmp(base, "board.bin")) {
                candidates[0] = "image/qca6750/bdwlan.elf"; candidates[1] = "image/qca6750/bdwlan.bin"; candidates[2] = "image/bdwlan.elf";
            } else if (!strcmp(base, "regdb.bin")) {
                candidates[0] = "image/qca6750/regdb.bin"; candidates[1] = "image/regdb.bin";
            }
            for (unsigned i = 0; error && candidates[i]; ++i) {
                error = asteroids_modem_file(candidates[i], &slot->data, &slot->size);
                if (!error) logf(6, "wifi: firmware %s from %s, %zu bytes", base, candidates[i], slot->size);
            }
        } else {
            logf(6, "wifi: firmware %s from Data, %zu bytes", base, slot->size);
        }
        if (error) {native_wlan_stats[8] = native_wlan_stats[8] + (1 << 16); return -1;}
    }
    native_wlan_stats[8] = native_wlan_stats[8] + 1;
    *data = slot->data; *size = slot->size;
    return 0;
}

// What the CPU really does with a mapping: the stage 1 translation of va for a read (AT S1E1R), from PAR_EL1: the physical address
// and the memory attribute byte of the mapping's MAIR slot (0x44 Normal non-cacheable, 0xff Normal write-back cacheable, 0x00/0x04/
// 0x0c Device). Memory the chip and the firmware share with the CPU must not be cacheable. Logs "<what>: va .. pa .. attr ..".
void logMapping(const char *what, const volatile void *va) {
    _disable_preemption();
    __asm__ volatile("at s1e1r, %0\n isb" : : "r"(va));
    uint64_t par;
    __asm__ volatile("mrs %0, par_el1" : "=r"(par));
    _enable_preemption();
    if (par & 1) {logf(3, "wifi: mapping %s: va %p does not translate (par %llx)", what, va, par); return;}
    const unsigned attr = (unsigned)(par >> 56);
    logf(attr == 0xff ? 3 : 6, "wifi: mapping %s: pa %llx attr %02x (%s)", what, (par & 0x0000fffffffff000ull) | ((uintptr_t)va & 0xfff), attr,
         attr == 0x44 ? "Normal non-cacheable" : attr == 0xff ? "Normal CACHEABLE" : (attr & 0xf0) == 0 ? "Device" : "other");
}
// The firmware's CX voltage, as soon as its capability reply names it (the module calls this from the QMI path through
// nw_board.cpr_voltage, long before the worker loop runs again) and again from the loop.
unsigned gSupplies = 1;
void applyCpr() {
    // icnss2's qcom,vreg_ol_cpr: smpb3 at the open-loop voltage the firmware reports, before the radio's MAC comes up.
    static uint32_t cprApplied;
    if (gSupplies && nw_wifi_cpr_mv && nw_wifi_cpr_mv != cprApplied && nw_wifi_cpr_mv >= 600 && nw_wifi_cpr_mv <= 1100) {
        cprApplied = nw_wifi_cpr_mv;
        const Vrm cx = {"smpb3", cprApplied};
        voteVrms(&cx, 1);
    }
    // icnss2 does not vote that rail itself: it gives the voltage to the AOP's WLAN power domain controller, whose wake sequence
    // then sets the rail (power.c icnss_update_cpr_info: "{class: wlan_pdc, res: s3b.v, upval: mV}" over the AOSS QMP channel, at
    // least BT_CXMX_VOLTAGE_MV = 950). Without it the PDC's wake voltage is whatever the AOP defaults to. wifi-pdc=0 skips.
    static uint32_t pdcSent;
    static uint64_t pdcAt;
    if (nw_wifi_cpr_mv && nw_wifi_cpr_mv != pdcSent && nw_wifi_cpr_mv >= 600 && nw_wifi_cpr_mv <= 1100) {
        unsigned pdc = 1;
        PE_parse_boot_argn("wifi-pdc", &pdc, sizeof pdc);
        pdcSent = nw_wifi_cpr_mv;
        if (pdc) {
            char message[64];
            snprintf(message, sizeof message, "{class: wlan_pdc, res: s3b.v, upval: %u}", pdcSent > 950 ? pdcSent : 950);
            const int rc = asteroids_wpss_aop(message);
            logf(rc ? 3 : 6, "wifi: AOP %s: queued %d", message, rc);
            pdcAt = pNow() + 1500;
        }
    }
    if (pdcAt && pNow() >= pdcAt) {pdcAt = 0; logf(asteroids_wpss_aop_result() ? 3 : 6, "wifi: AOP wlan_pdc result %d", asteroids_wpss_aop_result());}
}
void pCprVoltage(uint32_t) {applyCpr();}

// ---- board callbacks of the hybrid bus ----
void *pMapPhys(uint64_t pa, size_t n) {
    IODeviceMemory *memory = IODeviceMemory::withRange(pa, n);
    IOMemoryMap *map = memory ? memory->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
    if (memory) memory->release();
    if (!map) {logf(3, "wifi: cannot map %llx + %zx", pa, n); return nullptr;}
    logf(6, "wifi: register window %llx + %zx", pa, n);
    logMapping("register window", reinterpret_cast<void *>(map->getVirtualAddress()));
    return reinterpret_cast<void *>(map->getVirtualAddress());  // kept for the boot
}
int pQrtrSend(const void *packet, size_t len) {
    const int rc = asteroids_wpss_qrtr_send(packet, (uint32_t)len);
    if (!rc) native_wlan_stats[5] = native_wlan_stats[5] + 1;
    return rc ? -EAGAIN : 0;
}
int pRprocUp(unsigned waitMs) {
    for (unsigned waited = 0; !asteroids_wpss_qrtr_up(); waited += 50) {
        if (waited >= waitMs) return 0;
        IOSleep(50);
    }
    return 1;
}
int pSmp2pWlan(uint32_t mask, uint32_t value) {return asteroids_wpss_smp2p_wlan(mask, value);}

// ---- the native control endpoint (org.unixnu.wifi.control): the kernel control socket wpa_supplicant's -Dsheng backend connects to
// (sheng's WifiProbe.cpp). cfg80211's start binds to the transport; one client at a time owns the radio.
struct ControlClient {uint64_t session; uint32_t unit;};
kern_ctl_ref gControlRef;
IOLock *gControlLock;
ControlClient *gControlClient;
volatile uint32_t gControlOpens;
errno_t controlError(int status) {
    switch (status) {
        case NW_CONTROL_OK: return 0;
        case NW_CONTROL_INVALID: return EINVAL;
        case NW_CONTROL_BUSY: return EWOULDBLOCK;
        case NW_CONTROL_NOT_READY: return ENXIO;
        case NW_CONTROL_AUTH_FAILED: return EACCES;
        case NW_CONTROL_TIMED_OUT: return ETIMEDOUT;
        case NW_CONTROL_UNSUPPORTED: return EOPNOTSUPP;
        case NW_CONTROL_NO_MEMORY: return ENOBUFS;
        case NW_CONTROL_NOT_FOUND: return ENOENT;
        case NW_CONTROL_STALE: return ESTALE;
        default: return EIO;
    }
}
errno_t controlConnect(kern_ctl_ref, sockaddr_ctl *address, void **information) {
    auto *owner = static_cast<ControlClient *>(IOMalloc(sizeof(ControlClient)));
    if (!owner) return ENOMEM;
    const int status = nw_wifi_control_attach(&owner->session);
    if (status) {IOFree(owner, sizeof(*owner)); return controlError(status);}
    owner->unit = address->sc_unit;
    IOLockLock(gControlLock); gControlClient = owner; IOLockUnlock(gControlLock);
    gControlOpens = gControlOpens + 1;
    *information = owner;
    return 0;
}
errno_t controlDisconnect(kern_ctl_ref, uint32_t, void *information) {
    if (!information) return 0;  // XNU calls disconnect after a failed connect, with no client context
    auto *owner = static_cast<ControlClient *>(information);
    nw_wifi_control_detach(owner->session);
    IOLockLock(gControlLock); if (gControlClient == owner) gControlClient = nullptr; IOLockUnlock(gControlLock);
    IOFree(owner, sizeof(*owner));
    return 0;
}
int controlReadMbuf(void *source, void *destination, size_t length) {return mbuf_copydata(static_cast<mbuf_t>(source), 0, length, destination);}
errno_t controlReceive(kern_ctl_ref, uint32_t, void *information, mbuf_t packet, int) {
    auto *owner = static_cast<ControlClient *>(information);
    const int status = nw_wifi_control_receive(owner->session, mbuf_pkthdr_len(packet), controlReadMbuf, packet);
    mbuf_freem(packet);
    return controlError(status);
}
void controlReceived(kern_ctl_ref, uint32_t, void *information, int) {nw_wifi_control_writable(static_cast<ControlClient *>(information)->session);}
errno_t controlGetOption(kern_ctl_ref, uint32_t, void *information, int option, void *bytes, size_t *length) {
    if (option != NW_CONTROL_GET_SESSION) return ENOPROTOOPT;
    if (!length) return EINVAL;
    if (!bytes || *length < sizeof(uint64_t)) {*length = sizeof(uint64_t); return EMSGSIZE;}
    const uint64_t session = static_cast<ControlClient *>(information)->session;
    memcpy(bytes, &session, sizeof(session));
    *length = sizeof(session);
    return 0;
}
int controlSend(void *, uint64_t session, const void *bytes, size_t length) {
    if (!gControlLock) return NW_CONTROL_SEND_CLOSED;
    IOLockLock(gControlLock);
    const bool active = gControlClient && gControlClient->session == session;
    const uint32_t unit = active ? gControlClient->unit : 0;
    IOLockUnlock(gControlLock);
    if (!active) return NW_CONTROL_SEND_CLOSED;
    const errno_t result = ctl_enqueuedata(gControlRef, unit, const_cast<void *>(bytes), length, CTL_DATA_EOR);  // no lock held
    if (!result) return NW_CONTROL_SEND_OK;
    if (result == ENOBUFS || result == EWOULDBLOCK) return NW_CONTROL_SEND_AGAIN;
    return NW_CONTROL_SEND_CLOSED;
}
void controlWake(void *) {wakeWorker();}
// Registers the socket (when it can) and installs the transport, which cfg80211's start needs either way.
int controlStart() {
    if (!gControlLock) gControlLock = IOLockAlloc();
    if (!gControlLock) return ENOMEM;
    kern_ctl_reg registration{};
    snprintf(registration.ctl_name, sizeof(registration.ctl_name), "%s", NW_CONTROL_NAME);
    registration.ctl_flags = CTL_FLAG_PRIVILEGED | CTL_FLAG_REG_EXTENDED;
    registration.ctl_sendsize = 256 * 1024; registration.ctl_recvsize = 512 * 1024;
    registration.ctl_connect = controlConnect; registration.ctl_disconnect = controlDisconnect;
    registration.ctl_send = controlReceive; registration.ctl_getopt = controlGetOption; registration.ctl_rcvd = controlReceived;
    const errno_t registered = gControlRef ? 0 : ctl_register(&registration, &gControlRef);
    if (registered) {gControlRef = nullptr; logf(3, "wifi: control socket %s not registered: %d", NW_CONTROL_NAME, registered);}
    static const struct nw_control_transport transport = {nullptr, controlSend, controlWake};
    const int status = nw_wifi_control_transport_set(&transport);
    if (status) logf(3, "wifi: control transport refused: %d", status);
    return registered ? registered : status;
}

// ---- unattended join test (wifi-aj=1): join.txt in the Data add-on (the network name on the first line, its key on the second), staged
// only by a build made with UNIXNU_WIFI_JOIN; joined like the wireless daemon's join, through the supplicant. The key never reaches the log.
extern "C" int asteroids_wifi_join_read(uint8_t **data, size_t *size);
volatile int gConnectDone = -1;
void connectDone(int result) {gConnectDone = result; wakeWorker();}
struct Join {char ssid[33]; char key[65]; size_t ssidLen, keyLen; bool loaded, gaveUp; unsigned rounds; uint64_t next;} gJoin;
bool gNetStarted;
void netStart() {
    struct nw_wifi_status status;
    nw_wifi_status_get(&status);
    if (const OSReturn r = OSKextRegisterBoardClasses(g_kernel_kmod_info.name, native_wifi_net_static_init)) {
        logf(3, "wifi: network classes not registered: %d", (int)r);
        return;
    }
    gNetStarted = true;
    static const uint8_t zero[6] = {};
    uint8_t board[6];
    if (memcmp(status.mac, zero, 6) && !(boardMac(board) && !memcmp(board, status.mac, 6))) {
        publishMac(status.mac, true);
        logf(5, "wifi: en0's address is not the board address; the device tree now carries en0's");
    }
    sheng_wifi_net_start(status.mac);
}
void joinStep() {
    if (gJoin.gaveUp || pNow() < gJoin.next) return;
    gJoin.next = pNow() + 5000;
    if (!gJoin.loaded) {
        uint8_t *file = nullptr; size_t length = 0;
        const int rc = asteroids_wifi_join_read(&file, &length);
        if (rc) {if (++gJoin.rounds > 24) {gJoin.gaveUp = true; logf(4, "wifi: join test: no join.txt (%d)", rc);} return;}
        size_t i = 0, o = 0;
        while (i < length && file[i] != '\n' && o < 32) gJoin.ssid[o++] = (char)file[i++];
        gJoin.ssidLen = o;
        while (i < length && file[i] != '\n') ++i;
        ++i; o = 0;
        while (i < length && file[i] != '\n' && file[i] != '\r' && o < 64) gJoin.key[o++] = (char)file[i++];
        gJoin.keyLen = o;
        memset(file, 0, length);
        IOFreeData(file, length);
        gJoin.loaded = true; gJoin.rounds = 0;
        logf(6, "wifi: join test: a %zu byte network name and a %zu byte key", gJoin.ssidLen, gJoin.keyLen);
    }
    struct nw_wifi_status st; nw_wifi_status_get(&st);
    if (st.state >= NW_STA_AUTH) return;  // joining or joined: the events tell
    if (++gJoin.rounds > 24) {gJoin.gaveUp = true; memset(gJoin.key, 0, sizeof gJoin.key); logf(4, "wifi: join test gave up"); return;}
    bool found = false;
    struct nw_bss_info *bss = static_cast<struct nw_bss_info *>(IOMallocData(sizeof(struct nw_bss_info)));
    for (int i = 0, n = nw_wifi_bss_count(); bss && i < n && !found; ++i)
        found = !nw_wifi_bss_get(i, bss) && bss->ssid_len == gJoin.ssidLen && !memcmp(bss->ssid, gJoin.ssid, gJoin.ssidLen);
    if (bss) IOFreeData(bss, sizeof(struct nw_bss_info));
    if (!found) {logf(6, "wifi: join test round %u: the network is not among %d seen", gJoin.rounds, nw_wifi_bss_count()); nw_wifi_scan(nullptr, 0); return;}
    const int r = nw_wifi_connect(reinterpret_cast<const uint8_t *>(gJoin.ssid), gJoin.ssidLen, nullptr, gJoin.keyLen ? gJoin.key : nullptr, gJoin.keyLen);
    logf(6, "wifi: join test round %u: join requested: %d (supplicant sessions %u)", gJoin.rounds, r, gControlOpens);
}

// ---- the self-test scan: the networks the radio hears go to the log ----
volatile int gScanDone;
void scanDone() {gScanDone = 1; wakeWorker();}
// icnss2's SoC wake handshake (main.c icnss_send_smp2p + fw_soc_wake_ack_handler): the message (4 request, 5 release, sequence << 16) on
// the outbound "wlan_soc_wake" entry, then the firmware acknowledges by flipping bit 0 of its inbound "wlan_soc_wake" entry (the
// qcom,smp2p-soc-wake-ack interrupt, either edge) within SMP2P_SOC_WAKE_TIMEOUT, 500 ms. Returns the milliseconds to the ack, -60
// without one.
int socWake(uint32_t message) {
    const int32_t before = asteroids_wpss_soc_wake_in();
    const uint64_t start = pNow();
    const int32_t sent = asteroids_wpss_smp2p_send(1, message);
    int32_t now = before;
    while (pNow() - start < 500 && (now = asteroids_wpss_soc_wake_in()) == before) IOSleep(1);
    const int ms = now != before ? (int)(pNow() - start) : -60;
    logf(ms >= 0 ? 6 : 3, "wifi: soc wake %s %x: in %x -> %x, %s %d ms", message == 4 ? "req" : "rel", sent, before, now,
         ms >= 0 ? "ack after" : "no ack in", ms >= 0 ? ms : 500);
    return ms;
}

void logNetworks() {
    const int count = nw_wifi_bss_count();
    native_wlan_stats[12] = count;
    native_wlan_bss[0] = 0;
    struct nw_bss_info *bss = static_cast<struct nw_bss_info *>(IOMallocData(sizeof(struct nw_bss_info)));
    if (!bss) return;
    for (int i = 0; i < count; ++i) {
        if (nw_wifi_bss_get(i, bss)) continue;
        char ssid[33];
        unsigned n = 0;
        for (; n < bss->ssid_len && n < 32; ++n) ssid[n] = bss->ssid[n] >= 32 && bss->ssid[n] < 127 ? (char)bss->ssid[n] : '?';
        ssid[n] = 0;
        logf(6, "wifi: network %02x:%02x:%02x:%02x:%02x:%02x %u MHz %d dBm \"%s\"", bss->bssid[0], bss->bssid[1], bss->bssid[2],
             bss->bssid[3], bss->bssid[4], bss->bssid[5], bss->freq_mhz, bss->signal_dbm, ssid);
        char entry[48];
        snprintf(entry, sizeof entry, "%s%s/%d", native_wlan_bss[0] ? " " : "", n ? ssid : "(hidden)", bss->signal_dbm);
        strlcat(native_wlan_bss, entry, sizeof native_wlan_bss);
    }
    IOFreeData(bss, sizeof(struct nw_bss_info));
}

void wifiThread(void *, wait_result_t) {
    thread_set_thread_name(current_thread(), "wifi");
    stage(StThread);
    if (uint8_t m[6]; boardMac(m)) publishMac(m, false);
    // The WPSS comes up on the ADSP thread once the firmware partition is readable: wait for its "ready" (up to three minutes).
    for (unsigned waited = 0; native_wpss_stats[0] < 7; waited += 250) {
        if (native_wpss_stats[0] < 0 || waited >= 180000) {fail(StWpss, native_wpss_stats[0]); return;}
        IOSleep(250);
    }
    gRxLock = IOLockAlloc();
    gRx = static_cast<RxPacket *>(IOMallocData(sizeof(RxPacket) * kRxQueue));
    if (!gRxLock || !gRx) {fail(StWpss, -12); return;}
    const int attached = asteroids_wpss_qrtr_attach(linkRx, linkState, nullptr);
    if (attached) {fail(StWpss, attached); return;}
    stage(StWpss);
    uint32_t dmaMiB = 48;
    PE_parse_boot_argn("wifi-dma", &dmaMiB, sizeof dmaMiB);
    if (dmaMiB < 8 || dmaMiB > 256) dmaMiB = 48;
    if (!arenaInit((size_t)dmaMiB << 20)) {fail(StDma, -12); return;}
    logf(6, "wifi: DMA window %llx + %u MiB, write-combined (%s)", gArena.pa, dmaMiB, gArena.memory ? "XNU memory" : "below 4 GiB");
    logMapping("DMA window start", gArena.va);
    logMapping("DMA window end", gArena.va + gArena.bytes - 4096);
    if (asteroids_remote::smem().base) logMapping("SMEM (SMP2P, GLINK)", asteroids_remote::smem().base);
    if (!setupSmmu()) {fail(StSmmu, native_wlan_stats[9]); return;}
    stage(StSmmu);
    stage(StDma);
    PE_parse_boot_argn("wifi-power", &gSupplies, sizeof gSupplies);
    if (gSupplies) {
        const int rails = voteVrms(kVrms, sizeof kVrms / sizeof kVrms[0]), pa = enablePaSupply();
        native_wlan_stats[1] = rails | (pa ? 0x100 : 0);
    }
    stage(StIrqs);

    static const struct lx_platform plat = {pNow, pSleep, pPoll, pLog, pHalt, hAlloc, hFree, pDmaAlloc, pDmaFree, pDmaMap, pDmaSync, pFwGet,
                                            pNowNs, pBootNs, pBusyDelayUs, _disable_preemption, _enable_preemption,
                                            nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};   // one executor
    nw_wifi_set_platform(&plat);
    static struct nw_board board;
    board.irq_base = kIrqBase;
    board.map_phys = pMapPhys;
    board.msi_addr = kMsiAddr;
    board.msi_spi_first = kMsiSpi;
    board.msi_count = kMsiCount;
    board.qrtr_send = pQrtrSend;
    board.rproc_up = pRprocUp;
    board.smp2p_wlan = pSmp2pWlan;
    board.cpr_voltage = pCprVoltage;
    unsigned wmiMem = 1;  // wifi-wmi-mem=0: only log the firmware's host memory requests
    PE_parse_boot_argn("wifi-wmi-mem", &wmiMem, sizeof wmiMem);
    nw_wifi_wmi_mem = wmiMem;
    unsigned idlePs = 1;  // wifi-idle-ps=0: the firmware never enters IMPS between scans (WMI_PDEV_PARAM_IDLE_PS_CONFIG 0)
    PE_parse_boot_argn("wifi-idle-ps", &idlePs, sizeof idlePs);
    nw_wifi_idle_ps = idlePs;
    PE_parse_boot_argn("wifi-debug", &board.debug_mask, sizeof board.debug_mask);  // ath11k's ATH11K_DBG_* bits
    static const struct nw_wifi_events events = {scanDone, connectDone, nullptr, nullptr, nullptr, nullptr, nullptr};
    nw_wifi_set_events(&events);
    controlStart();
    const int r = nw_wifi_start(&plat, &board);
    native_wlan_stats[14] = r;
    if (r) {fail(StStarted, r); return;}
    stage(StStarted);
    // The vectors the module asked for (nw_wifi_irq_vectors) are attached as GIC SPIs (wifi88: 16 of 32; the worker's rounds fell from
    // ~1000/s to 6-150/s); wifi-irq=0 leaves them unattached and the module polls its interrupt lines every millisecond.
    // wifi-irq-poll=MS: with interrupts, still give every handler a turn every MS ms (default 20; 0 never).
    unsigned wantIrqs = 1, irqPoll = 20;
    PE_parse_boot_argn("wifi-irq", &wantIrqs, sizeof wantIrqs);
    PE_parse_boot_argn("wifi-irq-poll", &irqPoll, sizeof irqPoll);
    const uint32_t irqVectors = nw_wifi_irq_vectors();
    const int irqs = wantIrqs ? attachIrqs(irqVectors) : -1;
    if (irqs) logf(6, "wifi: interrupts %s (%d, vectors %x, attached %x): polling", wantIrqs ? "not attached" : "not asked for", irqs, irqVectors, gIrqMask);
    else logf(6, "wifi: interrupts on: vectors %x, backstop poll %u ms", gIrqMask, irqPoll);
    nw_wifi_irq_backstop(irqPoll);
    nw_wifi_irq_mode(gIrqs);

    unsigned selfScan = 1, autoJoin = 0;
    PE_parse_boot_argn("wifi-scan", &selfScan, sizeof selfScan);
    PE_parse_boot_argn("wifi-aj", &autoJoin, sizeof autoJoin);
    bool powered = false, scanAsked = false;
    uint64_t scanAt = 0;
    unsigned fwdLeft = 0;
    uint64_t fwdAt = 0;
    uint64_t nextFaults = 0, nextLoad = pNow() + 30000, loadRounds = 0;
    int32_t loadIrqs = native_wlan_stats[2];
    for (uint64_t n = 0;; ++n) {
        if (pNow() >= nextFaults) {nextFaults = pNow() + 500; faultReport();}
        if (pNow() >= nextLoad) {   // worker rounds and interrupts per second over the last 30 s (polling: about 1000 rounds)
            const int32_t irqsNow = native_wlan_stats[2];
            logf(6, "wifi: load %llu rounds/s, %d interrupts/s (vectors seen %x)", (n - loadRounds) / 30, (irqsNow - loadIrqs) / 30,
                 (uint32_t)native_wlan_stats[3]);
            loadRounds = n; loadIrqs = irqsNow; nextLoad = pNow() + 30000;
        }
        applyCpr();
        const int busy = nw_wifi_step();
        if (!powered && nw_wifi_stats.stage >= 4) {
            // As icnss2 around every ring pointer write of the vendor host (hal_srng delayed register writes: prevent L1, i.e. leave the
            // WPSS-chip link's power save, and a SoC wake vote): asked once and held (wifi-awake=0 skips). Without them the firmware
            // asserted in whalRecvDmaLeakRecovery/Detect at the first scan. Giving both back after each access, as icnss2 does, kept the
            // MAC from leaving IMPS (whal_wmac_transition_to_fw_control:1734).
            unsigned awake = 1;
            PE_parse_boot_argn("wifi-awake", &awake, sizeof awake);
            if (awake) {
                const int32_t ep = asteroids_wpss_smp2p_send(2, 7);
                logf(6, "wifi: ep power save exit %x", ep);
                socWake(4);
            }
            powered = true;
            stage(StInterface);
            nw_wifi_power(1);
            // wifi-net=0: no network side (the board's own join test, wifi-aj, then runs as before).
            unsigned net = 1;
            PE_parse_boot_argn("wifi-net", &net, sizeof net);
            if (net) netStart();
            scanAt = pNow() + 2000;   // the self-test scan 2 s after radio on
        }
        // The moment the WPSS reports a fatal error, in this log's time (the [wpss] rows are sampled every 2 s).
        static bool wpssDead;
        if (!wpssDead && native_wpss_stats[3] > 0 && (native_wpss_stats[3] & 1)) {
            wpssDead = true;
            logf(3, "wifi: WPSS fatal (slave %x)", native_wpss_stats[3]);
            fwdLeft = 240; fwdAt = pNow();
        }
        // The firmware's last diag records after it died: 80 a round, oldest first, a round every 2.5 s (the log takes 100 lines a slot).
        if (fwdLeft && pNow() >= fwdAt) {
            fwdLeft -= 80;
            nw_fw_diag_dump(fwdLeft, 80);
            fwdAt = pNow() + 2500;
        }
        if (powered && selfScan && !scanAsked && pNow() >= scanAt) {
            scanAsked = true;
            // The mainline scan request only notifies the control client (wpa_supplicant does the scanning); without one the board starts it.
            const int s = nw_wifi_self_scan();
            native_wlan_stats[11] = native_wlan_stats[11] + 1;
            if (s) logf(3, "wifi: scan refused: %d", s);
        }
        if (gScanDone) {
            gScanDone = 0; logNetworks(); stage(StScanned);
        }
        if (gConnectDone >= 0) {
            struct nw_wifi_status st; nw_wifi_status_get(&st);
            logf(6, "wifi: join result %d, state %u, %u MHz, %d dBm", gConnectDone, st.state, st.freq_mhz, st.signal_dbm);
            native_wlan_stats[10] = gConnectDone;
            gConnectDone = -1;
        }
        if (autoJoin && powered && scanAsked && !gNetStarted) joinStep();   // with the network side, WifiNet joins (wifi-aj)
        if (!busy) {
            const unsigned wait = nw_wifi_wait_ms(gIrqs ? 1000 : 1);
            if (wait) waitWork(wait);
        }
        if ((n & 1023) == 0) native_wlan_stats[13] = (int32_t)(n >> 10);
    }
}
}  // namespace

// ---- WifiNet's hooks ----
extern "C" void native_wifi_logf(int level, const char *fmt, ...) {
    char line[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    pLog(level, line);
}
extern "C" void sheng_wifi_wake(void) {wakeWorker();}   // a frame to send: the worker's next step takes it
// join.txt (the unattended test's network, UNIXNU_WIFI_JOIN): from the Data add-on, which is installed a while after boot (EAGAIN until
// then: waited for up to a minute on WifiNet's auto-join thread). Kept in one static copy, the read buffer cleared and freed.
extern "C" int sheng_wifi_file(const char *name, const uint8_t **data, size_t *size) {
    if (strcmp(name, "join.txt")) return pFwGet(name, data, size);
    static uint8_t copy[160];
    static size_t copied;
    if (!copied) {
        uint8_t *file = nullptr;
        size_t length = 0;
        int rc = EAGAIN;
        for (unsigned waited = 0; (rc = asteroids_wifi_join_read(&file, &length)) == EAGAIN && waited < 60000; waited += 500) IOSleep(500);
        if (rc || !file) return rc ? rc : ENOENT;
        copied = length < sizeof copy ? length : sizeof copy;
        memcpy(copy, file, copied);
        bzero(file, length);
        IOFreeData(file, length);
    }
    *data = copy; *size = copied;
    return 0;
}

// The firmware files an owner may install on the Data volume (VFS; AsteroidsWifiFiles.c, built with the BSD flags).
extern "C" int asteroids_wifi_vfs_read(const char *name, uint8_t **data, size_t *size);
namespace {
int readVfs(const char *name, uint8_t **data, size_t *size) {return asteroids_wifi_vfs_read(name, data, size);}
}

// The WPSS start (WpssProbe.cpp asteroids_wpss_run) sets the WCN's sleep clock first.
extern "C" int asteroids_wifi_sleep_clock(void) {return sleepClock();}

extern "C" void asteroids_wifi_probe(void) {
    unsigned mode = 0;
    if (!PE_parse_boot_argn("wifi", &mode, sizeof mode) || mode < 5 || native_wlan_stats[0]) return;
    native_wlan_lines = static_cast<char *>(IOMallocData(kLines * kLineBytes));
    if (native_wlan_lines) bzero(native_wlan_lines, kLines * kLineBytes);
    thread_t thread = nullptr;
    if (kernel_thread_start(wifiThread, nullptr, &thread) == KERN_SUCCESS) thread_deallocate(thread);
    else fail(StThread, -7);
}
