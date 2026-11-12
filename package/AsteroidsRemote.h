// SPDX-License-Identifier: BSD-2-Clause-Patent
// asteroids: what the ADSP thread (AdspProbe.cpp) lends to a second remote processor it brings up, the WPSS (WpssProbe.cpp). Both
// processors share one IPCC interrupt, one AOSS mailbox, one RPMh TCS, one SMEM and its hardware mutex, so one thread owns all of them and
// the WPSS code runs on it: asteroids_wpss_run() once the DSP is up, asteroids_wpss_service() for each IPCC doorbell of the WPSS client and
// for each client send. Every function here is called on that thread only.
#ifndef ASTEROIDS_REMOTE_H
#define ASTEROIDS_REMOTE_H
#include <stddef.h>
#include <stdint.h>
#include "QcomSmem.h"

namespace asteroids_remote {
// IPCC client of the WPSS (qcom,ipcc mboxes <&ipcc_mproc 0x18 ...> in the 3a's devicetree).
constexpr uint32_t kIpccWpss = 24;
// Rings client:signal; enables the receive of client:signal.
void ring(uint32_t client, uint32_t signal);
void enableDoorbell(uint32_t client, uint32_t signal);
// Runs events (the DSP's and the WPSS's) until done(context) holds or ms pass; returns done(context).
bool waitUntil(uint32_t ms, bool (*done)(void *), void *context);
// The SMEM window, an item allocated in a partition under the hardware mutex (0 or a QcomSmem error), an item's address.
const QcomSmem &smem();
int allocItem(const QcomSmemPartition &part, uint16_t item, uint32_t size);
volatile uint8_t *item(const QcomSmemPartition &part, uint16_t id, uint32_t *size);
// One message to the AOSS (the open QMP channel of the DSP bring-up); 0 when the AOSS took it within a second.
int qmp(const char *message);
// Votes two RPMh ARC resources (cmd-db names) to the levels given (RPMh level values; 0 drops the vote). 0 or a negative error.
int voteRails(const char *a, const char *b, uint32_t level_a, uint32_t level_b);
// A PAS image: firmware path (as sheng_fw_open takes it), authenticated, its segments loaded into base/size, then started.
// Returns 0, or -step (1 open, 2 parse, 3 init_image, 4 mem_setup, 5 load, 6 auth_and_reset); *kib gets the file size in KiB.
int pasBoot(const char *path, uint32_t pas, uint64_t base, uint64_t size, int32_t *kib, int32_t *scm_x0);
void pasShutdown(uint32_t pas);
// A VRM regulator (cmd-db name) on at mv millivolts (an active vote); 0 or a negative error.
int voteVrm(const char *name, uint32_t mv);
// From any thread: makes the thread call asteroids_wpss_service() soon.
void wakeWpss();
}  // namespace asteroids_remote

// Implemented by WpssProbe.cpp, which the board build always links beside AdspProbe.cpp.
extern "C" void asteroids_wpss_run(void);
extern "C" int asteroids_wifi_sleep_clock(void);   // AsteroidsWifi.cpp: PMK8550 GPIO5, the WCN's 32 kHz sleep clock
extern "C" void asteroids_wpss_service(void);
extern "C" int asteroids_wpss_pending(void);
#endif
