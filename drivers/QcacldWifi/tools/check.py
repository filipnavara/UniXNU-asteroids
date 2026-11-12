#!/usr/bin/env python3
"""tools/check.py <vendor wlan dir> deps|compile|objs|one [file-filter]: builds qcacld-3.0 (out/objs.txt, out/cflags.txt from
tools/objlist.sh) against the QcacldWifi shim (shim/include) and the Ath11kWifi runtime shim, and reports.
deps: headers no include path provides; compile: syntax errors grouped by kind and name; objs: objects into out/objs/.
tools/check.py <vendor wlan dir> board <out dir> <target triple>: the module's objects for the board build (-O2, the kernel's
target), printed as "OBJ <path>"; nonzero exit on any compile error."""
import collections, json, re, subprocess, sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

here = Path(__file__).resolve().parent.parent
wlan = Path(sys.argv[1]).resolve()  # out/wlan after tools/prepare.sh (the patched copy)
mode = sys.argv[2]
only = sys.argv[3] if len(sys.argv) > 3 and mode != 'board' else ''
board_out = Path(sys.argv[3]).resolve() if mode == 'board' else None
target = sys.argv[4] if mode == 'board' else 'arm64e-apple-ios27.0'
W = wlan / 'qcacld-3.0'
A = here.parent / 'Ath11kWifi'
clang = subprocess.run(['xcrun', '-f', 'clang'], check=True, capture_output=True, text=True).stdout.strip()
resource = subprocess.run([clang, '-print-resource-dir'], check=True, capture_output=True, text=True).stdout.strip() + '/include'
base = ['-target', target, '-mkernel', *(['-O2', '-g0', '-fno-delete-null-pointer-checks', '-momit-leaf-frame-pointer'] if mode == 'board' else ['-O1']), '-ffreestanding', '-fno-builtin', '-fno-common', '-fno-strict-aliasing',
        '-fwrapv', '-fno-stack-protector', '-mgeneral-regs-only', '-std=gnu11', '-w',
        '-D__linux__', '-DLINUX', '-D__KERNEL__', '-DMODULE', '-DKBUILD_MODNAME="qca_cld3_qca6750"', '-nostdinc', '-isystem', str(A / 'shim/libc'), '-isystem', resource,
        '-include', 'qcacld_prelude.h', '-include', 'lx_pci.h', '-include', 'qcacld_kernel.h', '-include', 'qcacld_net.h',
        '-I', str(here / 'shim/include'), '-I', str(A / 'shim/include'), '-I', str(A / 'include'), '-I', str(A / 'out/pinned'),
        '-I', str(A / 'out'), '-I', str(A / 'out/cfg80211'), '-I', str(W / 'uapi/linux'), '-I', str(here / 'out/platform-inc')]
vendor, raw, i = [], (here / 'out/cflags.txt').read_text().split('\n'), 0
while i < len(raw):
    f = raw[i].strip()
    if f == '-include' and i + 1 < len(raw):
        vendor += ['-include', str((W / raw[i + 1].strip()).resolve())]; i += 2; continue
    if f.startswith('-I'):
        p = f[2:]; vendor.append('-I' + (str((W / p).resolve()) if not p.startswith('/') else p))
    elif f.startswith('-D'):
        if f.count('"') == 1:  # a define split over lines (BUILD_TAG): keep it as one string
            while i + 1 < len(raw) and '"' not in raw[i + 1]: i += 1
            i += 1; f = f.split('"')[0] + '"qcacld"'
        vendor.append(f)
    i += 1
# the platform headers under the names a kernel build installs them as
pi = here / 'out/platform-inc'
for name, target in (('net/cnss2.h', 'cnss2.h'), ('net/cnss_utils.h', 'cnss_utils.h'), ('net/cnss_prealloc.h', 'cnss_prealloc.h'),
                     ('net/cnss_nl.h', 'cnss_nl.h'), ('soc/qcom/icnss2.h', 'icnss2.h')):
    link = pi / name; link.parent.mkdir(parents=True, exist_ok=True)
    if not link.exists(): link.symlink_to(wlan / 'platform/inc' / target)
srcs = [o[:-2] + '.c' for o in (here / 'out/objs.txt').read_text().split() if o and (W / (o[:-2] + '.c')).exists()]
# The platform side: vendor platform sources (config/platform-sources.txt, relative to the wlan dir) and the port's own src/*.c,
# built like the vendor's platform modules (their include dirs, not qcacld's flags).
plat_src = [str(wlan / l.strip()) for l in (here / 'config/platform-sources.txt').read_text().splitlines()
            if l.strip() and not l.startswith('#')] + [str(f) for f in sorted((here / 'src').glob('*.c'))] + \
           [str(f) for f in sorted((here / 'out/gen').glob('*.c'))]   # build-qcacld-wifi.py: embedded files
plat_flags = ['-DCONFIG_ICNSS2_QMI', '-DCONFIG_CNSS_OUT_OF_TREE', '-I', str(here / 'src'), '-I', str(A / 'src'), '-I', str(wlan / 'platform/inc'),
              '-I', str(wlan / 'platform/cnss_utils'), '-I', str(wlan / 'platform/icnss2')]
srcs += plat_src
srcs = list(dict.fromkeys(srcs))   # the vendor Kbuild lists some objects twice
srcs = [s for s in srcs if only in s]
def flags_for(src):
    return plat_flags if src in plat_src else vendor

if mode == 'one':
    r = subprocess.run([clang, '-fsyntax-only', '-ferror-limit=60', *base, *flags_for(srcs[0]), str(W / srcs[0])], capture_output=True, text=True, cwd=W)
    for l in r.stderr.splitlines():
        if 'error' in l: print(l.replace(str(wlan) + '/', '')[:240])
    raise SystemExit

def obj_path(src):
    name = ('port__' + Path(src).name) if src.startswith((str(here / 'src') + '/', str(here / 'out/gen') + '/')) else src.replace(str(wlan) + '/', '').replace('../', '').replace('/', '__')
    return (board_out if mode == 'board' else here / 'out/objs') / (name[:-2] + '.o')

def run(src):
    if mode == 'deps':
        r = subprocess.run([clang, '-M', '-MG', *base, *flags_for(src), str(W / src)], capture_output=True, text=True, cwd=W)
        files = r.stdout.replace('\\\n', ' ').split(':', 1)[-1].split()
        return src, [f for f in files if not f.startswith('/') and not (W / f).exists()]
    if mode in ('objs', 'board'):
        o = obj_path(src); o.parent.mkdir(parents=True, exist_ok=True)
        r = subprocess.run([clang, '-c', *base, *flags_for(src), str(W / src), '-o', str(o)], capture_output=True, text=True, cwd=W)
    else:
        r = subprocess.run([clang, '-fsyntax-only', '-ferror-limit=0', *base, *flags_for(src), str(W / src)], capture_output=True, text=True, cwd=W)
    return src, [l for l in r.stderr.splitlines() if 'error:' in l]

with ThreadPoolExecutor(12) as ex:
    res = list(ex.map(run, srcs))
if mode == 'board':
    failed = [(s, e) for s, e in res if e]
    for s, e in failed: sys.stderr.write(f'{s}:\n' + '\n'.join(e[:20]) + '\n')
    if failed: raise SystemExit(f'{len(failed)} sources failed to compile')
    for s, _ in res: print(f'OBJ {obj_path(s)}')
    raise SystemExit
if mode == 'deps':
    count = collections.Counter(m for _, ms in res for m in set(ms))
    print(len(count), 'missing headers over', len(srcs), 'sources')
    for h, n in count.most_common(): print(f'{n:4d} {h}')
    raise SystemExit
kinds, names, per = collections.Counter(), collections.Counter(), {}
for src, errs in res:
    per[src] = len(errs)
    seen = set()
    for e in errs:
        m = re.search(r'error: (.*)', e); msg = m[1] if m else e
        kinds[re.sub(r"'[^']*'", "'X'", msg)] += 1
        for n in re.findall(r"'([A-Za-z_][A-Za-z0-9_]*)'", msg):
            if (n, msg) not in seen: names[n] += 1; seen.add((n, msg))
(here / 'out/check.json').write_text(json.dumps({'kinds': kinds.most_common(), 'names': names.most_common(), 'per': per}, indent=0))
print(f"{sum(1 for v in per.values() if v == 0)} of {len(per)} sources clean; {sum(per.values())} errors; {len(names)} distinct names")
for k, n in kinds.most_common(12): print(f'{n:7d} {k}')
