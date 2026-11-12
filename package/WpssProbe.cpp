// SPDX-License-Identifier: BSD-2-Clause-Patent
// asteroids: the WiFi processor (WPSS) of the Nothing Phone (3a) (SM7635). The WCN6750's WLAN firmware runs on the WPSS (a q6v5 behind
// PAS); the host starts it, then reaches the WLAN firmware's QMI services (WLFW) through the IPC router on the WPSS's GLINK edge. Linux does
// the same in qcom_q6v5_pas (sc7280 wpss) and ath11k_ahb (WCN6750). This file starts the WPSS on the ADSP thread with that thread's
// machinery (AsteroidsRemote.h): the IPCC line, the AOSS mailbox, the RPMh TCS and the SMEM mutex are shared, so one thread owns them.
//
// Devicetree facts (stock 3a devicetree, remoteproc-wpss@9bb00000 "qcom,volcano-wpss-pas" and qcom,smp2p-wpss): carveout wpss_mem
// 0x9bb00000 + 0x1900000; firmware "qca6750/wpss.mdt"; proxy supplies cx.lvl and mx.lvl at level 384 (cx-uV-uA <0x180 ...>); AOSS
// load_state "wpss" (qcom,signal-aop); SMP2P items 617 (in) and 616 (out), remote pid 13, IPCC client 24 signal 2; entries
// master-kernel (bit 0 stop) out, slave-kernel in (fatal 0, ready 1, handover 2, stop ack 3); the WLAN entries "wlan", "wlan_soc_wake",
// "wlan_ep_power_save" out and "wlan", "wlan_soc_wake" in (icnss2's, ath11k uses "wlan" for wake from WoW); GLINK edge "wpss" with IPCC
// client 24 signal 0 and channel IPCRTR (intents 0x800 x5, 0x2000 x3, 0x4400 x2). PAS id 6 and crash reason SMEM item 626 are Linux's
// sc7280 values (qcom_q6v5_pas.c), the closest upstream WPSS.
//
// wifi=1 surveys (SMEM partition and items, the WPSS's SMP2P state) and writes nothing; wifi=2 starts the WPSS and runs the IPC router
// on its edge so the services it announces show in the log. With wifi=5 the router link is handed to the WiFi module (asteroids_wpss_qrtr_*)
// instead. wifi-fw=PATH names the image (as sheng_fw_open takes it); otherwise the firmware partition's qca6750/wpss and wpss are tried.
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <kern/thread.h>
#include <sys/errno.h>
#include <pexpert/pexpert.h>
extern "C" {
#include "QcomGlink.h"
#include "QcomQrtr.h"
#include "QcomScm.h"
#include "QcomSmem.h"
#include "QcomSmp2p.h"
}
#include "AsteroidsRemote.h"

// [0] stage (see Stage; negative: the step that failed), [1] last error, [2] doorbells serviced, [3] the WPSS's slave-kernel bits (-1 no
// item), [4] items (bit 0 SMP2P in 617, 1 out 616, 2 GLINK 478, 3 479, 4 480, 8 the partition with pid 13), [5] wpss image KiB, [6] ms
// from auth_and_reset to "ready", [7] GLINK messages in, [8] out, [9] IPCRTR up | version agreed << 14 | link broken << 15, [10] last
// SCM x0, [11] QRTR packets in, [12] the WPSS's node (-1 before its hello), [13] services announced, [14] WLFW (69) instance (-1 none),
// [15] the WPSS's inbound "wlan" bits | "wlan_soc_wake" bits << 8 | firmware source index << 16 | module packets queued << 20
extern "C" {volatile int32_t native_wpss_stats[16]; char native_wpss_text[96]; char native_wpss_services[160]; char native_wpss_files[256];
            volatile uint32_t native_wpss_wlen[4]; char native_wpss_crash[224];}
extern "C" int asteroids_modem_list(const char *path, char *out, size_t cap);

namespace {
namespace R = asteroids_remote;
enum Stage {StPartition = 1, StSurveyed = 2, StSmp2p = 3, StLoadState = 4, StRails = 5, StStarted = 6, StReady = 7, StGlink = 8,
            StChannel = 9, StHello = 10, StWlfw = 11};
constexpr uint64_t kWpssBase = 0x9bb00000, kWpssSize = 0x1900000;
constexpr uint32_t kPasWpss = 6, kRemotePid = 13, kItemIn = 617, kItemOut = 616, kItemCrash = 626;
constexpr uint32_t kItemGlinkDesc = 478, kItemGlinkTx = 479, kItemGlinkRx = 480, kGlinkFifo = 0x4000;
constexpr uint32_t kSignalGlink = 0, kSignalSmp2p = 2;
constexpr uint32_t kFatal = 1u << 0, kReady = 1u << 1, kHandover = 1u << 2;
constexpr uint32_t kLevelProxy = 384;  // RPMH_REGULATOR_LEVEL_TURBO, the devicetree's cx-uV-uA and mx-uV-uA
constexpr uint32_t kWlfwService = 69;
constexpr uint32_t kIntentSize = 0x4400, kIntents = 6;
constexpr unsigned kQueue = 16, kPacket = 0x4400;

void fail(int step, int error) {native_wpss_stats[1] = error; native_wpss_stats[0] = -step;}
void stage(int value) {native_wpss_stats[0] = value;}
void note(const char *prefix, const char *text) {
    size_t at = 0;
    for (; prefix[at] && at < sizeof native_wpss_text - 1; ++at) native_wpss_text[at] = prefix[at];
    for (size_t i = 0; text[i] && at < sizeof native_wpss_text - 1; ++i) native_wpss_text[at++] = text[i];
    native_wpss_text[at] = 0;
}

unsigned gMode;
QcomSmemPartition gPart;
bool gHavePart;
QcomSmp2p gP2p;
int gStop = -1, gWlanOut = -1, gWakeOut = -1, gPowerSaveOut = -1;
uint32_t gSlave;
bool gHandedOver, gRunning;
QcomGlink gGlink;
bool gGlinkStarted;
QcomGlinkChannel *gChannel;
uint8_t *gIntents;
QcomQrtr gQrtr;

// Packets for the WPSS: QRTR control answers from this side and, with wifi=5, the module's packets. Filled from any thread under gLock,
// drained on the ADSP thread.
struct Packet {uint32_t length; uint8_t data[kPacket];};
Packet *gQueue;
unsigned gHead, gCount;
IOLock *gLock;
void (*gModuleRx)(void *, const uint8_t *, uint32_t);
// With wifi=5 the router link belongs to the module: what the WPSS sends before the module attaches waits here (heap copies) and is
// handed over, in order, at the attach.
constexpr unsigned kBacklog = 32;
struct Held {uint8_t *data; uint32_t length;} *gBacklog;
unsigned gHeld;
bool gLinkWasUp, gReplayed;
void (*gModuleState)(void *, int);
void *gModuleContext;

volatile uint8_t *item(uint16_t id, uint32_t *size) {return gHavePart ? R::item(gPart, id, size) : nullptr;}
void kickSmp2p() {R::ring(R::kIpccWpss, kSignalSmp2p);}
void kickGlink(void *) {R::ring(R::kIpccWpss, kSignalGlink);}

void readCrashReason() {
    const QcomSmem &smem = R::smem();
    QcomSmemPartition global;
    uint32_t at = 0, size = 0;
    const bool shared = gHavePart && !qcom_smem_find(&smem, &gPart, kItemCrash, &at, &size) && size;
    if (!shared && (qcom_smem_partition(&smem, QCOM_SMEM_HOST_GLOBAL, QCOM_SMEM_HOST_GLOBAL, &global) ||
                    qcom_smem_find(&smem, &global, kItemCrash, &at, &size) || !size))
        return;
    char text[80];
    size_t n = 0;
    for (; n < sizeof text - 1 && n < size; ++n) {const char c = (char)smem.base[at + n]; if (!c) break; text[n] = c >= 32 && c < 127 ? c : '.';}
    text[n] = 0;
    if (n) note("crash: ", text);
    // the whole reason (the note keeps 80 characters of it)
    size_t m = 0;
    for (; m < sizeof native_wpss_crash - 1 && m < size; ++m) {
        const char c = (char)smem.base[at + m];
        if (!c) break;
        native_wpss_crash[m] = c >= 32 && c < 127 ? c : '.';
    }
    native_wpss_crash[m] = 0;
}

// ---- the survey: reads only ----
void survey() {
    int32_t bits = 0;
    gHavePart = !qcom_smem_partition(&R::smem(), QCOM_SMEM_HOST_APPS, kRemotePid, &gPart);
    if (gHavePart) {
        bits |= 1 << 8;
        const uint16_t ids[5] = {kItemIn, kItemOut, kItemGlinkDesc, kItemGlinkTx, kItemGlinkRx};
        for (unsigned i = 0; i < 5; ++i) if (item(ids[i], nullptr)) bits |= 1 << i;
    }
    native_wpss_stats[4] = bits;
    native_wpss_stats[3] = -1;
    native_wpss_stats[12] = -1;
    native_wpss_stats[14] = -1;
    if (volatile uint8_t *in = item(kItemIn, nullptr)) {
        QcomSmp2p view = {};
        view.in = reinterpret_cast<volatile uint32_t *>(in);
        view.local_pid = 0; view.remote_pid = kRemotePid;
        const int index = qcom_smp2p_check_in(view.in, 0, kRemotePid) ? -1 : qcom_smp2p_find_in(&view, "slave-kernel");
        if (index >= 0) native_wpss_stats[3] = (int32_t)qcom_smp2p_in_value(&view, index);
    }
    readCrashReason();
}

// ---- the QRTR side: the board's router (wifi=2) or the module (wifi=5) ----
bool queuePacket(const uint8_t *data, uint32_t length) {
    if (!gQueue || !length || length > kPacket) return false;
    IOLockLock(gLock);
    const bool room = gCount < kQueue;
    if (room) {
        Packet &p = gQueue[(gHead + gCount) % kQueue];
        memcpy(p.data, data, length);
        p.length = length;
        gCount++;
    }
    IOLockUnlock(gLock);
    return room;
}
int routerSend(void *, const uint8_t *packet, uint32_t length) {return queuePacket(packet, length) ? 0 : -1;}

void appendService(uint32_t service, uint32_t instance) {
    char entry[24];
    snprintf(entry, sizeof entry, "%s%x:%x", native_wpss_services[0] ? " " : "", service, instance);
    strlcat(native_wpss_services, entry, sizeof native_wpss_services);
}
void routerCounters() {
    native_wpss_stats[11] = (int32_t)gQrtr.rx_packets;
    native_wpss_stats[12] = gQrtr.hello_received ? (int32_t)gQrtr.remote_node : -1;
    native_wpss_stats[13] = (int32_t)gQrtr.remote_count;
    native_wpss_services[0] = 0;
    for (uint32_t i = 0; i < gQrtr.remote_count; ++i) {
        appendService(gQrtr.remote[i].service, gQrtr.remote[i].instance);
        if (gQrtr.remote[i].service == kWlfwService) native_wpss_stats[14] = (int32_t)gQrtr.remote[i].instance;
    }
    if (gQrtr.hello_received && native_wpss_stats[0] >= StChannel && native_wpss_stats[0] < StHello) stage(StHello);
    if (native_wpss_stats[14] >= 0 && native_wpss_stats[0] >= StChannel) stage(StWlfw);
}

// QRTR v1 header words: version, type, src node, src port, confirm, size, dst node, dst port. With the module attached, control packets
// still update the service list here (the log keeps showing what the WPSS announced).
void channelState(void *, QcomGlinkChannel *, int up) {
    gLinkWasUp = up != 0;
    if (up) {
        if (native_wpss_stats[0] >= StGlink && native_wpss_stats[0] < StChannel) stage(StChannel);
        if (gModuleRx) {if (gModuleState) gModuleState(gModuleContext, 1); return;}
        if (gMode >= 5) return;  // the module says hello when it attaches
        gQrtr.hello_sent = gQrtr.hello_received = 0;
        gQrtr.remote_count = 0;
        gQrtr.remote_node = QCOM_QRTR_NODE_BROADCAST;
        qcom_qrtr_start(&gQrtr);
    } else if (gModuleRx && gModuleState) {
        gModuleState(gModuleContext, 0);
    }
}
void channelReceive(void *, QcomGlinkChannel *, const uint8_t *data, uint32_t length) {
    if (!gModuleRx && gMode >= 5) {
        uint8_t *copy = gBacklog && gHeld < kBacklog && length ? static_cast<uint8_t *>(IOMallocData(length)) : nullptr;
        if (copy) {memcpy(copy, data, length); gBacklog[gHeld++] = {copy, length};}
        native_wpss_stats[11] = native_wpss_stats[11] + 1;
        return;
    }
    if (gModuleRx) {
        gModuleRx(gModuleContext, data, length);
        // Keep the log's list of announced services from the control packets that pass by.
        if (length >= 32 + 20 && data[4] == QCOM_QRTR_NEW_SERVER) {
            uint32_t service, instance;
            memcpy(&service, data + 32 + 4, 4);
            memcpy(&instance, data + 32 + 8, 4);
            appendService(service, instance);
            if (service == kWlfwService) native_wpss_stats[14] = (int32_t)instance;
        }
        native_wpss_stats[11] = native_wpss_stats[11] + 1;
        return;
    }
    qcom_qrtr_receive(&gQrtr, data, length);
    routerCounters();
}

void pumpSends() {
    if (!gChannel || !gChannel->up) return;
    for (;;) {
        IOLockLock(gLock);
        if (!gCount) {IOLockUnlock(gLock); break;}
        Packet *p = &gQueue[gHead];
        IOLockUnlock(gLock);
        // Only this thread takes packets off the queue, so p stays valid while the lock is dropped for the send.
        if (qcom_glink_send(&gGlink, gChannel, p->data, p->length) != QcomGlinkOk) break;
        IOLockLock(gLock);
        gHead = (gHead + 1) % kQueue;
        gCount--;
        IOLockUnlock(gLock);
    }
}

void serviceSmp2p() {
    if (!gP2p.in) {
        if (volatile uint8_t *in = item(kItemIn, nullptr)) gP2p.in = reinterpret_cast<volatile uint32_t *>(in);
        else return;
    }
    uint32_t value = 0, changed = 0;
    if (qcom_smp2p_service(&gP2p, "slave-kernel", &value, &changed)) kickSmp2p();
    gSlave = value;
    native_wpss_stats[3] = (int32_t)value;
    const int wlan = qcom_smp2p_find_in(&gP2p, "wlan"), wake = qcom_smp2p_find_in(&gP2p, "wlan_soc_wake");
    native_wpss_stats[15] = (native_wpss_stats[15] & ~0xffff) | (int32_t)((wlan >= 0 ? qcom_smp2p_in_value(&gP2p, wlan) & 0xff : 0) |
                                                                         (wake >= 0 ? (qcom_smp2p_in_value(&gP2p, wake) & 0xff) << 8 : 0));
    if (changed & value & kFatal) readCrashReason();
    if ((value & kHandover) && !gHandedOver) {
        gHandedOver = true;
        R::voteRails("cx.lvl", "mx.lvl", 0, 0);  // the WPSS holds its own votes now (Linux drops the proxy votes at handover)
    }
}

// A message for the AOP from another thread (the WLAN worker's wlan_pdc configuration, as icnss2 sends over the same QMP channel):
// kept here and sent by this thread, which owns the AOSS QMP mailbox. gAopResult: 1 pending, 0 sent and acknowledged, negative failed.
char gAop[64];
volatile int gAopPending;
volatile int32_t gAopResult = 1;
void serviceAop() {
    if (!gAopPending) return;
    gAopResult = R::qmp(gAop);
    gAopPending = 0;
}
// The post-mortem (the board's wifi-postmortem): once the WPSS died, a PAS shutdown hands its carveout back to this side, as Linux's
// recovery does before its coredump, and the firmware's memory is read by its virtual addresses. gPmState: 1 asked, 2 readable,
// negative failed.
volatile int gPmState;
IOMemoryMap *gPmMap;
volatile uint8_t *gPmVa;
void servicePostmortem() {
    if (gPmState != 1) return;
    R::pasShutdown(kPasWpss);
    IODeviceMemory *memory = IODeviceMemory::withRange(kWpssBase, kWpssSize);
    gPmMap = memory ? memory->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
    if (memory) memory->release();
    gPmVa = gPmMap ? reinterpret_cast<volatile uint8_t *>(gPmMap->getVirtualAddress()) : nullptr;
    gPmState = gPmVa ? 2 : -12;
}
void serviceGlink() {
    if (!gGlinkStarted) return;
    if (!gGlink.rx.data) {
        uint32_t size = 0;
        if (volatile uint8_t *rx = item(kItemGlinkRx, &size)) qcom_glink_set_rx(&gGlink, rx, size);
    }
    qcom_glink_poll(&gGlink);
    pumpSends();
    native_wpss_stats[7] = (int32_t)gGlink.rx_messages;
    native_wpss_stats[8] = (int32_t)gGlink.tx_messages;
    native_wpss_stats[9] = (int32_t)((gChannel && gChannel->up ? 1u : 0u) | (uint32_t)gGlink.version_agreed << 14 |
                                     (uint32_t)gGlink.broken << 15);
    if (gGlink.unknown_opens && strncmp(native_wpss_text, "crash: ", 7)) note("open: ", gGlink.unknown_names);
    if (gGlink.version_agreed && native_wpss_stats[0] >= StReady && native_wpss_stats[0] < StGlink) stage(StGlink);
}

// The module attached (from its thread): the link state and the packets held for it, on this thread.
void replay() {
    if (gReplayed || !gModuleRx) return;
    gReplayed = true;
    if (gLinkWasUp && gModuleState) gModuleState(gModuleContext, 1);
    for (unsigned i = 0; i < gHeld; ++i) {
        gModuleRx(gModuleContext, gBacklog[i].data, gBacklog[i].length);
        IOFreeData(gBacklog[i].data, gBacklog[i].length);
    }
    gHeld = 0;
}
void service() {
    if (!gRunning) return;
    replay();
    native_wpss_stats[2] = native_wpss_stats[2] + 1;
    serviceSmp2p();
    serviceGlink();
    native_wpss_stats[15] = (native_wpss_stats[15] & ~(0xf << 20)) | (int32_t)((gCount & 0xf) << 20);
}

// What the firmware partition holds for the WiFi processor, for the log: the WPSS, board data and regulatory names in image/ (and the
// subdirectories there), then everything in image/qca6750/.
bool has(const char *t, size_t n, const char *k) {
    for (size_t i = 0; i + strlen(k) <= n; ++i) {
        size_t j = 0;
        while (k[j] && (t[i + j] | 0x20) == (k[j] | 0x20)) ++j;
        if (!k[j]) return true;
    }
    return false;
}
// image/: directories and the WLAN names; image/qca6750/: everything but the WPSS image's own segments (wpss.bNN).
bool wanted(const char *t, size_t n) {
    static const char *const keys[] = {"wpss", "bdw", "regdb", "wlan", "qca", "wcn"};
    if (n && t[n - 1] == '/') return true;
    for (const char *k : keys) if (has(t, n, k)) return true;
    return false;
}
void appendNames(const char *all, bool (*keep)(const char *, size_t)) {
    for (const char *t = all; *t;) {
        size_t n = 0;
        while (t[n] && t[n] != ' ') ++n;
        if (keep(t, n) && strlen(native_wpss_files) + n + 2 < sizeof native_wpss_files) {
            if (native_wpss_files[0]) strlcat(native_wpss_files, " ", sizeof native_wpss_files);
            strncat(native_wpss_files, t, n);
        }
        t += n;
        while (*t == ' ') ++t;
    }
}
void listFirmware() {
    constexpr size_t kList = 4096;
    char *all = static_cast<char *>(IOMallocData(kList));
    if (!all) return;
    native_wpss_files[0] = 0;
    if (!asteroids_modem_list("image", all, kList)) appendNames(all, wanted);
    if (!asteroids_modem_list("image/qca6750", all, kList)) {
        strlcat(native_wpss_files, " | qca6750:", sizeof native_wpss_files);
        appendNames(all, [](const char *t, size_t n) {return !(n == 8 && has(t, n, "wpss.b"));});
    }
    IOFreeData(all, kList);
}

// ---- the bring-up ----
int openAndBoot(const char *chosen) {
    const char *sources[3] = {"modem:qca6750/wpss", "modem:wpss", nullptr};
    if (chosen) {sources[0] = chosen; sources[1] = nullptr;}
    static const uint32_t pauses[] = {1, 2, 4, 8, 16, 30, 30};
    int rc = -1;
    for (unsigned attempt = 0;; ++attempt) {
        for (unsigned s = 0; sources[s]; ++s) {
            int32_t kib = 0, x0 = 0;
            rc = R::pasBoot(sources[s], kPasWpss, kWpssBase, kWpssSize, &kib, &x0);
            native_wpss_stats[10] = x0;
            if (rc == -1) continue;  // not there (or the UFS driver not up yet): the next source
            listFirmware();
            native_wpss_stats[5] = kib;
            native_wpss_stats[15] = (native_wpss_stats[15] & ~(0xf << 16)) | (int32_t)(s << 16);
            note("fw: ", sources[s]);
            return rc;
        }
        if (attempt == sizeof pauses / sizeof pauses[0]) {listFirmware(); return rc;}
        IOSleep(pauses[attempt] * 1000);
    }
}

// The WCN6750's WLAN enable (stock devicetree bt_wcn6750: qcom,wl-reset-gpio = <&tlmm 44>), which Android's Bluetooth power driver drives;
// nothing does here, and with it low the firmware asserted in whal_wmac_transition_to_fw_ctrl at the first vdev. Driven high before the
// WPSS starts (wifi-wlen=0 leaves it). TLMM 0xf100000, 0x1000 per pin: config at +0 (bit 9 output enable), in/out at +4 (bit 1 the level).
// [0] config before | in/out before << 16, [1] config | in/out after << 16
constexpr uint64_t kTlmm = 0xf100000;
constexpr unsigned kWlEnPin = 44;
void wlanEnable() {
    unsigned want = 1;
    PE_parse_boot_argn("wifi-wlen", &want, sizeof want);
    IODeviceMemory *memory = IODeviceMemory::withRange(kTlmm + kWlEnPin * 0x1000ull, 0x1000);
    IOMemoryMap *map = memory ? memory->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
    if (memory) memory->release();
    if (!map) return;
    volatile uint32_t *pin = reinterpret_cast<volatile uint32_t *>(map->getVirtualAddress());
    native_wpss_wlen[0] = (pin[0] & 0xffff) | (pin[1] & 0xffff) << 16;
    if (want) {
        pin[1] = 2;            // the level first, so the pin goes high the moment it drives
        pin[0] = 1u << 9;      // GPIO function, output, 2 mA, no pull
        __asm__ volatile("dsb sy" ::: "memory");
        IOSleep(50);
    }
    native_wpss_wlen[1] = (pin[0] & 0xffff) | (pin[1] & 0xffff) << 16;
    native_wpss_wlen[2] = want | 0x100;
    map->release();
}
bool start() {
    wlanEnable();
    if (native_wpss_stats[3] > 0) {fail(StSmp2p, -114); return false;}  // running already (a bootloader started it): left alone
    if (!gHavePart) {fail(StPartition, -2); return false;}
    int rc = R::allocItem(gPart, kItemOut, QCOM_SMP2P_ITEM_BYTES);
    volatile uint8_t *out = rc ? nullptr : item(kItemOut, nullptr);
    if (!out) {fail(StSmp2p, rc ? rc : -2); return false;}
    qcom_smp2p_init_out(&gP2p, reinterpret_cast<volatile uint32_t *>(out), 0, kRemotePid);
    gStop = qcom_smp2p_add_entry(&gP2p, "master-kernel");
    gWlanOut = qcom_smp2p_add_entry(&gP2p, "wlan");
    gWakeOut = qcom_smp2p_add_entry(&gP2p, "wlan_soc_wake");
    gPowerSaveOut = qcom_smp2p_add_entry(&gP2p, "wlan_ep_power_save");
    R::enableDoorbell(R::kIpccWpss, kSignalSmp2p);
    R::enableDoorbell(R::kIpccWpss, kSignalGlink);
    gRunning = true;
    kickSmp2p();
    stage(StSmp2p);

    if ((rc = R::qmp("{class: image, res: load_state, name: wpss, val: on}"))) {fail(StLoadState, rc); return false;}
    // wifi-vrm=1: the WLAN chip's supplies as icnss2 votes them (stock devicetree qcom,wcn6750: vdd-cx-mx = pm7550 S3B 968-1040 mV,
    // vdd-1.8-xo = S1B 1856-2040 mV, vdd-1.3-rfa = S2B 1256-1408 mV): S3B at the top of its range, the others at their minimum.
    unsigned vrm = 0;
    PE_parse_boot_argn("wifi-vrm", &vrm, sizeof vrm);
    if (vrm) {
        char text[80];
        const int a = R::voteVrm("smpb3", 1040), b = R::voteVrm("smpb1", 1856), c = R::voteVrm("smpb2", 1256);
        snprintf(text, sizeof text, "smpb3 1040 %d, smpb1 1856 %d, smpb2 1256 %d", a, b, c);
        note("vrm: ", text);
    }
    stage(StLoadState);
    if ((rc = R::voteRails("cx.lvl", "mx.lvl", kLevelProxy, kLevelProxy))) {
        fail(StRails, rc); R::qmp("{class: image, res: load_state, name: wpss, val: off}"); return false;
    }
    stage(StRails);

    char chosen[80] = "";
    const bool have = PE_parse_boot_argn("wifi-fw", chosen, sizeof chosen) && chosen[0];
    const uint64_t began = mach_absolute_time();
    if ((rc = openAndBoot(have ? chosen : nullptr))) {
        if (rc == -1) note("fw: ", "no wpss image");
        fail(StStarted, rc);
        R::voteRails("cx.lvl", "mx.lvl", 0, 0);
        R::qmp("{class: image, res: load_state, name: wpss, val: off}");
        return false;
    }
    stage(StStarted);
    const bool ready = R::waitUntil(5000, [](void *) {return (gSlave & (kReady | kFatal)) != 0;}, nullptr);
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time() - began, &ns);
    native_wpss_stats[6] = (int32_t)(ns / 1000000);
    if (!ready || (gSlave & kFatal)) {
        readCrashReason();
        fail(StReady, ready ? -5 : -60);
        R::pasShutdown(kPasWpss);
        if (!gHandedOver) R::voteRails("cx.lvl", "mx.lvl", 0, 0);
        return false;
    }
    stage(StReady);
    return true;
}

bool startGlink() {
    int rc = R::allocItem(gPart, kItemGlinkDesc, 32);
    if (!rc) rc = R::allocItem(gPart, kItemGlinkTx, kGlinkFifo);
    uint32_t size = 0;
    volatile uint8_t *desc = item(kItemGlinkDesc, nullptr), *txf = item(kItemGlinkTx, &size);
    if (rc || !desc || !txf) {fail(StGlink, rc ? rc : -2); return false;}
    volatile uint32_t *d = reinterpret_cast<volatile uint32_t *>(desc);
    const QcomGlinkFifo tx = {&d[1], &d[0], txf, size}, rx = {&d[3], &d[2], nullptr, 0};
    qcom_glink_init(&gGlink, tx, rx, kickGlink, nullptr);
    static const QcomGlinkClient client = {channelState, channelReceive};
    gChannel = qcom_glink_channel(&gGlink, "IPCRTR", kIntentSize, kIntents, gIntents, &client, nullptr, 0);
    if (!gChannel) {fail(StGlink, -12); return false;}
    gGlinkStarted = true;
    serviceGlink();
    return true;
}
}  // namespace

extern "C" void asteroids_wpss_run(void) {
    gMode = 0;
    PE_parse_boot_argn("wifi", &gMode, sizeof gMode);
    if (!gMode || native_wpss_stats[0]) return;
    survey();
    stage(StSurveyed);
    if (gMode < 2) return;
    gLock = IOLockAlloc();
    gQueue = static_cast<Packet *>(IOMallocData(sizeof(Packet) * kQueue));  // on the heap: the kernel's __DATA is tight
    gIntents = static_cast<uint8_t *>(IOMallocData(kIntentSize * kIntents));
    if (gMode >= 5) gBacklog = static_cast<Held *>(IOMallocData(sizeof(Held) * kBacklog));
    if (!gLock || !gQueue || !gIntents) {fail(StSmp2p, -12); return;}
    qcom_qrtr_init(&gQrtr, 1, routerSend, nullptr);
    if (!start()) return;
    startGlink();
}

extern "C" void asteroids_wpss_service(void) {serviceAop(); servicePostmortem(); service();}
extern "C" int asteroids_wpss_aop(const char *message) {
    if (gAopPending) return -16;
    strlcpy(gAop, message, sizeof gAop);
    gAopResult = 1;
    gAopPending = 1;
    R::wakeWpss();
    return 0;
}
extern "C" int32_t asteroids_wpss_aop_result(void) {return gAopPending ? 1 : gAopResult;}
// The post-mortem from the WLAN worker: asks for the carveout once (after the WPSS reported a fatal error) and returns gPmState.
extern "C" int asteroids_wpss_postmortem(void) {
    if (!gPmState && gRunning && (gSlave & kFatal)) {gPmState = 1; R::wakeWpss();}
    return gPmState;
}
// Words of the dead firmware's memory: at its virtual address (physical 0: the image's segment map, 0xfe000000.. and 0xc0000000..), or at a
// physical address inside the carveout (physical 1). 0, -1 outside, -19 before the carveout is back.
extern "C" int asteroids_wpss_pm_read(uint64_t address, int physical, uint32_t *out, uint32_t words) {
    if (gPmState != 2) return -19;
    uint64_t pa = 0;
    if (physical) pa = address;
    else if (address >= 0xfe000000u && address < 0xfe074000u) pa = address - 0xfe000000u + 0x9bb00000u;
    else if (address >= 0xc0000000u && address < 0xc15de000u) pa = address - 0xc0000000u + 0x9bc00000u;
    else return -1;
    if (pa < kWpssBase || pa + 4ull * words > kWpssBase + kWpssSize || (pa & 3)) return -1;
    const volatile uint32_t *from = reinterpret_cast<const volatile uint32_t *>(gPmVa + (pa - kWpssBase));
    for (uint32_t i = 0; i < words; ++i) out[i] = from[i];
    return 0;
}
extern "C" int asteroids_wpss_pending(void) {return gRunning && gCount != 0;}

// ---- the router link for the WiFi module (wifi=5): raw QRTR packets both ways ----
// attach: packets from the WPSS go to rx (on the ADSP thread; copy before returning), link changes to state. Returns 0, or -19 when the
// WPSS is not up. send: queues one packet for the WPSS from any thread; 0, or -11 when the queue is full.
extern "C" int asteroids_wpss_qrtr_attach(void (*rx)(void *, const uint8_t *, uint32_t), void (*state)(void *, int), void *context) {
    if (!gLock || native_wpss_stats[0] < StReady) return -19;
    IOLockLock(gLock);
    gModuleContext = context;
    gModuleState = state;
    gModuleRx = rx;
    IOLockUnlock(gLock);
    R::wakeWpss();
    return 0;
}
extern "C" int asteroids_wpss_qrtr_up(void) {return gChannel && gChannel->up;}
extern "C" int asteroids_wpss_qrtr_send(const void *packet, uint32_t length) {
    if (!queuePacket(static_cast<const uint8_t *>(packet), length)) return -11;
    R::wakeWpss();
    return 0;
}
// The WLAN SMP2P bits (ath11k: "wlan" wakes the firmware from WoW). From the ADSP thread or any thread: the update is one word.
extern "C" int asteroids_wpss_smp2p_wlan(uint32_t mask, uint32_t value) {
    if (gWlanOut < 0) return -19;
    qcom_smp2p_update(&gP2p, gWlanOut, mask, value);
    kickSmp2p();
    return 0;
}
// icnss2's SMP2P messages to the WLAN firmware (platform/icnss2 main.c icnss_send_smp2p): value = sequence << 16 | message, a sequence
// per entry. entry 1: "wlan_soc_wake" (4 SOC_WAKE_REQ, 5 SOC_WAKE_REL), entry 2: "wlan_ep_power_save" (6 PCI_EP_POWER_SAVE_ENTER,
// 7 PCI_EP_POWER_SAVE_EXIT), entry 0: "wlan" (1 POWER_SAVE_ENTER, 2 POWER_SAVE_EXIT); message 0 resets the entry (sequence 0). Returns
// the value written, or -19 before the WPSS runs.
extern "C" int32_t asteroids_wpss_smp2p_send(unsigned entry, uint32_t message) {
    static uint32_t sequence[3];
    const int index = entry == 0 ? gWlanOut : entry == 1 ? gWakeOut : entry == 2 ? gPowerSaveOut : -1;
    if (index < 0 || entry > 2) return -19;
    if (!message) sequence[entry] = 0;
    const uint32_t value = message ? (sequence[entry]++ & 0xffff) << 16 | message : 0;
    qcom_smp2p_update(&gP2p, index, 0xffffffffu, value);
    kickSmp2p();
    return (int32_t)value;
}
// Both SMP2P items as text for the log: header (version|features, pids, entry counts, flags) and every entry's name=value, ours first.
extern "C" void asteroids_wpss_smp2p_dump(char *out, uint32_t cap) {
    if (!cap) return;
    out[0] = 0;
    uint32_t at = 0;
    auto put = [&](const char *fmt, auto... v) {
        if (at + 1 >= cap) return;
        const int n = snprintf(out + at, cap - at, fmt, v...);
        if (n > 0) at = at + (uint32_t)n < cap ? at + (uint32_t)n : cap - 1;
    };
    for (int side = 0; side < 2; ++side) {
        const volatile uint32_t *w = side ? gP2p.in : gP2p.out;
        put("%s", side ? " | in" : "out");
        if (!w) {put("%s", " none"); continue;}
        put(" %x %x %x %x %x", w[0], w[1], w[2], w[3], w[4]);
        const uint32_t valid = w[3] >> 16;
        for (uint32_t i = 0; i < valid && i < QCOM_SMP2P_MAX_ENTRIES; ++i) {
            char name[17];
            for (unsigned c = 0; c < 16; ++c) name[c] = (char)(w[5 + 5 * i + c / 4] >> (8 * (c % 4)));
            name[16] = 0;
            put(" %s=%x", name, w[5 + 5 * i + 4]);
        }
    }
}
// The inbound "wlan_soc_wake" value (the firmware's acknowledgement of a wake request), -1 without the entry.
extern "C" int32_t asteroids_wpss_soc_wake_in(void) {
    const int wake = gP2p.in ? qcom_smp2p_find_in(&gP2p, "wlan_soc_wake") : -1;
    return wake >= 0 ? (int32_t)qcom_smp2p_in_value(&gP2p, wake) : -1;
}
