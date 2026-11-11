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

# The vendor's board data files are named by the board id the firmware reports (icnss2: bdwlan.bNN, three hex digits from 0x100 on);
# the copy of core.c asks for that name before board.bin.
rm -f $O/core.c
python3 - "$A/core.c" "$O/core.c" <<'PY'
import sys
src, dst = sys.argv[1], sys.argv[2]
s = open(src).read()
old = '\tret = ath11k_core_fetch_board_data_api_1(ab, bd, ATH11K_DEFAULT_BOARD_FILE);\n'
assert s.count(old) == 1, "core.c board.bin mark"
new = ('\t{\n\t\tchar vendor[24];\n\t\tu32 id = ab->qmi.target.board_id;\n'
       '\t\tsnprintf(vendor, sizeof(vendor), id < 0x100 ? "bdwlan.b%02x" : "bdwlan.b%03x", id);\n'
       '\t\tath11k_info(ab, "board id %x: trying %s\\n", id, vendor);\n'
       '\t\tret = ath11k_core_fetch_board_data_api_1(ab, bd, vendor);\n\t}\n'
       '\tif (!ret) {\n\t\tbd_api = 1;\n\t\tgoto exit;\n\t}\n' + old)
s = s.replace(old, new, 1)
open(dst, 'w').write(s)
PY
grep -q 'bdwlan.b%02x' $O/core.c || { echo "core.c patch missed" >&2; exit 1; }
# The WCN6750 here keeps its address in NV (icnss2's use-nv-mac) and reports none, and ath11k's fallback (the devicetree's address) has
# nothing: the copy of mac.c takes the board's address (nw_board_mac, made from the chip id) for a radio whose address is not valid or
# is the vendor default 00:03:7f, as Ath12kWifi does for ath12k. Without it the interface refuses to open (-EADDRNOTAVAIL).
rm -f $O/mac.c
python3 - "$A/mac.c" "$O/mac.c" <<'PY'
import sys
src, dst = sys.argv[1], sys.argv[2]
s = open(src).read()
old = '\t\t\tar->mac_addr[4] += i;\n\t\t}\n'
assert s.count(old) == 1, "mac.c address mark"
s = s.replace(old, old + '\t\tif (!is_valid_ether_addr(ar->mac_addr) ||\n\t\t    (!ar->mac_addr[0] && ar->mac_addr[1] == 0x03 && ar->mac_addr[2] == 0x7f)) {\n'
              '\t\t\tnw_board_mac(ar->mac_addr);\n\t\t\tar->mac_addr[4] += i;\n\t\t}\n', 1)
s = s.replace('#include "mac.h"\n', '#include "mac.h"\nvoid nw_board_mac(u8 *a);\n', 1)
assert 'void nw_board_mac' in s
open(dst, 'w').write(s)
PY
grep -q 'nw_board_mac(ar->mac_addr)' $O/mac.c || { echo "mac.c patch missed" >&2; exit 1; }
echo "staged ath11k sources: $PWD/$O"
