#!/bin/sh
# tools/mkpatch.sh <vendor wlan dir>: writes the edits made in out/wlan (against the vendor tree) to patches/qcacld-port.patch.
set -u
here=$(cd "$(dirname "$0")/.." && pwd)
src=$(cd "$1" && pwd)
cd "$here/out" && ln -sfn "$src" vendor-wlan
diff -ruN --exclude .git vendor-wlan wlan > "$here/patches/qcacld-port.patch"
rm -f vendor-wlan
grep -c '^+++ ' "$here/patches/qcacld-port.patch" | sed 's/$/ files patched/'
