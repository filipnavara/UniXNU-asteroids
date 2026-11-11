#!/bin/bash
set -euo pipefail
# Lays out out/ath11k-src for the WCN6750 build: links to the pinned Linux ath11k sources with empty trace points, the two headers its sources
# include from the parent directory, and patched copies where this board needs a different value. Run after tools/prepare.sh.
cd "$(dirname "$0")/.."
L=$PWD/deps/linux
A=$L/drivers/net/wireless/ath/ath11k; O=out/ath11k-src
rm -rf $O; mkdir -p $O
for f in $A/*.c $A/*.h; do ln -s $f $O/; done
for f in spectral_common.h testmode_i.h; do ln -sf $L/drivers/net/wireless/ath/$f out/$f; done
rm -f $O/trace.h
{ echo '#ifndef LX_ATH11K_TRACE_H'; echo '#define LX_ATH11K_TRACE_H'
  grep -ohE "trace_ath11k_[a-z_0-9]+" $A/*.c $A/*.h | sort -u | while read t; do
    case $t in *_enabled) echo "#define $t() 0";; *) echo "#define $t(...) ((void)0)";; esac; done
  echo '#endif'; } > $O/trace.h
# The receive refill ring is the largest user of the DMA window (4096 buffers of 2 KiB): 1024 here, as Ath12kWifi's 512 for the WCN7850.
rm -f $O/dp.h; sed -e 's/^#define DP_RXDMA_BUF_RING_SIZE\t\t4096/#define DP_RXDMA_BUF_RING_SIZE\t\t1024/' $A/dp.h > $O/dp.h
grep -q 'DP_RXDMA_BUF_RING_SIZE.*1024' $O/dp.h || { echo "dp.h patch missed" >&2; exit 1; }
# Sending a command is the condition of the wait in ath11k_wmi_cmd_send: that wait must evaluate it once per round and never after it has succeeded.
rm -f $O/wmi.c; sed '/^int ath11k_wmi_cmd_send(/,/^}/ s/wait_event_timeout(/wait_event_timeout_once(/' $A/wmi.c > $O/wmi.c
grep -q 'wait_event_timeout_once' $O/wmi.c || { echo "wmi.c patch missed" >&2; exit 1; }
echo "staged ath11k sources: $PWD/$O"
