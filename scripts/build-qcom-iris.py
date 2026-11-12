#!/usr/bin/env python3
"""Build the C parts of QcomIris (the iris core, sessions and buffers, the VPU SMMU domain, the VideoToolbox paravirtualized codec
host) for the selected native kernel, with the flags of one osfmk file. The ArmSmmu4K kit itself comes from the GPU kit archive.
Prints OBJ lines for the board build."""
import argparse, hashlib, json, subprocess, sys
from pathlib import Path
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('component', type=Path)
p.add_argument('output', type=Path)
p.add_argument('--tree', type=Path, required=True)
a = p.parse_args()
component, tree, out = a.component.resolve(), a.tree.resolve(), a.output.resolve()
out.mkdir(parents=True, exist_ok=True)
subprocess.run([sys.executable, component / 'tools/fetch-sources.py'], check=True, stdout=subprocess.DEVNULL)
iris = component / 'deps/linux/drivers/media/platform/qcom/iris'
recipe = json.loads((tree / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/osfmk/DEVELOPMENT/startup.o.json').read_text().strip().rstrip(','))
sources = sorted((component / 'src').glob('*.c')) + [iris / 'iris_vpu_buffer.c']
includes = [component / 'include', component / 'src', component / 'shim', iris, tree / 'kits/ArmSmmu4K', tree / '.cache/xnu-work/osfmk']
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
    # -Os: the kernel's native text has little room left below the collection data.
    cmd += ['-Os', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-mgeneral-regs-only'] + ['-I' + str(d) for d in includes]
    (out / (source.stem + '.command.json')).write_text(json.dumps(cmd, indent=2) + '\n')
    result = subprocess.run(cmd, cwd=recipe['directory'], text=True, capture_output=True)
    (out / (source.stem + '.log')).write_text(result.stdout + result.stderr)
    if result.returncode:
        raise SystemExit(f'{source}:\n{result.stderr[-4000:]}')
    objects.append(obj)
(out / 'report.json').write_text(json.dumps(dict(built=True, hardware_verified=False,
    source={str(s): hashlib.sha256(s.read_bytes()).hexdigest() for s in sources},
    objects={o.name: hashlib.sha256(o.read_bytes()).hexdigest() for o in objects}), indent=2) + '\n')
for obj in objects:
    print('OBJ ' + str(obj))
