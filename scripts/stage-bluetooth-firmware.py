#!/usr/bin/env python3
"""Stage owner-supplied WCN6750 Bluetooth firmware under the kernel's add-on prefix (optional).

From UniXNU-sheng scripts/stage-bluetooth-firmware.py. The 3a's kernel reads msbtfwNN.mbn/.tlv and msnvNN.bin from the phone's own
bluetooth partition (package/BluetoothFirmware.c), so nothing is required here and nothing is fetched; copies in --source (for
instance from /vendor/bt_firmware/image of the stock system) are staged under firmware/qca and used when the partition has none."""
import argparse
import hashlib
import json
from pathlib import Path
import re

root = Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('output', type=Path)
p.add_argument('--source', type=Path, default=root/'.cache/firmware/qca')
a = p.parse_args()
prefix = re.search(r'^#define SHENG_BT_FIRMWARE_PREFIX "([^"]+)"$',
    (root/'package/BluetoothFirmware.h').read_text(), re.M).group(1)
names = sorted(path.name for path in a.source.iterdir()
               if re.fullmatch(r'msbtfw[0-9a-f]{2}\.(mbn|tlv)|msnv[0-9a-f]{2}\.bin', path.name)) if a.source.is_dir() else []
files = {}
destination = a.output.resolve()/prefix
for name in names:
    data = (a.source/name).read_bytes()
    destination.mkdir(parents=True, exist_ok=True)
    (destination/name).write_bytes(data)
    files[name] = dict(bytes=len(data), sha256=hashlib.sha256(data).hexdigest())
report = dict(staged=bool(files), firmware_prefix=prefix, files=files, source=str(a.source), hardware_verified=False)
a.output.mkdir(parents=True, exist_ok=True)
(a.output/'bluetooth-firmware.json').write_text(json.dumps(report, indent=2)+'\n')
print(f'Bluetooth firmware: {len(files)} file(s) staged' + (f' in {destination}' if files else ' (the kernel reads the bluetooth partition)'))
