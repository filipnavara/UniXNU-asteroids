#!/bin/sh
# tools/prepare.sh <vendor wlan dir>: copies the vendor wlan tree (qcacld-3.0, qca-wifi-host-cmn, fw-api, platform) to out/wlan
# and applies patches/*.patch (p1, relative to the wlan dir). Rerun after editing a patch: the copy is reset first.
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
src=$(cd "$1" && pwd)
mkdir -p "$here/out/wlan"
rsync -a --delete --exclude .git "$src/" "$here/out/wlan/"
for p in "$here"/patches/*.patch; do
  [ -e "$p" ] || continue
  patch -s -p1 -d "$here/out/wlan" < "$p" || { echo "patch failed: $p" >&2; exit 1; }
  echo "applied $(basename "$p")"
done
