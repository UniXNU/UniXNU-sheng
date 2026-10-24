#!/usr/bin/env python3
"""Compile the WiFi module (Ath12kWifi) to kernel objects for the board build. Usage: build-ath12k-wifi.py <module dir> <output dir>
Prints one line "OBJ <path>" per object. The module lays out its generated include trees with tools/prepare.sh first."""
import subprocess, sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

module = Path(sys.argv[1]).resolve()
out = Path(sys.argv[2]).resolve()
out.mkdir(parents=True, exist_ok=True)
subprocess.run(['bash', str(module / 'tools/prepare.sh')], check=True, stdout=subprocess.DEVNULL)
clang = subprocess.run(['xcrun', '-f', 'clang'], check=True, capture_output=True, text=True).stdout.strip()
resource = subprocess.run([clang, '-print-resource-dir'], check=True, capture_output=True, text=True).stdout.strip() + '/include'
flags = ['-target', 'arm64-apple-ios17.0', '-mkernel', '-O2', '-g0', '-ffreestanding', '-fno-builtin', '-fno-common', '-fno-strict-aliasing', '-fwrapv',
         '-fno-stack-protector', '-Wno-incompatible-pointer-types', '-Wno-error', '-fno-delete-null-pointer-checks', '-mgeneral-regs-only', '-momit-leaf-frame-pointer', '-std=gnu11', '-w',
         '-DCONFIG_ATH12K_DEBUG', '-nostdinc', '-isystem', str(module / 'shim/libc'), '-isystem', resource, '-include', 'lx_pci.h',
         '-I', str(module / 'shim/include'), '-I', str(module / 'include'), '-I', str(module / 'out/pinned'), '-I', str(module / 'src')]
trees = {'ath12k': ['-I', str(module / 'out/ath12k-src'), '-I', str(module / 'out/ath12k-src/wifi7')],
         'mhi': ['-I', str(module / 'out/mhi-src')]}

jobs = []
for line in (module / 'tools/sources.txt').read_text().splitlines():
    line = line.strip()
    if not line or line.startswith('#'):
        continue
    tree, _, name = line.partition(':') if ':' in line else ('', '', line)
    if tree == 'ath12k':
        src = module / 'out/ath12k-src' / name
    elif tree == 'mhi':
        src = module / 'out/mhi-src' / name
    else:
        src = module / name
    obj = out / (('w7_' if '/wifi7/' in str(src) else tree + '_' if tree else '') + src.stem + '.o')
    extra = trees.get(tree, trees['ath12k'] if name.startswith('src/') else [])
    jobs.append((src, obj, extra))

def build(job):
    src, obj, extra = job
    r = subprocess.run([clang, '-c', *flags, *extra, str(src), '-o', str(obj)], capture_output=True, text=True)
    if r.returncode:
        sys.stderr.write(r.stderr)
        raise SystemExit(f'compile failed: {src}')
    return obj

with ThreadPoolExecutor(8) as pool:
    list(pool.map(build, jobs))

# The module's global names (sock_release, kernel_sendmsg, ...) are common in a kernel: give every one the prefix nwk_ except the entry points the board uses.
keep = {'_nw_wifi_start', '_nw_wifi_step', '_nw_wifi_stop', '_nw_wifi_stats', '_nw_wifi_set_events', '_nw_wifi_scan', '_nw_wifi_bss_count',
        '_nw_wifi_bss_get', '_nw_wifi_connect', '_nw_wifi_disconnect', '_nw_wifi_tx', '_nw_wifi_status_get', '_nw_wifi_where',
        '_nw_wifi_tx_reasons'}
objs = [o for _, o, _ in jobs]
names = set()
llvm = Path(subprocess.run(['brew', '--prefix', 'llvm'], check=True, capture_output=True, text=True).stdout.strip()) / 'bin'
for o in objs:
    r = subprocess.run([str(llvm / 'llvm-nm'), '-g', '--defined-only', str(o)], check=True, capture_output=True, text=True)
    for line in r.stdout.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] not in keep:
            names.add(parts[2])
mapping = out / 'rename.txt'
mapping.write_text(''.join(f'{n} _nwk{n}\n' for n in sorted(names)))
for o in objs:
    subprocess.run([str(llvm / 'llvm-objcopy'), f'--redefine-syms={mapping}', str(o)], check=True)
for o in objs:
    print(f'OBJ {o}')
