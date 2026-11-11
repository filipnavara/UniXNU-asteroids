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
// wifi=5 runs it (with the WPSS of wifi>=2). wifi-power=0 leaves the chip's RF supplies as they are. wifi-irq=1 attaches the 32 interrupt lines (otherwise the module polls them). wifi-scan=0 skips the self-test scan the board otherwise asks for once the interface is up
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
#include "nw_abi.h"
#include "nw_control_endpoint.h"
#include "SmmuBypass.h"
#include "BootResetCore.h"
extern "C" {
#include "QcomRpmh.h"
}

extern "C" int armcpu_spi_attach(uint32_t, bool, void (*)(void *), void *, uint32_t *, uint64_t *) __attribute__((weak_import));
extern "C" int asteroids_wpss_qrtr_attach(void (*rx)(void *, const uint8_t *, uint32_t), void (*state)(void *, int), void *context);
extern "C" int asteroids_wpss_qrtr_up(void);
extern "C" int asteroids_wpss_qrtr_send(const void *packet, uint32_t length);
extern "C" int asteroids_wpss_smp2p_wlan(uint32_t mask, uint32_t value);
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
    if (!native_wlan_lines) return;
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
struct Arena {uint8_t *va; uint64_t pa; size_t bytes; struct Block {uint32_t off, size; bool used;} *blocks; unsigned count; size_t used, peak; unsigned failures;
              IOBufferMemoryDescriptor *memory; IOMemoryMap *map;} gArena;
bool arenaInit(size_t bytes) {
    gArena.blocks = static_cast<Arena::Block *>(IOMallocData(kArenaBlocks * sizeof(Arena::Block)));
    if (!gArena.blocks) return false;
    gArena.memory = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, (IOOptionBits)kIODirectionInOut | (IOOptionBits)kIOMemoryPhysicallyContiguous, bytes,
                                                                     0x0000000ffffff000ull);
    if (!gArena.memory || gArena.memory->prepare() != kIOReturnSuccess) return false;
    gArena.pa = gArena.memory->getPhysicalSegment(0, nullptr, kIOMemoryMapperNone);
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
        if (bus) *bus = gArena.pa + start;
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
uint32_t smmuRd(void *ctx, unsigned o) {return *reinterpret_cast<volatile uint32_t *>(reinterpret_cast<uintptr_t>(ctx) + o);}
void smmuWr(void *ctx, unsigned o, uint32_t v) {
    *reinterpret_cast<volatile uint32_t *>(reinterpret_cast<uintptr_t>(ctx) + o) = v;
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
// PMR735B (SPMI SID 5) GPIO1 high: the enable of S1J. PMIC5 GPIO registers: 0x40 mode (1 output), 0x44 output source (bit 7 the level,
// function 0 normal), 0x46 enable (bit 7).
int enablePaSupply() {
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
            if (!map || ((map >> 8) & 0xfff) != 0x588) continue;
            const int o2 = (int)(arb.cnfg[(BR_CNFG_OWNER + 4 * i) / 4] & 7u);
            if (apid < 0 || o2 == arb.ee) {apid = (int)i; owner = o2;}
        }
        if (o && c && apid >= 0) {
            uint8_t before[4] = {}, after[4] = {};
            const uint8_t regs[4] = {0x40, 0x44, 0x46, 0x08};
            for (unsigned i = 0; i < 4; ++i) br_read8(o, &arb, apid, regs[i], &before[i]);
            rc = br_write8(c, apid, 0x40, 0x01);
            if (!rc) rc = br_write8(c, apid, 0x44, 0x80);
            if (!rc) rc = br_write8(c, apid, 0x46, 0x80);
            IOSleep(2);  // startup-delay-us 1000
            for (unsigned i = 0; i < 4; ++i) br_read8(o, &arb, apid, regs[i], &after[i]);
            logf(6, "wifi: S1J enable (pmr735b gpio1, apid %d owner %d): mode/out/en/sts %02x %02x %02x %02x -> %02x %02x %02x %02x, %d", apid,
                 owner, before[0], before[1], before[2], before[3], after[0], after[1], after[2], after[3], rc);
        } else {
            logf(3, "wifi: S1J enable: no arbiter channel for pmr735b gpio1 (apid %d)", apid);
        }
    }
    IOMemoryMap *const maps[4] = {core, cnfg, obs, chn};
    for (IOMemoryMap *m : maps) if (m) m->release();
    return rc;
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
int attachIrqs() {
    if (!armcpu_spi_attach) return -78;
    for (uint32_t v = 0; v < kMsiCount; ++v) {
        int rc = 0;
        for (unsigned tries = 0;; ++tries) {
            rc = armcpu_spi_attach(32 + kMsiSpi + v, true, msiIrq, reinterpret_cast<void *>(uintptr_t(v)), nullptr, nullptr);
            if (rc != kIOReturnNotReady || tries == 40) break;
            IOSleep(250);
        }
        if (rc) return rc;
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
    return gArena.pa + ((uint8_t *)p - gArena.va);
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

// ---- board callbacks of the hybrid bus ----
void *pMapPhys(uint64_t pa, size_t n) {
    IODeviceMemory *memory = IODeviceMemory::withRange(pa, n);
    IOMemoryMap *map = memory ? memory->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
    if (memory) memory->release();
    if (!map) {logf(3, "wifi: cannot map %llx + %zx", pa, n); return nullptr;}
    logf(6, "wifi: register window %llx + %zx", pa, n);
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

// ---- the native control endpoint (org.unixnu.wifi.control): cfg80211's start binds to it. No socket yet (no wpa_supplicant on this
// board), so no session ever opens; sheng's WifiProbe.cpp registers the kernel control socket here.
int controlSend(void *, uint64_t, const void *, size_t) {return NW_CONTROL_SEND_CLOSED;}
void controlWake(void *) {wakeWorker();}

// ---- the self-test scan: the networks the radio hears go to the log ----
volatile int gScanDone;
void scanDone() {gScanDone = 1; wakeWorker();}
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
    logf(6, "wifi: DMA window %llx + %u MiB, write-combined", gArena.pa, dmaMiB);
    if (!setupSmmu()) {fail(StSmmu, native_wlan_stats[9]); return;}
    stage(StSmmu);
    stage(StDma);
    unsigned supplies = 1;
    PE_parse_boot_argn("wifi-power", &supplies, sizeof supplies);
    if (supplies) {
        const int rails = voteVrms(kVrms, sizeof kVrms / sizeof kVrms[0]), pa = enablePaSupply();
        native_wlan_stats[1] = rails | (pa ? 0x100 : 0);
    }
    // ArmCpu keeps 24 SPI handlers for every driver, fewer than the chip's 32 vectors: the module polls its interrupt lines every
    // millisecond unless wifi-irq=1 attaches them all (and falls back to polling when they do not fit).
    unsigned wantIrqs = 0;
    PE_parse_boot_argn("wifi-irq", &wantIrqs, sizeof wantIrqs);
    const int irqs = wantIrqs ? attachIrqs() : -1;
    if (irqs) logf(6, "wifi: interrupts %s (%d): polling", wantIrqs ? "not attached" : "not asked for", irqs);
    stage(StIrqs);

    static const struct lx_platform plat = {pNow, pSleep, pPoll, pLog, pHalt, hAlloc, hFree, pDmaAlloc, pDmaFree, pDmaMap, pDmaSync, pFwGet,
                                            pNowNs, pBootNs, pBusyDelayUs, _disable_preemption, _enable_preemption};
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
    static const struct nw_wifi_events events = {scanDone, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
    nw_wifi_set_events(&events);
    static const struct nw_control_transport transport = {nullptr, controlSend, controlWake};
    const int control = nw_wifi_control_transport_set(&transport);
    if (control) logf(3, "wifi: control transport refused: %d", control);
    const int r = nw_wifi_start(&plat, &board);
    native_wlan_stats[14] = r;
    if (r) {fail(StStarted, r); return;}
    stage(StStarted);
    nw_wifi_irq_mode(gIrqs);

    unsigned selfScan = 1;
    PE_parse_boot_argn("wifi-scan", &selfScan, sizeof selfScan);
    bool powered = false, scanAsked = false;
    uint64_t scanAt = 0;
    uint64_t nextFaults = 0;
    for (uint64_t n = 0;; ++n) {
        if (pNow() >= nextFaults) {nextFaults = pNow() + 500; faultReport();}
        // icnss2's qcom,vreg_ol_cpr: smpb3 at the open-loop voltage the firmware reports, before the radio's MAC comes up.
        static uint32_t cprApplied;
        if (supplies && nw_wifi_cpr_mv && nw_wifi_cpr_mv != cprApplied && nw_wifi_cpr_mv >= 600 && nw_wifi_cpr_mv <= 1100) {
            cprApplied = nw_wifi_cpr_mv;
            const Vrm cx = {"smpb3", cprApplied};
            voteVrms(&cx, 1);
        }
        const int busy = nw_wifi_step();
        if (!powered && nw_wifi_stats.stage >= 4) {
            powered = true;
            stage(StInterface);
            nw_wifi_power(1);
            scanAt = pNow() + 2000;
        }
        if (powered && selfScan && !scanAsked && pNow() >= scanAt) {
            scanAsked = true;
            const int s = nw_wifi_scan(nullptr, 0);
            native_wlan_stats[11] = native_wlan_stats[11] + 1;
            if (s) logf(3, "wifi: scan refused: %d", s);
        }
        if (gScanDone) {gScanDone = 0; logNetworks(); stage(StScanned);}
        if (!busy) {
            const unsigned wait = nw_wifi_wait_ms(gIrqs ? 1000 : 1);
            if (wait) waitWork(wait);
        }
        if ((n & 1023) == 0) native_wlan_stats[13] = (int32_t)(n >> 10);
    }
}
}  // namespace

// The firmware files an owner may install on the Data volume (VFS; AsteroidsWifiFiles.c, built with the BSD flags).
extern "C" int asteroids_wifi_vfs_read(const char *name, uint8_t **data, size_t *size);
namespace {
int readVfs(const char *name, uint8_t **data, size_t *size) {return asteroids_wifi_vfs_read(name, data, size);}
}

extern "C" void asteroids_wifi_probe(void) {
    unsigned mode = 0;
    if (!PE_parse_boot_argn("wifi", &mode, sizeof mode) || mode < 5 || native_wlan_stats[0]) return;
    native_wlan_lines = static_cast<char *>(IOMallocData(kLines * kLineBytes));
    if (native_wlan_lines) bzero(native_wlan_lines, kLines * kLineBytes);
    thread_t thread = nullptr;
    if (kernel_thread_start(wifiThread, nullptr, &thread) == KERN_SUCCESS) thread_deallocate(thread);
    else fail(StThread, -7);
}
