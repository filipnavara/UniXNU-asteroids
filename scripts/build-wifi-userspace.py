#!/usr/bin/env python3
"""Stage the native supplicant and its launchd job for the Data add-on (asteroids, iOS 27).

sheng's build-wifi-userspace.py (UniXNU-sheng 516a780) for drivers/Ath11kWifi: the supplicant is the module's wpa_supplicant 2.12 with
the -Dsheng backend (the module's native control socket, org.unixnu.wifi.control), signed ad hoc with the no-container entitlement;
build-boot-runtime.py merges its job into the OS launchd cache and adds its trust hash. The WCN6750 needs no firmware files here (the
WPSS image and board data are read from the firmware partition in place), so nothing else is staged, except for unattended testing:
UNIXNU_WIFI_JOIN names a local file (the network name on the first line, its key on the second) that goes to wifi-test/join.txt, which
the board joins with wifi-aj=1. Never set for a release: the file holds a network key.

    build-wifi-userspace.py OUTPUT ios27
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import plistlib
import re
import shutil
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
parser.add_argument("output", type=Path)
parser.add_argument("target", choices=("ios27",))
args = parser.parse_args()
out = args.output.resolve()
out.mkdir(parents=True, exist_ok=True)
(out / "report.json").unlink(missing_ok=True)
component = root / "drivers/Ath11kWifi"
for name in ("bin", "etc/wifi", "daemons", "firmware"):
    shutil.rmtree(out / name, ignore_errors=True)
    (out / name).mkdir(parents=True)
join = os.environ.get("UNIXNU_WIFI_JOIN")
if join:
    (out / "firmware/join.txt").write_bytes(Path(join).read_bytes())
subprocess.run([sys.executable, component / "tools/build-supplicant.py", "--target", "ios"], check=True)
binary = out / "bin/unixnu-wpa_supplicant"
shutil.copy2(component / "out/wpa-ios/bin/wpa_supplicant", binary)
binary.chmod(0o755)
entitlements = out / "entitlements.plist"
# iOS puts a daemon without this entitlement in a container.
entitlements.write_bytes(plistlib.dumps({"com.apple.private.security.no-container": True}))
subprocess.run(["codesign", "--force", "--sign", "-", "--pagesize", "4096", "--identifier", "org.unixnu.wifi.supplicant",
                "--entitlements", entitlements, binary], check=True)
subprocess.run(["codesign", "--verify", "--strict", binary], check=True)
detail = subprocess.run(["codesign", "--display", "--verbose=4", binary], check=True, capture_output=True, text=True).stderr
match = re.search(r"^CDHash=([0-9a-f]{40})$", detail, re.M)
if not match:
    raise SystemExit("signed supplicant has no CDHash")
(out / "etc/wifi/wpa_supplicant.conf").write_text("# Networks arrive from the native Wi-Fi control channel.\nap_scan=1\n")
# /usr/libexec/unixnu-wpa_supplicant: the add-on's wifi-bin/ shown over /usr/libexec (patches/xnu-12377/zzzzzzzzzc). -s: warnings and
# errors go to syslog (logd keeps them bounded); no log file grows on Data.
job = {"Label": "org.unixnu.wifi.supplicant", "ProgramArguments": [
    "/usr/libexec/unixnu-wpa_supplicant", "-Dsheng", "-ien0", "-c/private/var/ngpu/etc/wifi/wpa_supplicant.conf", "-q", "-s"],
    "RunAtLoad": True, "KeepAlive": True, "ThrottleInterval": 3, "UserName": "root", "Umask": 0o77, "ProcessType": "Interactive"}
(out / "daemons/org.unixnu.wifi.supplicant.plist").write_bytes(plistlib.dumps(job))
files = [path for name in ("bin", "etc/wifi", "daemons") for path in sorted((out / name).rglob("*")) if path.is_file()]
(out / "report.json").write_text(json.dumps({
    "built": True, "target": args.target, "cdhash": match[1], "hardware_verified": False,
    # The runtime adds the firmware/ directory under this prefix; only the join test's file is ever there.
    "firmware_prefix": "wifi-test" if join else None,
    "daemons_prefix": "daemons",
    "join_test": bool(join),
    "files": {str(path.relative_to(out)): hashlib.sha256(path.read_bytes()).hexdigest() for path in files},
    "supplicant": json.loads((component / "out/wpa-ios/report.json").read_text())}, indent=2) + "\n")
print(f"staged signed native Wi-Fi service: {out}; CDHash {match[1]}" + ("; with the join test's network" if join else ""))
