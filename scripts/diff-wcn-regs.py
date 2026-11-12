#!/usr/bin/env python3
"""Diff the WCN6750 data-path registers: our port (wifi-regdump=1, a pulled log directory) against the vendor stack
(tools/nwregdump, a dmesg capture).

    diff-wcn-regs.py OUR_LOGDIR VENDOR_DMESG [--ours-slot 0|1] [--vendor-dump N] [--all]

Ours: "[wlan-log] .. nwreg sSLOT IDX: v0..v7" lines (slot 0 idle after radio on, slot 1 0.3 s into the first scan). Vendor:
"nwreg STAMP IDX: v0..v7" lines, one dump per module load (--vendor-dump picks one, default the first). Registers that hold addresses or
ring pointers (BASE_LSB/MSB, MSI addresses, HP/TP) differ between hosts by nature; they are listed apart from the configuration.
"""
import re
import sys
from pathlib import Path

args = [a for a in sys.argv[1:] if not a.startswith("--")]
opts = dict(a[2:].split("=", 1) if "=" in a else (a[2:], "1") for a in sys.argv[1:] if a.startswith("--"))
ours_dir, vendor_file = Path(args[0]), Path(args[1])
names = []
for line in (Path(__file__).resolve().parent.parent / "drivers/Ath11kWifi/tools/wcn-regs.txt").read_text().splitlines():
    off, name = line.split(" ", 1)
    names.append((int(off, 16), name))


def fill(table, idx, vals):
    for k, v in enumerate(vals):
        if idx + k < len(names):
            table[idx + k] = int(v, 16)


ours, slot = {}, opts.get("ours-slot", "0")
pat = re.compile(r"nwreg s(\d) (\d+): ((?:[0-9a-f]{8} ?){8})")
for path in sorted(ours_dir.rglob("slots.txt")):
    for line in path.read_text(errors="replace").splitlines():
        m = pat.search(line)
        if m and m[1] == slot:
            fill(ours, int(m[2]), m[3].split())
dumps = {}
pat = re.compile(r"nwreg (\d+) (\d+): ((?:[0-9a-f]{8} ?){8})")
for line in vendor_file.read_text(errors="replace").splitlines():
    m = pat.search(line)
    if m:
        fill(dumps.setdefault(m[1], {}), int(m[2]), m[3].split())
if not dumps:
    sys.exit("no vendor dump in " + str(vendor_file))
stamps = list(dumps)
vendor = dumps[stamps[int(opts.get("vendor-dump", "0"))]]
print(f"ours: slot {slot}, {len(ours)} of {len(names)} registers; vendor: dump {stamps.index(stamps[int(opts.get('vendor-dump', '0'))])} "
      f"of {len(stamps)}, {len(vendor)} registers")
addr = re.compile(r"BASE_LSB|BASE_MSB|MSI1_BASE|_ADDR|_HP$|_TP$|\+040|\+400|\+404|\+000|\+004")
config, pointers, same = [], [], 0
for i, (off, name) in enumerate(names):
    a, b = ours.get(i), vendor.get(i)
    if a is None or b is None:
        continue
    if a == b and "all" not in opts:
        same += 1
        continue
    (pointers if addr.search(name) else config).append(f"{off:07x} {name:58s} ours {a:08x}  vendor {b:08x}" +
                                                         ("" if a != b else "  (same)"))
print(f"{same} registers the same; {len(config)} configuration registers differ; {len(pointers)} address/pointer registers differ\n")
print("== configuration")
print("\n".join(config))
print("\n== addresses and ring pointers")
print("\n".join(pointers))
