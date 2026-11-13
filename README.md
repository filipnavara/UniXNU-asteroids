# UniXNU-asteroids

[UniXNU](https://github.com/UniXNU/UniXNU) device repository for the **Nothing Phone (3a)** (`asteroids`, Qualcomm
SM7635 "Volcano"). Work in progress: not hardware-complete, and nothing here is meant to be flashed yet (images are booted with
`fastboot boot`; the OS lives in `userdata`).

| Repository | Contents |
|---|---|
| [filipnavara/uefi-asteroids](https://github.com/filipnavara/uefi-asteroids) (branch `asteroids-8gb`) | Mu-Silicium UEFI port: `VolcanoPkg` and `asteroidsPkg` |
| [filipnavara/UniXNU](https://github.com/filipnavara/UniXNU) (branch `asteroids-resync`) | the UniXNU core fork the port builds with: v3/v4 boot images, the payload carried in the kernel image, bring-up patches, CI |
| [rootstockos/UniXNU-sm7635-common](https://github.com/rootstockos/UniXNU-sm7635-common) | the shared SM7635 profile (device format 2): CPUs, GIC, UFS and Adreno 810 resources |
| this repository | `board.toml`, `dts/asteroids.dts`, kernel defconfigs, board XNU patches, device package (`kits/asteroids`), device scripts |

The repository uses UniXNU's device format 2 (`schema_version = 2`): the SM7635 SoC profile, the iPadOS 17.7.11 (21H461) OS
profile and catalog components. Its own driver forks are submodules at their catalog paths, where they take precedence over
UniXNU's:

| Module | Repository | Use |
|---|---|---|
| `drivers/ShengUfs` | [filipnavara/NativeUfs](https://github.com/filipnavara/NativeUfs) (branch `asteroids`) | UFS root in `userdata` |
| `drivers/Adreno740` | [filipnavara/NativeAdreno](https://github.com/filipnavara/NativeAdreno) (branch `asteroids`) | Adreno 810 (gen8): GMU, ring, submits and the user client |
| `drivers/ArmDTPlatform` | [filipnavara/ArmDTPlatform](https://github.com/filipnavara/ArmDTPlatform) (branch `asteroids`) | Platform expert; forwards the wall clock to the PMIC RTC (counter + offset on Data) |
| `drivers/ShengHidInput` | [UniXNU/ShengHidInput](https://github.com/UniXNU/ShengHidInput) | HID touch and buttons |
| `drivers/GopFramebuffer` | [filipnavara/GopFramebuffer](https://github.com/filipnavara/GopFramebuffer) (branch `asteroids`) | GOP framebuffer and the GPU copy presenter |
| `kits/AdrenoFirmware` | [filipnavara/AdrenoFirmware](https://github.com/filipnavara/AdrenoFirmware) (branch `asteroids`) | GMU/HFI kit with the A8xx performance table and votes |

## State (2026-11-13)

**iPadOS 27.0.1 (24A446, the iPad17,3 images)** boots from UFS to the home screen on all eight cores (2026-11-11): touch and
buttons work, the GPU add-on renders through Metal on the Adreno 810 (metal2vulkan and Turnip), the system apps are there,
icons load (rendered on the CPU) and the wallpaper shows on the home and lock screens. Open issues:

- graphic artifacts in GPU-rendered content; zero-copy scanout (`dpu-scanout=2`) is off;
- the GPU fault and freeze after the 2026-11-12 sync (about half of the boots, mostly once the lock screen was touched) is
  fixed (2026-11-13, UniXNU 1aa69d2: the Darwin Turnip backend wiped a live BO's entry); the kernel still does not restart
  the GPU after a hang, so a hang freezes the UI until a reboot;
- iconservicesagent renders icons on the CPU (RenderBox aborted on the GPU once per boot);
- Wi-Fi (WCN6750, an ath11k port) works and is on by default (`wifi=5` in `board.toml` since 2026-11-13; that image is not
  yet booted): Settings lists networks and joins them, web pages load; Bluetooth is not started (see `porting.toml`, area `network`).

iPadOS 17.7.11 (21H461) remains the board's default profile and boots to the desktop as before. `porting.toml` records each
area; older test results are in `docs/asteroids-gpu-gen8.md` and `docs/asteroids-touch.md` of rootstockos/iosonandng.

Several vendor drivers are disabled in the UEFI port for bring-up; `Platforms/Nothing/asteroidsPkg/PORTING.md` there
lists them and why.

## Build

From the UniXNU checkout, with UniXNU-sm7635-common next to this repository (`local.toml` points the SoC source at
`../UniXNU-sm7635-common` until `feeds.toml`'s pin is published):

```sh
./unixnu defconfig asteroids_defconfig -D ../UniXNU-asteroids
./unixnu status --json -D ../UniXNU-asteroids
./unixnu build kernel -D ../UniXNU-asteroids
./unixnu build payload -D ../UniXNU-asteroids --uefi ../uefi-asteroids --edk2 ../uefi-asteroids/Mu_Basecore
./unixnu bootimg -D ../UniXNU-asteroids --uefi ../uefi-asteroids --dtb /path/to/live.dtb
```

Outputs are under `out/asteroids/ipados-17.7.11-21H461/asteroids/`.

For iPadOS 27 add `--os ipados-27.0.1-24A446` to each command (outputs under `out/asteroids/ipados-27.0.1-24A446/asteroids_ios27/`).
The GPU add-on build needs on `PATH` bison 3.8.2 (Xcode's 2.3 fails ir3_parser), glslang's `glslangValidator`, a `glslc`, and
the Rust `rust-src` component. A first boot, or one after the data protection settings change, needs a payload built with
`--kernel-arg data-reset=1`. The kernel builds in CI (rootstockos/UniXNU's macOS
workflow, with `scripts/ci-bootstrap-candidate.sh` as its pre-build step); the payload step compiles `dts/asteroids.dts`
with `dtc` and writes the ADT with UniXNU's shared generator.

`live.dtb` is the phone's `/sys/firmware/fdt`. It carries the `splash_region` the bootloader needs to keep the
panel on.

## Third-party sources in `package/`

The boot progress kit (`BootProgress*.{c,h}`, `BootFaultProbe.c`, `BootDiagExport.c`, `BootUiTrace*`, `BootProcessProbe.c`)
is UniXNU-sheng's (commit 0e1f6e5; MIT, `BootProcessProbe.c` BSD-2-Clause-Patent), unchanged. `Font.h` is the AOSP
framebuffer font from Mu-Silicium's FrameBufferSerialPortLib (its licence is in the file). `BootWatchdogPolicy.c` is
Allenwdk/UniXNU-uke's (MIT) with asteroids additions.
