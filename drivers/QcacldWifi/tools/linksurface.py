#!/usr/bin/env python3
"""tools/linksurface.py <ath11k board build dir>: the link surface of the qcacld objects (out/objs from check.py objs).
Undefined symbols of all objects minus the ones the objects define = what qcacld needs from outside. Those are split into
what the Ath11kWifi runtime already defines (or, like the runtime, imports from the kernel) (lx_rt, cfg80211, nw_*, qmi, crypto objects of the board build; ath11k_* and
mac80211_* are left out: qcacld replaces them) and what is missing. Writes out/linksurface.json and prints the missing
names grouped by prefix."""
import collections, json, re, subprocess, sys
from pathlib import Path

here = Path(__file__).resolve().parent.parent
board = Path(sys.argv[1]).resolve()
nm = subprocess.run(['xcrun', '-f', 'nm'], check=True, capture_output=True, text=True).stdout.strip()


def symbols(objs):
    defined, undefined, users = set(), set(), collections.defaultdict(set)
    for o in objs:
        out = subprocess.run([nm, '-g', str(o)], capture_output=True, text=True).stdout
        for line in out.splitlines():
            parts = line.split()
            if len(parts) == 2 and parts[0] == 'U':
                name = parts[1][1:] if parts[1].startswith('_') else parts[1]
                undefined.add(name); users[name].add(o.stem)
            elif len(parts) == 3 and parts[1] not in ('U',):
                name = parts[2][1:] if parts[2].startswith('_') else parts[2]
                defined.add(name[4:] if name.startswith('nwk_') else name)  # the board build namespaces runtime symbols
    return defined, undefined, users


q_def, q_undef, users = symbols(sorted((here / 'out/objs').glob('*.o')))
runtime = [o for o in board.glob('*.o') if not re.match(r'(ath11k|mac80211)_|nw_(ahb|wifi|sta|mac80211|debug|crypto_mac80211|mainline)$', o.stem)]  # ath11k's glue is replaced too
r_def, r_undef, _ = symbols(runtime)
kernel = {n for n in r_undef if not n.startswith('nwk_')} - r_def  # the runtime itself imports these from the kernel
need = sorted(q_undef - q_def)
have = [n for n in need if n in r_def]
from_kernel = [n for n in need if n not in r_def and n in kernel]
missing = [n for n in need if n not in r_def and n not in kernel]
dup = sorted(q_def & r_def)   # defined twice: a clash when the runtime and the qcacld objects are linked together
groups = collections.defaultdict(list)
for n in missing:
    groups[re.match(r'[a-z0-9]*', n.lower())[0] or n[:3]].append(n)
(here / 'out/linksurface.json').write_text(json.dumps(
    {'needed': len(need), 'provided': have, 'kernel': from_kernel, 'duplicate': dup, 'missing': {n: sorted(users[n]) for n in missing}}, indent=1))
print(f'{len(q_def)} defined in qcacld objects; {len(need)} needed from outside: {len(have)} provided by the runtime, '
      f'{len(from_kernel)} by the kernel (as for the runtime), {len(missing)} missing; {len(dup)} defined twice'
      + (': ' + ' '.join(dup) if dup else ''))
for g, ns in sorted(groups.items(), key=lambda kv: -len(kv[1])):
    print(f'{len(ns):4d} {g}: ' + ' '.join(ns))
