#!/bin/sh
# tools/objlist.sh <vendor wlan dir>: the object list and flags of qcacld-3.0 (qca6750, volcano profile minus config/disabled.txt)
# into out/objs.txt and out/cflags.txt. <vendor wlan dir> holds qcacld-3.0, qca-wifi-host-cmn, fw-api, platform (NothingOSS kernel).
set -e
here=$(cd "$(dirname "$0")/.." && pwd)
wlan=$(cd "$1" && pwd)
off=$(grep -v '^#' "$here/config/disabled.txt" | grep . | sed 's/$/=/' | tr '\n' ' ')
mkdir -p "$here/out"
gmake -s -f "$here/tools/objlist.mk" -C "$wlan/qcacld-3.0" list OUT="$here/out" MODNAME=qca_cld3_qca6750 WLAN_ROOT=. \
	WLAN_COMMON_ROOT=../qca-wifi-host-cmn WLAN_COMMON_INC=../qca-wifi-host-cmn WLAN_FW_API=../fw-api WLAN_PLATFORM_INC=../platform/inc \
	WLAN_PROFILE=volcano_gki_qca6750 CONFIG_QCA_CLD_WLAN_PROFILE=volcano_gki_qca6750 srctree=. src=. $off 2>/dev/null
echo "$(wc -l < "$here/out/objs.txt") objects, $(wc -l < "$here/out/cflags.txt") flags"
