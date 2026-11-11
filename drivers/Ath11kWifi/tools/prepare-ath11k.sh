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
    case $t in *_enabled) echo "#define $t() 0";;
      # The firmware's diag messages (WMI_DIAG_EVENTID, on with the QMI ini's enablefwlog): kept by src/nw_debug.c for the board's log.
      trace_ath11k_wmi_diag) echo "void nw_fw_diag(const void *data, unsigned long len);"; echo "#define $t(ab, d, n) nw_fw_diag(d, n)";;
      *) echo "#define $t(...) ((void)0)";; esac; done
  echo '#endif'; } > $O/trace.h
# The receive refill ring keeps Linux's 4096 buffers: with 1024 (Ath12kWifi halves its WCN7850 ring to save DMA window) the WCN6750
# firmware asserted in whalRecvDmaLeakRecovery right after the first scan; its per-MAC rings take 2048 each from this one.
# Sending a command is the condition of the wait in ath11k_wmi_cmd_send: that wait must evaluate it once per round and never after it has succeeded.
rm -f $O/wmi.c; sed '/^int ath11k_wmi_cmd_send(/,/^}/ s/wait_event_timeout(/wait_event_timeout_once(/' $A/wmi.c > $O/wmi.c
grep -q 'wait_event_timeout_once' $O/wmi.c || { echo "wmi.c patch missed" >&2; exit 1; }
# The firmware asks for host memory in WMI_SERVICE_READY (mem_reqs: req_id, unit size, units, flags); Qualcomm's host allocates every
# request and lists the chunks in WMI_INIT (qca-wifi-host-cmn target_if/init_deinit init_deinit_handle_host_mem_req). ath11k ignores
# them, so the WCN6750 vendor firmware would run without memory it counted on. The copy of wmi.c logs each request and, unless the
# board clears nw_wifi_wmi_mem, allocates it (units: the request's, or the peers + 1 for NUM_UNITS_IS_NUM_PEERS/ACTIVE_PEERS as the
# vendor host does, ath11k's WCN6750 config has 16 peers).
python3 - "$O/wmi.c" <<'PY'
import sys
path = sys.argv[1]
s = open(path).read()
old = """	case WMI_TAG_ARRAY_UINT32:
		if (!svc_ready->wmi_svc_bitmap_done) {"""
assert s.count(old) == 1, "wmi.c svc ready mark"
new = """	case WMI_TAG_ARRAY_STRUCT: {
		/* Ath11kWifi: the firmware's host memory requests (see tools/prepare-ath11k.sh). */
		extern unsigned nw_wifi_wmi_mem;
		const u32 *w = ptr;
		u32 n = len / 20, i;

		ath11k_info(ab, "service ready: %u host memory requests\\n", n);
		for (i = 0; i < n; i++, w += 5) {
			u32 req_id = w[1], unit = w[2], info = w[3], units = w[4], bytes, idx;
			dma_addr_t paddr;
			void *vaddr;

			if (FIELD_GET(WMI_TLV_TAG, w[0]) != WMI_TAG_WLAN_HOST_MEM_REQ)
				break;
			if (info & 0x6)
				units = 16 + 1;
			bytes = units * unit;
			ath11k_info(ab, "host memory request %u: id %u unit %u info 0x%x units %u -> %u bytes\\n",
				    i, req_id, unit, info, w[4], bytes);
			if (!nw_wifi_wmi_mem || !bytes || ab->wmi_ab.num_mem_chunks >= WMI_MAX_MEM_REQS)
				continue;
			vaddr = dma_alloc_coherent(ab->dev, bytes, &paddr, GFP_KERNEL);
			if (!vaddr) {
				ath11k_warn(ab, "host memory request %u: %u bytes not allocated\\n", i, bytes);
				continue;
			}
			idx = ab->wmi_ab.num_mem_chunks++;
			ab->wmi_ab.mem_chunks[idx].vaddr = vaddr;
			ab->wmi_ab.mem_chunks[idx].paddr = paddr;
			ab->wmi_ab.mem_chunks[idx].len = bytes;
			ab->wmi_ab.mem_chunks[idx].req_id = req_id;
		}
		break;
	}
""" + old
s = s.replace(old, new, 1)
open(path, 'w').write(s)
PY
grep -q 'host memory requests' $O/wmi.c || { echo "wmi.c mem req patch missed" >&2; exit 1; }
# WMI_INIT with the vendor host's values where they differ from ath11k's WCN6750 ones (qcacld wma_set_default_tgt_config, wma's
# service-ready handlers, wmi_copy_resource_config), each behind a bit of nw_wifi_wmi_vendor (board: wifi-wmi-vendor=MASK, 0 keeps
# ath11k's): 1 rx decap Ethernet (2) instead of native Wi-Fi (1); 2 sizing, 38 peers and 76 TIDs instead of 16 and 32; 4 flag1 as
# the vendor sets it (MGMT_COMP_EVT_BUNDLE bit 9, TX_MSDU_ID_NEW_PARTITION bit 10, IPA_DISABLE bit 30; not BSS_CHANNEL_INFO_64 bit
# 5); 8 flags2 without ath11k's CALC_NEXT_DTIM.
python3 - "$O/wmi.c" <<'PY'
import sys
path = sys.argv[1]
s = open(path).read()
old = "\tmemcpy(&wmi_ab->wlan_resource_config, &config, sizeof(config));\n"
assert s.count(old) == 1, "wmi.c init config mark"
new = """\t{
\t\t/* Ath11kWifi: the vendor host's values (see tools/prepare-ath11k.sh). */
\t\textern unsigned nw_wifi_wmi_vendor;
\t\tif (nw_wifi_wmi_vendor & 1)
\t\t\tconfig.rx_decap_mode = TARGET_DECAP_MODE_ETH;
\t\tif (nw_wifi_wmi_vendor & 2) {
\t\t\tconfig.num_peers = 38;
\t\t\tconfig.num_tids = 76;
\t\t}
\t\tif (nw_wifi_wmi_vendor & 4)
\t\t\tconfig.flag1 = (config.flag1 & ~BIT(5)) | BIT(9) | BIT(10) | BIT(30);
\t\tath11k_info(ab, "wmi init: vendor values 0x%x: decap %u peers %u tids %u flag1 0x%x\\n", nw_wifi_wmi_vendor,
\t\t\t    config.rx_decap_mode, config.num_peers, config.num_tids, config.flag1);
\t}
""" + old
s = s.replace(old, new, 1)
old = "\twmi_cfg->flags2 = WMI_RSRC_CFG_FLAG2_CALC_NEXT_DTIM_COUNT_SET;\n"
assert s.count(old) == 1, "wmi.c flags2 mark"
s = s.replace(old, "\t{\n\t\textern unsigned nw_wifi_wmi_vendor;\n\t\twmi_cfg->flags2 = (nw_wifi_wmi_vendor & 8) ? 0 : WMI_RSRC_CFG_FLAG2_CALC_NEXT_DTIM_COUNT_SET;\n\t}\n", 1)
open(path, 'w').write(s)
PY
grep -q 'nw_wifi_wmi_vendor & 8' $O/wmi.c || { echo "wmi.c vendor init patch missed" >&2; exit 1; }

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
s = s.replace(old, '\tif (resp.voltage_mv_valid) {\n\t\textern u32 nw_wifi_cpr_mv;\n\t\textern void nw_wifi_cpr_report(u32 mv);\n\t\tnw_wifi_cpr_mv = resp.voltage_mv;\n\t\tnw_wifi_cpr_report(resp.voltage_mv);\n'
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
# icnss2 tells the firmware where the host's DMA memory is: the host capability's ddr_range (TLV 0x1F, three {start, size}; icnss2:
# the WLAN IOVA window, which also holds the MSI doorbell it maps, then the MSA region). ath11k sends none; with ranges the firmware
# rejects chip writes outside them (ccpm_pcie.c invalid PCIe write access). The board fills nw_wifi_ddr_range (its DMA window as the chip
# addresses it, the MSA region, the MSI doorbell's page) before the module starts; the copy of qmi.c adds the TLV when the first range is set. NW_DDR_RANGE=0 leaves it out.
if [ "${NW_DDR_RANGE:-1}" != 0 ]; then
python3 - "$O/qmi.c" <<'PY'
import sys
path = sys.argv[1]
s = open(path).read()
fn = 'int ath11k_qmi_host_cap_send(struct ath11k_base *ab)\n{\n'
assert s.count(fn) == 1, "qmi.c host cap mark"
decl = '/* clang stack usage explodes if this is inlined */\nstatic noinline_for_stack\n' + fn
assert s.count(decl) == 1, "qmi.c host cap declaration mark"
helper = """/* Ath11kWifi: the host capability with icnss2's ddr_range (cnss_utils wlfw_host_cap_req_msg_v01, TLV 0x1F). */
extern u64 nw_wifi_ddr_range[6];
struct nw_ddr_range { u64 start; u64 size; };
struct nw_host_cap_req {
\tstruct qmi_wlanfw_host_cap_req_msg_v01 base;
\tu8 ddr_range_valid;
\tstruct nw_ddr_range ddr_range[3];
};
static const struct qmi_elem_info nw_ddr_range_ei[] = {
\t{ .data_type = QMI_UNSIGNED_8_BYTE, .elem_len = 1, .elem_size = sizeof(u64), .array_type = NO_ARRAY, .tlv_type = 0,
\t  .offset = offsetof(struct nw_ddr_range, start) },
\t{ .data_type = QMI_UNSIGNED_8_BYTE, .elem_len = 1, .elem_size = sizeof(u64), .array_type = NO_ARRAY, .tlv_type = 0,
\t  .offset = offsetof(struct nw_ddr_range, size) },
\t{ .data_type = QMI_EOTI, .array_type = NO_ARRAY, .tlv_type = QMI_COMMON_TLV_TYPE },
};
static struct qmi_elem_info nw_host_cap_ei_buf[64];
static const struct qmi_elem_info *nw_host_cap_ei(void)
{
\tint n = 0;

\tif (nw_host_cap_ei_buf[0].data_type)
\t\treturn nw_host_cap_ei_buf;
\twhile (qmi_wlanfw_host_cap_req_msg_v01_ei[n].data_type != QMI_EOTI)
\t\tn++;
\tmemcpy(nw_host_cap_ei_buf, qmi_wlanfw_host_cap_req_msg_v01_ei, n * sizeof(nw_host_cap_ei_buf[0]));
\tnw_host_cap_ei_buf[n] = (struct qmi_elem_info){ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(u8),
\t\t.array_type = NO_ARRAY, .tlv_type = 0x1F, .offset = offsetof(struct nw_host_cap_req, ddr_range_valid) };
\tnw_host_cap_ei_buf[n + 1] = (struct qmi_elem_info){ .data_type = QMI_STRUCT, .elem_len = 3,
\t\t.elem_size = sizeof(struct nw_ddr_range), .array_type = STATIC_ARRAY, .tlv_type = 0x1F,
\t\t.offset = offsetof(struct nw_host_cap_req, ddr_range), .ei_array = nw_ddr_range_ei };
\tnw_host_cap_ei_buf[n + 2] = (struct qmi_elem_info){ .data_type = QMI_EOTI, .array_type = NO_ARRAY,
\t\t.tlv_type = QMI_COMMON_TLV_TYPE };
\treturn nw_host_cap_ei_buf;
}

"""
s = s.replace(decl, helper + decl, 1)
start = s.index(fn)
end = s.index('\n}\n', start)
body = s[start:end]
for a, b in (('\tstruct qmi_wlanfw_host_cap_req_msg_v01 req;\n',
              '\tstruct nw_host_cap_req nw_req = {};\n#define req nw_req.base\n'),
             ('\tret = qmi_txn_init(&ab->qmi.handle, &txn,\n\t\t\t   qmi_wlanfw_host_cap_resp_msg_v01_ei, &resp);\n',
              '\tif (nw_wifi_ddr_range[1]) {\n\t\tint i;\n\n\t\tnw_req.ddr_range_valid = 1;\n\t\tfor (i = 0; i < 3; i++) {\n'
              '\t\t\tnw_req.ddr_range[i].start = nw_wifi_ddr_range[2 * i];\n\t\t\tnw_req.ddr_range[i].size = nw_wifi_ddr_range[2 * i + 1];\n\t\t}\n'
              '\t\tath11k_info(ab, "host cap ddr range %llx+%llx, %llx+%llx, %llx+%llx\\n", nw_wifi_ddr_range[0],\n'
              '\t\t\t    nw_wifi_ddr_range[1], nw_wifi_ddr_range[2], nw_wifi_ddr_range[3], nw_wifi_ddr_range[4],\n'
              '\t\t\t    nw_wifi_ddr_range[5]);\n\t}\n\n'
              '\tret = qmi_txn_init(&ab->qmi.handle, &txn,\n\t\t\t   qmi_wlanfw_host_cap_resp_msg_v01_ei, &resp);\n'),
             ('\t\t\t       QMI_WLANFW_HOST_CAP_REQ_MSG_V01_MAX_LEN,\n\t\t\t       qmi_wlanfw_host_cap_req_msg_v01_ei, &req);\n',
              '\t\t\t       QMI_WLANFW_HOST_CAP_REQ_MSG_V01_MAX_LEN + 3 + 3 * 16,\n\t\t\t       nw_host_cap_ei(), &nw_req);\n')):
    assert body.count(a) == 1, a
    body = body.replace(a, b, 1)
body += '\n#undef req'
s = s[:start] + body + s[end:]
open(path, 'w').write(s)
PY
grep -q 'nw_host_cap_ei(), &nw_req' $O/qmi.c || { echo "qmi.c host cap patch missed" >&2; exit 1; }
fi
grep -q 'nw_wifi_cpr_mv = resp.voltage_mv' $O/qmi.c || { echo "qmi.c patch missed" >&2; exit 1; }
# The vendor host follows the SRING_SETUPs with an HTT RX_RING_SELECTION_CFG for every per-MAC data ring (qca-wifi-host-cmn li/dp_li.c
# dp_rxdma_ring_sel_cfg_li): which frames (FP only: control BA request, unicast and multicast data) and TLVs go to the ring, the buffer
# size and where each TLV sits in a buffer (offsets valid). ath11k sends it for that ring only in monitor mode. With the WCN6750 vendor
# firmware the host refill ring was never consumed; the copy of dp_rx.c sends the vendor's message (30 words, fw-api
# htt_rx_ring_selection_cfg_t) after each rx_mac_buf ring's setup. NW_RX_RING_SEL=0 leaves it out.
rm -f $O/dp_rx.c
NW_RX_RING_SEL=${NW_RX_RING_SEL:-1} python3 - "$A/dp_rx.c" "$O/dp_rx.c" <<'PY'
import os, sys
src, dst = sys.argv[1], sys.argv[2]
s = open(src).read()
if os.environ["NW_RX_RING_SEL"] != "0":
    fn = 'int ath11k_dp_rx_pdev_alloc(struct ath11k_base *ab, int mac_id)\n'
    assert s.count(fn) == 1, "dp_rx.c pdev alloc mark"
    helper = """/* Ath11kWifi: the vendor host's RX_RING_SELECTION_CFG for a per-MAC data ring (see tools/prepare-ath11k.sh). */
#define NW_RX_SEL_WORDS 30
static int nw_htt_rx_ring_sel(struct ath11k_base *ab, u32 ring_id, int mac_id)
{
\tstruct hal_srng_params params = {};
\tstruct sk_buff *skb;
\tu32 *w;
\tint ret;

\tskb = ath11k_htc_alloc_skb(ab, NW_RX_SEL_WORDS * 4);
\tif (!skb)
\t\treturn -ENOMEM;
\tath11k_hal_srng_get_params(ab, &ab->hal.srng_list[ring_id], &params);
\tskb_put(skb, NW_RX_SEL_WORDS * 4);
\tw = (u32 *)skb->data;
\tmemset(w, 0, NW_RX_SEL_WORDS * 4);
\tw[0] = FIELD_PREP(HTT_RX_RING_SELECTION_CFG_CMD_INFO0_MSG_TYPE, HTT_H2T_MSG_TYPE_RX_RING_SELECTION_CFG) |
\t       FIELD_PREP(HTT_RX_RING_SELECTION_CFG_CMD_INFO0_PDEV_ID, DP_SW2HW_MACID(mac_id)) |
\t       FIELD_PREP(HTT_RX_RING_SELECTION_CFG_CMD_INFO0_RING_ID, HTT_RXDMA_HOST_BUF_RING) |
\t       FIELD_PREP(HTT_RX_RING_SELECTION_CFG_CMD_INFO0_SS, !!(params.flags & HAL_SRNG_FLAGS_MSI_SWAP)) |
\t       FIELD_PREP(HTT_RX_RING_SELECTION_CFG_CMD_INFO0_PS, !!(params.flags & HAL_SRNG_FLAGS_DATA_TLV_SWAP)) |
\t       BIT(26) /* rx_offsets_valid */;
\tw[1] = FIELD_PREP(HTT_RX_RING_SELECTION_CFG_CMD_INFO1_BUF_SIZE, DP_RX_BUFFER_SIZE);
\tw[4] = BIT(24) /* FLAG2 FP CTRL 1000: block ack request */;
\tw[5] = HTT_RX_FP_DATA_PKT_FILTER_TLV_FLASG3_MCAST | HTT_RX_FP_DATA_PKT_FILTER_TLV_FLASG3_UCAST;
\tw[6] = HTT_RX_FILTER_TLV_FLAGS_MPDU_START | HTT_RX_FILTER_TLV_FLAGS_MSDU_START | HTT_RX_FILTER_TLV_FLAGS_RX_PACKET |
\t       HTT_RX_FILTER_TLV_FLAGS_MSDU_END | HTT_RX_FILTER_TLV_FLAGS_MPDU_END | HTT_RX_FILTER_TLV_FLAGS_PACKET_HEADER |
\t       HTT_RX_FILTER_TLV_FLAGS_ATTENTION;
\t/* The WCN6750 buffer layout (struct hal_rx_desc_qcn9074, the vendor's RXDMA_OPTIMIZATION rx_pkt_tlvs), byte offsets of the TLVs
\t * with their tags; the packet follows the descriptor. */
#define NW_OFF(f) ((u32)offsetof(struct hal_rx_desc_qcn9074, f))
\tw[7] = (u32)sizeof(struct hal_rx_desc_qcn9074) | NW_OFF(hdr_status_tag) << 16;
\tw[8] = NW_OFF(mpdu_end_tag) | NW_OFF(mpdu_start_tag) << 16;
\tw[9] = NW_OFF(msdu_end_tag) | NW_OFF(msdu_start_tag) << 16;
\tw[10] = NW_OFF(rx_attn_tag);
#undef NW_OFF
\tath11k_info(ab, "rx ring sel mac %d ring %u: %08x %08x %08x %08x %08x %08x %08x %08x %08x\\n", mac_id, ring_id,
\t\t    w[0], w[1], w[4], w[5], w[6], w[7], w[8], w[9], w[10]);
\tret = ath11k_htc_send(&ab->htc, ab->dp.eid, skb);
\tif (ret)
\t\tdev_kfree_skb_any(skb);
\treturn ret;
}

"""
    s = s.replace(fn, helper + fn, 1)
    old = ('\t\t\t\tath11k_warn(ab, "failed to configure rx_mac_buf_ring%d %d\\n",\n'
           '\t\t\t\t\t    i, ret);\n\t\t\t\treturn ret;\n\t\t\t}\n')
    assert s.count(old) == 1, "dp_rx.c rx_mac_buf_ring mark"
    s = s.replace(old, old + '\t\t\tret = nw_htt_rx_ring_sel(ab, ring_id, mac_id + i);\n\t\t\tif (ret)\n'
                  '\t\t\t\tath11k_warn(ab, "failed to send rx ring selection %d: %d\\n", i, ret);\n', 1)
open(dst, 'w').write(s)
PY
[ "${NW_RX_RING_SEL:-1}" = 0 ] || grep -q 'nw_htt_rx_ring_sel(ab, ring_id' $O/dp_rx.c || { echo "dp_rx.c patch missed" >&2; exit 1; }
echo "staged ath11k sources: $PWD/$O"
