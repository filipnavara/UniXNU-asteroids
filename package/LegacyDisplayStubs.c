/* SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 * Weak fallbacks for the sheng legacy display probes when the first-console
 * configuration disables the BootProgress layer and the Adreno GPU probe.
 * The full sheng configuration links the real strong definitions; a weak
 * definition here is then discarded by the linker.
 *
 * boot_progress_prepare_scanout returns 1 in the real layer when boot
 * progress is disabled, which is the "no diagnostic writer" answer the
 * display probe expects.
 */
#include <stdbool.h>
#include <stdint.h>

__attribute__((weak)) int
boot_progress_pulse_anchor (uintptr_t base, uint64_t bytes, unsigned width,
                            unsigned height, unsigned row_bytes)
{
  (void)base;
  (void)bytes;
  (void)width;
  (void)height;
  (void)row_bytes;
  return 0;
}

__attribute__((weak)) int
boot_progress_prepare_scanout (void)
{
  return 1;
}

__attribute__((weak)) void
boot_progress_cancel_scanout (void)
{
}

__attribute__((weak)) void
boot_progress_stage (const char *text)
{
  (void)text;
}

__attribute__((weak)) char boot_progress_ext[8][48];
__attribute__((weak)) char boot_progress_ext_wide[2][112];

/* sheng_gpu_power_probe: GpuPowerProbe.cpp (asteroids modes 1-2) is always built; a weak stub here could be picked from the
 * archive first and keep the real one out, so there is none. */

/*
 * NativeLegacyDisplay always references the GPU hand-off hooks, even when
 * CONFIG_XNU_NATIVE_ADRENO is off and GpuPowerProbe.cpp is not compiled.
 * The weak definitions keep the display-only kernel linkable; the Adreno
 * configuration supplies the strong definitions and discards these.
 */
/* asteroids T30: GpuPowerProbe.cpp's user line reads uke's display swap counters, which the T24+ display probe does not
 * define; the static kernel link needs a definition, so these weak zeros stand in. */
__attribute__((weak)) volatile uint32_t asteroids_swap_end_calls, asteroids_swap_end_size, asteroids_swap_end_id,
    asteroids_swap_end_status, asteroids_swap_wait_calls;

/* asteroids T35: GopFramebuffer 33e941a's direct scanout maps swap surfaces through UniXNU's ShengUfs display SMMU window
 * (native_ufs_dpu_map); our NativeUfs fork predates it. State 0 (no window) keeps direct scanout off, as before. */
__attribute__((weak)) volatile uint32_t native_ufs_dpu_state;

__attribute__((weak)) uint32_t
native_ufs_dpu_map (unsigned slot, const uint64_t *pages, unsigned count)
{
	(void)slot; (void)pages; (void)count;
	return 0;
}

__attribute__((weak)) bool
native_legacy_gpu_offer (void *display, void *memory, uint64_t stride,
                         uint32_t width, uint32_t height, uint32_t format)
{
  (void)display;
  (void)memory;
  (void)stride;
  (void)width;
  (void)height;
  (void)format;
  return false;
}

__attribute__((weak)) void
native_legacy_gpu_fallback_observed (int status)
{
  (void)status;
}

__attribute__((weak)) void
native_legacy_swap_stage (unsigned stage)
{
  (void)stage;
}

__attribute__((weak)) void
native_legacy_surface_rejected (uint64_t width, uint64_t height, uint32_t format,
                                uint32_t reason, uint64_t a, uint64_t b, uint64_t c)
{
  (void)width;
  (void)height;
  (void)format;
  (void)reason;
  (void)a;
  (void)b;
  (void)c;
}

/* QEMU-only companions of the CPU display probe. The uke CPU baseline has no
 * shared window, virtio input or QEMU Metal backend, so these no-ops resolve
 * the QEMU LegacyDisplayProbe's optional late probes. */
__attribute__((weak)) int
native_qemu_metal_publish (void)
{
  return 0;
}

__attribute__((weak)) int
native_shared_window_probe (void)
{
  return 0;
}

__attribute__((weak)) int
native_virtio_input_probe (void)
{
  return 0;
}

/* Board stand-ins: weak on arm64 (iPadOS 17), where a real definition may be linked beside them; strong on arm64e (iOS 27),
 * whose chained-fixup kernel keeps every weak definition as a by-name import that the prelinker cannot bind. */
#ifndef BOARD_STANDIN
#if defined(__arm64e__)
#define BOARD_STANDIN
#else
#define BOARD_STANDIN __attribute__((weak))
#endif
#endif
/* native_adreno_fences, native_adreno_fences_detail and the GPU trace (native_gpu_trace_*) come from Adreno740 itself since
 * the 2026-11-12 upstream sync (NativeAdrenoPublish.cpp); the board's stand-ins for them are gone. */

/* Upstream Adreno740's pool views (NativeAdrenoPublish.cpp) ask the kernel to report a device pager's close; only UniXNU's macOS
 * kernel has the hook (gpupool-device-pager-close.patch). The board registers no such pool, so no view is made: refuse. */
BOARD_STANDIN int
iokit_physical_descriptor_close_notify (void *descriptor, void (*closed)(void *), void *context)
{
  (void)descriptor;
  (void)closed;
  (void)context;
  return 5; /* KERN_FAILURE: the view is not tracked */
}

/* iPadOS 17's boot progress kit (BootFaultProbe.c) records the caller stack; iOS 27 has no such kit: no frames. */
BOARD_STANDIN unsigned
boot_progress_caller_stack (uint64_t *out, unsigned limit, uint64_t *slide)
{
  (void)out;
  (void)limit;
  if (slide)
    *slide = 0;
  return 0;
}

/* The WiFi worker (AsteroidsWifi.cpp) is built on iOS 27 only; iPadOS 17's late probe calls this instead. */
#if !defined(__arm64e__)
__attribute__((weak)) void asteroids_wifi_probe (void) {}
#endif
