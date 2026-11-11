# Ath11kWifi

Linux ath11k, mac80211 and cfg80211 on XNU for the WCN6750 of the Nothing Phone (3a)
(asteroids, SM7635). An in-tree fork of [UniXNU/Ath12kWifi](https://github.com/UniXNU/Ath12kWifi)
at 5f7f5a8: its runtime (shim, cfg80211/mac80211 start-up, native control endpoint,
buffers, QRTR router and name service) is kept as it is; the driver and the bus are
ath11k's and the WCN6750's.

## What differs from Ath12kWifi

| Area | Ath12kWifi (WCN7850) | Ath11kWifi (WCN6750) |
| --- | --- | --- |
| Driver | ath12k, wifi7 | ath11k (`tools/sources.txt`, `tools/prepare-ath11k.sh`) |
| Bus | PCIe endpoint, MHI | the hybrid bus of `ath11k_ahb`: no PCI, registers by physical address |
| Firmware start | MHI boot of amss.bin | the WPSS (q6v5, PAS) runs the WLAN firmware; the board starts it |
| QRTR transport | MHI channel IPCR | the WPSS's GLINK channel IPCRTR, through the board (`nw_board.qrtr_send`, `nw_wifi_qrtr_rx`) |
| Interrupts | MSI vectors through the root port | GIC SPIs the chip raises by writing their numbers to a GIC register (`msi_addr`) |
| Firmware files | amss.bin, m3.bin, board-2.bin | board data (board-2.bin or board.bin, the vendor's ELF accepted) and regdb.bin |

`src/nw_ahb.c` is the platform device ath11k_ahb probes: the devicetree properties
the WCN6750 path reads (`qcom,rproc`, `interrupts`), the MSI resource, irq numbers,
the remote processor (already running: boot waits for the board's router link) and
the SMP2P "wlan" state. `shim/include/lx_ath11k.h` maps the Linux calls ath11k makes
beyond ath12k's onto it. `src/nw_debug.c` replaces ath11k's `debug.c`, which prints
with `%pV`. `src/nw_wifi.c` starts cfg80211, mac80211 and the ath11k_ahb driver.

`include/nw_abi.h` adds the hybrid-bus fields to `struct nw_board`: `msi_addr`,
`msi_spi_first`, `msi_count`, `qrtr_send`, `rproc_up`, `smp2p_wlan`, and the receive
entry points `nw_wifi_qrtr_rx` and `nw_wifi_qrtr_down`, called on the wireless worker.

`tools/prepare-ath11k.sh` lays out `out/ath11k-src` after `tools/prepare.sh`: links to
the pinned sources, empty trace points, a 1024-entry receive refill ring (4096 in
Linux) and the WMI send wait evaluated once per round, as Ath12kWifi does for ath12k.

## Board integration (asteroids)

`package/WpssProbe.cpp` starts the WPSS on the ADSP thread and owns its GLINK edge;
`package/AsteroidsWifi.cpp` runs this module on the "wifi" thread with a 48 MiB
uncached DMA window in a pass-through SMMU stream (0x1400, 0x1401) and hands QRTR
packets between the two threads. `scripts/build-ath11k-wifi.py` compiles the module;
`scripts/build-native-legacy-display-27.py` links it into the board archive.

## Build

```sh
python3 tools/fetch-sources.py --check
bash tools/prepare.sh && bash tools/prepare-ath11k.sh
```

The board build runs both through `scripts/build-ath11k-wifi.py`. The Ath12kWifi
host fixtures (`tools/test-*.py`) exercise the shared runtime and still apply to it;
the ath12k-specific checks (`check-ath12k.sh`, `check-mhi.sh`) do not apply.
