#!/usr/bin/env python3
"""Turn the vendor WLAN stack's firmware log (cld80211 "fw_logs" messages captured as "M <len> <hex>" lines by the nllisten tool)
into the "fwd" record lines our board logs, so scripts/decode-wpss-diag.py decodes both the same way.

    vendor-fwlog-to-fwd.py CAPTURE OUTDIR

Each message: genl attribute 1 (CLD80211_ATTR_VENDOR_DATA) > attribute 2 (CLD80211_ATTR_DATA) = radio (u32), then qcacld's dbglog slot
(diag_type, timestamp, length, dropped = firmware version) and the WMI_DIAG buffer. That buffer is the one ath11k hands us with its
first word zeroed instead of the WMI TLV header, so the records start at the same offset 8 (drivers/Ath11kWifi/src/nw_debug.c
nw_fw_diag). Writes OUTDIR/vendor/slots.txt with "[wlan-log] N 6 fwd ..." lines (N = message number).
"""
import struct
import sys
from pathlib import Path


def attrs(b, off, end):
    while off + 4 <= end:
        ln, ty = struct.unpack_from("<HH", b, off)
        if ln < 4:
            break
        yield ty & 0x3FFF, off + 4, off + ln
        off += (ln + 3) & ~3


out = []
messages = [l.split() for l in Path(sys.argv[1]).read_text().splitlines() if l.startswith("M ")]
records = 0
for number, m in enumerate(messages):
    b = bytes.fromhex(m[2])
    for ty, s, e in attrs(b, 0, len(b)):
        if ty != 1:
            continue
        for ty2, s2, e2 in attrs(b, s, e):
            if ty2 != 2:
                continue
            radio, diag_type, stamp, length, dropped = struct.unpack_from("<5I", b, s2)
            p = b[s2 + 20: s2 + 20 + length]
            p, n = p[8:], len(p) - 8
            while n >= 8:
                w0, w1 = struct.unpack_from("<II", p, 0)
                head = 8
                if w0 >> 24 == 7:
                    if n < 12:
                        break
                    w2 = struct.unpack_from("<I", p, 8)[0]
                    w0, w1, head = 7 << 24 | (w1 & 0xFFFFFF), w2, 12
                pay = w1 & 0xFF
                padded = (pay + 3) & ~3
                if padded > n - head:
                    break
                a = list(struct.unpack_from("<%dI" % (padded // 4), p, head)) + [0] * 6
                out.append(f"[wlan-log] {number} 6 fwd {w0 & 0xFFFFFF:06x} {w0 >> 24}/{w1 >> 16}/{pay}: " + " ".join(f"{x:x}" for x in a[:6]))
                records += 1
                p, n = p[head + padded:], n - head - padded
target = Path(sys.argv[2]) / "vendor"
target.mkdir(parents=True, exist_ok=True)
(target / "slots.txt").write_text("\n".join(out) + "\n")
print(f"{len(messages)} messages, {records} records -> {target / 'slots.txt'}")
