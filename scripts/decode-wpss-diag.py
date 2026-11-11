#!/usr/bin/env python3
"""Decode the WLAN firmware's diag records in a pulled boot log.

drivers/Ath11kWifi keeps the firmware's last diag records (WMI_DIAG_EVENTID) and the board logs them as
"fwd <timestamp> <type>/<id>/<payload bytes>: <args>" when the WPSS dies. The ids index the firmware image's message dictionary,
image/qca6750/Data.msc on the firmware partition ("<id>,<argument types>,[ file : line ] format"); copy it off the phone first, it is
not part of this repository.

    decode-wpss-diag.py LOGDIR Data.msc
"""
import re
import sys
from pathlib import Path

log, dictionary = Path(sys.argv[1]), Path(sys.argv[2])
messages = {}
for line in dictionary.read_text(errors="replace").splitlines():
    match = re.match(r"(\d+),([A-Za-z]*),(.*)", line)
    if match:
        messages[int(match[1])] = (match[2], match[3].strip())
seen = set()
pattern = re.compile(r"\[wlan-log\] (\d+) \d+ fwd ([0-9a-f]+) (\d+)/(\d+)/(\d+): (.*)")
rows = []
for path in sorted(log.rglob("slots.txt")):
    for line in path.read_text(errors="replace").splitlines():
        match = pattern.match(line)
        if match and line not in seen:
            seen.add(line)
            rows.append(match)
for match in rows:
    at, stamp, kind, ident, pay = int(match[1]), int(match[2], 16), int(match[3]), int(match[4]), int(match[5])
    args = [int(a, 16) for a in match[6].split()]
    when = f"{stamp:06x}"
    if kind == 7 and ident in messages and len(messages[ident][0]) * 4 == pay:
        pass  # a module that reads type 7 records itself: the header already carries the id
    elif kind == 7 and args and not args[0] & 0xff & 3 and (args[0] >> 16) in messages:
        # A type 7 record kept by a module that read it as the older kind (two header words): its second word is the time
        # (seconds << 16 | milliseconds) and the id and length word is the first "argument".
        when = f"{ident}.{pay:03d}s"
        ident, pay, args = args[0] >> 16, args[0] & 0xff, args[1:]
    elif kind == 7:
        when = f"{args[-1] >> 16}.{args[-1] & 0xffff:03d}s" if len(args) > 6 else when
        args = args[:6]
    elif ident not in messages or (len(messages[ident][0]) * 4 != pay and pay):
        continue  # not a record boundary
    types, text = messages.get(ident, ("", f"? id {ident}"))
    out, index = "", 0
    # %d %u %x %p and friends take the arguments in order; anything else is left as written
    def take(spec):
        global index
        if index >= len(args):
            return spec[0]
        value = args[index]
        index += 1
        kind_char = spec[0][-1]
        if kind_char in "di":
            return str(value - (1 << 32) if value & 0x80000000 else value)
        if kind_char == "u":
            return str(value)
        return f"{value:x}"
    out = re.sub(r"%[0-9.l]*[diuxXp]", take, text)
    extra = " | " + " ".join(f"{a:x}" for a in args[index:pay // 4]) if pay // 4 > index else ""
    print(f"{at:7d} fw {when:>10} t{kind} {out}{extra}")
if not rows:
    print("no fwd records in", log)
