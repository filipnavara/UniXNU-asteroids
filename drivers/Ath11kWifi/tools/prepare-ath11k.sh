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
open(dst, 'w').write(s)
PY
grep -q 'bdwlan.b%02x' $O/core.c || { echo "core.c patch missed" >&2; exit 1; }
# The board gives IMPS back (nw_wifi_idle_ps; wifi-idle-ps, default 1) now that the SoC wake handshake and the sleep clock are in
# place: the firmware sleeps between scans, as under the vendor host (on since wifi78; wifi-idle-ps=0 keeps it off).
python3 - "$O/core.c" <<'PY'
import sys
path = sys.argv[1]
s = open(path).read()
old = '\tab->hw_params = *hw_params;\n'
assert s.count(old) == 1, "core.c hw_params copy mark"
s = s.replace(old, old + '\t{\n\t\textern unsigned nw_wifi_idle_ps;\n\n\t\tif (nw_wifi_idle_ps)\n\t\t\tab->hw_params.idle_ps = true;\n\t}\n', 1)
open(path, 'w').write(s)
PY
grep -q 'nw_wifi_idle_ps' $O/core.c || { echo "core.c idle ps patch missed" >&2; exit 1; }
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
open(dst, 'w').write(s)
PY
grep -q 'nw_wifi_cpr_mv = resp.voltage_mv' $O/qmi.c || { echo "qmi.c patch missed" >&2; exit 1; }
# The vendor host follows the SRING_SETUPs with an HTT RX_RING_SELECTION_CFG for every per-MAC data ring (qca-wifi-host-cmn li/dp_li.c
# dp_rxdma_ring_sel_cfg_li): which frames (FP only: control BA request, unicast and multicast data) and TLVs go to the ring, the buffer
# size and where each TLV sits in a buffer (offsets valid). ath11k sends it for that ring only in monitor mode. With the WCN6750 vendor
# firmware the host refill ring was never consumed; the copy of dp_rx.c sends the vendor's message (30 words, fw-api
# htt_rx_ring_selection_cfg_t) after each rx_mac_buf ring's setup.
rm -f $O/dp_rx.c
python3 - "$A/dp_rx.c" "$O/dp_rx.c" <<'PY'
import sys
src, dst = sys.argv[1], sys.argv[2]
s = open(src).read()
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
grep -q 'nw_htt_rx_ring_sel(ab, ring_id' $O/dp_rx.c || { echo "dp_rx.c patch missed" >&2; exit 1; }
# The firmware's HTT messages ath11k does not handle, with their first words.
python3 - "$O/dp_rx.c" <<'PY'
import sys
path = sys.argv[1]
s = open(path).read()
old = '\t\tath11k_warn(ab, "htt event %d not handled\\n", type);\n'
assert s.count(old) == 1, "dp_rx.c htt default mark"
s = s.replace(old, '\t\tath11k_warn(ab, "htt event %d not handled: %08x %08x %08x %08x (%u bytes)\\n", type, resp->version_msg.version,\n'
              '\t\t\t    skb->len > 4 ? ((u32 *)skb->data)[1] : 0, skb->len > 8 ? ((u32 *)skb->data)[2] : 0,\n'
              '\t\t\t    skb->len > 12 ? ((u32 *)skb->data)[3] : 0, skb->len);\n', 1)
open(path, 'w').write(s)
PY
grep -q 'not handled: %08x' $O/dp_rx.c || { echo "dp_rx.c htt default patch missed" >&2; exit 1; }
# The IV of decrypted group frames: ath11k keeps it (mac80211 checks the PN) by copying it out of the 802.11 header the chip stores in
# the receive descriptor, but the WCN6750 stores no IV there (all zero: key id 0, so mac80211 found no group key and dropped every
# broadcast/multicast frame as RX_DROP_U_UNPROTECTED, wifi86). When the copy is all zero the IV is rebuilt from the descriptor's
# mpdu_start (PN words and key id, QCN9074 layout as the WCN6750 uses), as ath12k builds it (ath12k_hal_rx_desc_get_crypto_hdr_qcn9274).
python3 - "$O/dp_rx.c" <<'PY'
import sys
path = sys.argv[1]
s = open(path).read()
old = """		memcpy(skb_push(msdu,
				ath11k_dp_rx_crypto_param_len(ar, enctype)),
		       (void *)hdr + hdr_len,
		       ath11k_dp_rx_crypto_param_len(ar, enctype));"""
assert s.count(old) == 2, "dp_rx.c crypto param copies"
s = s.replace(old, "\t\tnw_dp_rx_put_crypto_param(ar, msdu, (u8 *)hdr + hdr_len, enctype);")
mark = 'static void ath11k_dp_rx_h_undecap_nwifi('
helper = """static void nw_dp_rx_put_crypto_param(struct ath11k *ar, struct sk_buff *msdu, const u8 *stored, enum hal_encrypt_type enctype)
{
	const int len = ath11k_dp_rx_crypto_param_len(ar, enctype);
	u8 *iv = skb_push(msdu, len);
	struct hal_rx_desc *desc = ATH11K_SKB_RXCB(msdu)->rx_desc;
	int i;

	memcpy(iv, stored, len);
	if (len != IEEE80211_CCMP_HDR_LEN || !desc || ar->ab->hw_params.hw_ops->rx_desc_get_hdr_status != wcn6750_ops.rx_desc_get_hdr_status)
		return;
	for (i = 0; i < len; i++)
		if (iv[i])
			return;
	switch (enctype) {
	case HAL_ENCRYPT_TYPE_CCMP_128:
	case HAL_ENCRYPT_TYPE_CCMP_256:
	case HAL_ENCRYPT_TYPE_GCMP_128:
	case HAL_ENCRYPT_TYPE_AES_GCMP_256: {
		const u32 pn0 = __le32_to_cpu(desc->u.qcn9074.mpdu_start.pn[0]), pn1 = __le32_to_cpu(desc->u.qcn9074.mpdu_start.pn[1]);
		const u32 key_id = FIELD_GET(RX_MPDU_START_INFO12_KEY_ID, __le32_to_cpu(desc->u.qcn9074.mpdu_start.info12));

		iv[0] = pn0; iv[1] = pn0 >> 8; iv[2] = 0; iv[3] = 0x20 | ((key_id & 3) << 6);
		iv[4] = pn0 >> 16; iv[5] = pn0 >> 24; iv[6] = pn1; iv[7] = pn1 >> 8;
		break;
	}
	default:
		break;
	}
}

"""
assert s.count(mark) == 1
s = s.replace(mark, helper + mark, 1)
open(path, 'w').write(s)
PY
grep -q 'nw_dp_rx_put_crypto_param(ar, msdu' $O/dp_rx.c || { echo "dp_rx.c group IV patch missed" >&2; exit 1; }
echo "staged ath11k sources: $PWD/$O"
