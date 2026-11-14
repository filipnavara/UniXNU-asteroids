#!/usr/bin/env python3
"""Build QcaBluetooth (for the WCN6750) and the 3a's UART clock for the selected native kernel.

From UniXNU-sheng scripts/build-qca-bluetooth.py: the component's core, its Linux boundary, GENI SE DMA, the UART.SK front end and
Linux's btqca.c (staged by the component's prepare-linux.py; QBT_LINUX_TREE names a local Linux v6.18.60 tree), compiled with the
flags of the kernel's kern_exec.c (BSD: the front end uses sysctl and kernel control), plus package/AsteroidsGeniUart.c. Prints
OBJ <path> per object."""
import argparse, hashlib, json, os, subprocess, sys
from pathlib import Path

root = Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('component', type=Path)
p.add_argument('output', type=Path)
p.add_argument('--tree', type=Path, required=True, help='the UniXNU checkout the kernel was built in')
p.add_argument('--package', type=Path, default=root / 'package', help="the board's package directory (the tree's kits/asteroids)")
a = p.parse_args()
component, tree, out = a.component.resolve(), a.tree.resolve(), a.output.resolve()
out.mkdir(parents=True, exist_ok=True)
subprocess.run([sys.executable, component / 'tools/prepare-linux.py'], check=True, stdout=subprocess.DEVNULL)
recipe = json.loads((tree / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/bsd/DEVELOPMENT/kern_exec.o.json').read_text().strip().rstrip(','))
package = a.package.resolve()
sources = [component / f for f in ('src/qbt_core.c', 'src/qbt_linux.c', 'src/qbt_geni.c', 'src/qbt_transport.c', 'out/linux/btqca.c')]
sources.append(package / 'AsteroidsGeniUart.c')
objects = []
for source in sources:
    obj = out / (source.stem + '.o')
    cmd, args, i = [], recipe['arguments'], 0
    while i < len(args):
        value = args[i]
        if value == recipe['file']:
            cmd.append(str(source))
        elif value in ('-o', '-MF', '-MT', '-MJ'):
            if value == '-o':
                cmd += ['-o', str(obj)]
            i += 1
        elif value == '-Xclang' and args[i + 1].startswith('-W'):
            i += 1
        elif not value.startswith('-W'):
            cmd.append(value)
        i += 1
    cmd += ['-mgeneral-regs-only', '-DQBT_SOC_TYPE=QCA_WCN6750', '-Wno-sign-compare',
            *['-I' + str(d) for d in (component / 'include', component / 'src', component / 'shim', component / 'out/linux', package)]]
    (out / (source.stem + '.command.json')).write_text(json.dumps(cmd, indent=2) + '\n')
    result = subprocess.run(cmd, cwd=recipe['directory'], text=True, capture_output=True)
    (out / (source.stem + '.log')).write_text(result.stdout + result.stderr)
    if result.returncode:
        raise SystemExit(f'{source}:\n' + result.stdout + result.stderr)
    objects.append(obj)
(out / 'report.json').write_text(json.dumps(dict(built=True, hardware_verified=False, soc_type='QCA_WCN6750',
    source={str(s): hashlib.sha256(s.read_bytes()).hexdigest() for s in sources},
    objects={o.name: hashlib.sha256(o.read_bytes()).hexdigest() for o in objects}), indent=2) + '\n')
for obj in objects:
    print('OBJ ' + str(obj))
