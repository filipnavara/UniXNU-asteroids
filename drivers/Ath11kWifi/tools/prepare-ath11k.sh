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
# The receive refill ring keeps Linux's 4096 buffers: with 1024 (Ath12kWifi halves its WCN7850 ring to save DMA window) the WCN6750
# firmware asserted in whalRecvDmaLeakRecovery right after the first scan; its per-MAC rings take 2048 each from this one.
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
# WCN6750 without idle power save (IMPS): the vendor firmware (WLAN.MSL.3.0.1) asserted in whal_wmac_transition_to_fw_ctrl when the first
# vdev woke the MAC from IMPS; icnss2 brackets that wake with a SMP2P wlan_soc_wake request this host does not make.
start = s.index('.hw_rev = ATH11K_HW_WCN6750_HW10')
at = s.index('.idle_ps = true,', start)
assert at - start < 4000, "core.c wcn6750 idle_ps mark"
s = s[:at] + '.idle_ps = false,' + s[at + len('.idle_ps = true,'):]
# NW_RX_MAC_BUF_RING=0 (an experiment, not the default): the host refill ring as RXDMA's own buffer ring (HTT_RXDMA_HOST_BUF_RING, SW to HW)
# instead of the mobile model (HTT_HOST1_TO_FW_RXBUF_RING, the firmware copies into per-MAC rings). With the mobile model the firmware
# never takes a buffer from the refill ring and asserts in whalRecvDmaLeakRecovery after the first scan; with this one it stopped
# answering WMI at the radio's start (no assert).
import os
if os.environ.get('NW_RX_MAC_BUF_RING', '1') == '0':
    at = s.index('.rx_mac_buf_ring = true,', start)
    assert at - start < 4000, "core.c wcn6750 rx_mac_buf_ring mark"
    s = s[:at] + '.rx_mac_buf_ring = false,' + s[at + len('.rx_mac_buf_ring = true,'):]
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
# No default monitor-status TLV filter at start: the vendor host (qcacld, MCL) enables status-ring TLVs only for packet logging; with
# ath11k's default the WLAN.MSL.3 firmware asserted in whalRecvDmaLeakRecovery during the first scan.
old2 = '\tret = ath11k_mac_config_mon_status_default(ar, true);\n'
assert s.count(old2) == 1, "mac.c mon status mark"
s = s.replace(old2, '\tret = 0; /* Ath11kWifi: no default monitor-status filter (see tools/prepare-ath11k.sh) */\n', 1)
assert 'void nw_board_mac' in s
open(dst, 'w').write(s)
PY
grep -q 'nw_board_mac(ar->mac_addr)' $O/mac.c || { echo "mac.c patch missed" >&2; exit 1; }
# The capability reply carries the open-loop voltage the chip's CX rail needs (voltage_mv; icnss2 votes the "qcom,vreg_ol_cpr" rail, smpb3
# here, to it). ath11k decodes it and drops it: the copy of qmi.c logs it and leaves it in nw_wifi_cpr_mv for the board.
rm -f $O/qmi.c
python3 - "$A/qmi.c" "$O/qmi.c" <<'PY'
import sys
src, dst = sys.argv[1], sys.argv[2]
s = open(src).read()
old = '\tath11k_info(ab, "chip_id 0x%x chip_family 0x%x board_id 0x%x soc_id 0x%x\\n",\n'
assert s.count(old) == 1, "qmi.c chip_id mark"
s = s.replace(old, '\tif (resp.voltage_mv_valid) {\n\t\textern u32 nw_wifi_cpr_mv;\n\t\tnw_wifi_cpr_mv = resp.voltage_mv;\n'
              '\t\tath11k_info(ab, "cpr voltage %u mV\\n", resp.voltage_mv);\n\t}\n' + old, 1)
# The vendor's device information reply (platform/cnss_utils wlfw_device_info_resp_msg_v01) also carries the address and size of the
# firmware's MHI state words (TLVs 0x12/0x13; icnss2 reads the WPSS-chip link state at +0 and the endpoint-awake word at +4): decoded
# into nw_wifi_mhi_pa/size for the board's SoC wake diagnostics.
ext = """/* Ath11kWifi: the vendor's longer device information reply (cnss_utils wlfw_device_info_resp_msg_v01). */
struct nw_device_info_resp {
\tstruct qmi_response_type_v01 resp;
\tu8 bar_addr_valid;
\tu64 bar_addr;
\tu8 bar_size_valid;
\tu32 bar_size;
\tu8 mhi_valid;
\tu64 mhi_addr;
\tu8 mhi_size_valid;
\tu32 mhi_size;
};
#define NW_DI(t, ty, sz, f) { .data_type = ty, .elem_len = 1, .elem_size = sz, .array_type = NO_ARRAY, .tlv_type = t, \\
\t.offset = offsetof(struct nw_device_info_resp, f) }
static const struct qmi_elem_info nw_device_info_resp_ei[] = {
\t{ .data_type = QMI_STRUCT, .elem_len = 1, .elem_size = sizeof(struct qmi_response_type_v01), .array_type = NO_ARRAY,
\t  .tlv_type = 0x02, .offset = offsetof(struct nw_device_info_resp, resp), .ei_array = qmi_response_type_v01_ei },
\tNW_DI(0x10, QMI_OPT_FLAG, sizeof(u8), bar_addr_valid), NW_DI(0x10, QMI_UNSIGNED_8_BYTE, sizeof(u64), bar_addr),
\tNW_DI(0x11, QMI_OPT_FLAG, sizeof(u8), bar_size_valid), NW_DI(0x11, QMI_UNSIGNED_4_BYTE, sizeof(u32), bar_size),
\tNW_DI(0x12, QMI_OPT_FLAG, sizeof(u8), mhi_valid), NW_DI(0x12, QMI_UNSIGNED_8_BYTE, sizeof(u64), mhi_addr),
\tNW_DI(0x13, QMI_OPT_FLAG, sizeof(u8), mhi_size_valid), NW_DI(0x13, QMI_UNSIGNED_4_BYTE, sizeof(u32), mhi_size),
\t{ .data_type = QMI_EOTI, .array_type = NO_ARRAY, .tlv_type = QMI_COMMON_TLV_TYPE },
};
extern u64 nw_wifi_mhi_pa;
extern u32 nw_wifi_mhi_size;

"""
fn = 'static int ath11k_qmi_request_device_info(struct ath11k_base *ab)\n'
assert s.count(fn) == 1, "qmi.c device info mark"
s = s.replace(fn, ext + fn, 1)
for a, b in (('\tstruct qmi_wlanfw_device_info_resp_msg_v01 resp = {};\n', '\tstruct nw_device_info_resp resp = {};\n'),
             ('\t\t\t   qmi_wlfw_device_info_resp_msg_v01_ei, &resp);\n', '\t\t\t   nw_device_info_resp_ei, &resp);\n'),
             ('\tab->mem = bar_addr_va;\n', '\tab->mem = bar_addr_va;\n\tif (resp.mhi_valid) {\n\t\tnw_wifi_mhi_pa = resp.mhi_addr;\n'
              '\t\tnw_wifi_mhi_size = resp.mhi_size_valid ? resp.mhi_size : 0;\n'
              '\t\tath11k_info(ab, "mhi state info 0x%llx size 0x%x\\n", resp.mhi_addr, nw_wifi_mhi_size);\n\t}\n')):
    assert s.count(a) == 1, a
    s = s.replace(a, b, 1)
open(dst, 'w').write(s)
PY
grep -q 'nw_wifi_mhi_pa = resp.mhi_addr' $O/qmi.c || { echo "qmi.c device info patch missed" >&2; exit 1; }
grep -q 'nw_wifi_cpr_mv = resp.voltage_mv' $O/qmi.c || { echo "qmi.c patch missed" >&2; exit 1; }
echo "staged ath11k sources: $PWD/$O"
