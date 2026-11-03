#!/usr/bin/env python3
"""Compile the WiFi module (Ath12kWifi) to kernel objects for the board build. Usage: build-ath12k-wifi.py <module dir> <output dir>
Prints one line "OBJ <path>" per object. The module fetches its pinned public sources with tools/fetch-sources.sh and lays out its generated include trees with tools/prepare.sh first."""
import hashlib, json, shlex, shutil, subprocess, sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from kernel_target import TRIPLE

module = Path(sys.argv[1]).resolve()
out = Path(sys.argv[2]).resolve()
out.mkdir(parents=True, exist_ok=True)
(out / 'build-report.json').unlink(missing_ok=True)
raw = out / 'raw'
raw.mkdir(exist_ok=True)
subprocess.run(['bash', str(module / 'tools/fetch-sources.sh')], check=True, stdout=subprocess.DEVNULL)
subprocess.run(['bash', str(module / 'tools/prepare.sh')], check=True, stdout=subprocess.DEVNULL)
clang = subprocess.run(['xcrun', '-f', 'clang'], check=True, capture_output=True, text=True).stdout.strip()
resource = subprocess.run([clang, '-print-resource-dir'], check=True, capture_output=True, text=True).stdout.strip() + '/include'
flags = ['-target', TRIPLE, '-mkernel', '-O2', '-g0', '-ffreestanding', '-fno-builtin', '-fno-common', '-fno-strict-aliasing', '-fwrapv',
         '-fno-stack-protector', '-Wno-incompatible-pointer-types', '-Wno-error', '-fno-delete-null-pointer-checks', '-mgeneral-regs-only', '-momit-leaf-frame-pointer', '-std=gnu11', '-w',
         '-DCONFIG_ATH12K_DEBUG', '-DCONFIG_CFG80211=1', '-DCONFIG_MAC80211=1',
         '-DCONFIG_MAC80211_STA_HASH_MAX_SIZE=0', '-DCONFIG_MAC80211_RC_DEFAULT=""',
         '-nostdinc', '-isystem', str(module / 'shim/libc'), '-isystem', resource, '-include', 'lx_pci.h',
         '-I', str(module / 'shim/include'), '-I', str(module / 'include'), '-I', str(module / 'out/pinned'), '-I', str(module / 'src')]
flags += ['-I', str(module / 'out')]
trees = {'ath12k': ['-I', str(module / 'out/ath12k-src'), '-I', str(module / 'out/ath12k-src/wifi7')],
         'mhi': ['-I', str(module / 'out/mhi-src')], 'qmi': ['-I', str(module / 'out/os-src')]}
trees.update({name: ['-I', str(module / 'out' / name)] for name in ('mac80211', 'cfg80211')})
trees['generated'] = trees['cfg80211']
trees['crypto'] = ['-I', str(module / 'shim/crypto'), '-I', str(module / 'out/crypto')]

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
    elif tree == 'qmi':
        src = module / 'out/os-src' / name
    elif tree in ('mac80211', 'cfg80211', 'crypto'):
        src = module / 'out' / tree / name
    elif tree == 'generated':
        src = module / 'out' / name
    else:
        src = module / name
    obj = out / (('w7_' if '/wifi7/' in str(src) else tree + '_' if tree else '') + src.stem + '.o')
    extra = trees.get(tree, trees['ath12k'] if name.startswith('src/') else [])
    if name.startswith('src/nw_cfg80211_'): extra = trees['cfg80211']
    if name == 'src/nw_fils.c': extra = trees['mac80211']
    if name == 'src/nw_crypto_mac80211.c': extra = trees['crypto'] + trees['mac80211']
    if tree == 'crypto' and name == 'aes-siv.c':
        extra = [*extra, '-Daes_siv_encrypt=nw_aes_siv_encrypt', '-Daes_siv_decrypt=nw_aes_siv_decrypt']
    jobs.append((src, obj, extra))

def build(job):
    src, obj, extra = job
    raw_obj = raw / obj.name
    dependencies = raw_obj.with_suffix('.d')
    identity = raw_obj.with_suffix('.inputs.json')
    command = [clang, '-c', *extra, *flags, '-MD', '-MF', str(dependencies), str(src), '-o', str(raw_obj)]
    if raw_obj.exists() and identity.exists():
        record = json.loads(identity.read_text())
        if record.get('schema') == 1 and record.get('command') == command and record.get('object_sha256') == hashlib.sha256(raw_obj.read_bytes()).hexdigest() and all(
            Path(path).is_file() and hashlib.sha256(Path(path).read_bytes()).hexdigest() == digest
            for path, digest in record.get('dependencies', {}).items()):
            return raw_obj
    r = subprocess.run(command, capture_output=True, text=True)
    if r.returncode:
        sys.stderr.write(r.stderr)
        raise SystemExit(f'compile failed: {src}')
    obj.with_suffix('.command.json').write_text(json.dumps(command) + '\n')
    header_paths = shlex.split(dependencies.read_text().replace('\\\n', '').partition(':')[2])
    identity.write_text(json.dumps({'schema': 1, 'command': command, 'object_sha256': hashlib.sha256(raw_obj.read_bytes()).hexdigest(),
        'dependencies': {str(Path(path).absolute()): hashlib.sha256(Path(path).read_bytes()).hexdigest() for path in header_paths}}, sort_keys=True) + '\n')
    return raw_obj

with ThreadPoolExecutor(8) as pool:
    raw_objects = list(pool.map(build, jobs))

# The module's global names (sock_release, kernel_sendmsg, ...) are common in a kernel: give every one the prefix nwk_ except the entry points the board uses.
keep = {'_nw_wifi_start', '_nw_wifi_step', '_nw_wifi_stop', '_nw_wifi_stats', '_nw_wifi_set_events', '_nw_wifi_scan', '_nw_wifi_bss_count',
        '_nw_wifi_bss_get', '_nw_wifi_connect', '_nw_wifi_disconnect', '_nw_wifi_tx', '_nw_wifi_status_get', '_nw_wifi_where',
        '_nw_wifi_power',
        '_nw_wifi_tx_packet', '_nw_wifi_link_state',
        '_nw_wifi_tx_reasons', '_nw_wifi_buffer_stats_get', '_nw_wifi_buffer_pool_configure', '_nw_wifi_buffer_pool_trim',
        '_nw_wifi_irq_mode', '_nw_wifi_irq_pending', '_nw_wifi_wait_ms', '_nw_wifi_poll'}
keep.update({'_nw_wifi_control_transport_set', '_nw_wifi_control_attach', '_nw_wifi_control_detach',
             '_nw_wifi_control_receive', '_nw_wifi_control_writable', '_nw_wifi_control_stats_get'})
keep.add('_nw_wifi_set_platform')
objs = [o for _, o, _ in jobs]
names = set()
llvm = Path(subprocess.run(['brew', '--prefix', 'llvm'], check=True, capture_output=True, text=True).stdout.strip()) / 'bin'
for o in raw_objects:
    r = subprocess.run([str(llvm / 'llvm-nm'), '-g', '--defined-only', str(o)], check=True, capture_output=True, text=True)
    for line in r.stdout.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] not in keep:
            names.add(parts[2])
mapping = out / 'rename.txt'
mapping.write_text(''.join(f'{n} _nwk{n}\n' for n in sorted(names)))
for o in objs:
    shutil.copyfile(raw / o.name, o)
    subprocess.run([str(llvm / 'llvm-objcopy'), f'--redefine-syms={mapping}', str(o)], check=True)
(out / 'build-report.json').write_text(json.dumps({
    'runtime': 'mainline', 'legacy_station_linked': False, 'hardware_verified': False,
    'objects': [{'path': str(obj), 'source': str(src), 'source_sha256': hashlib.sha256(src.read_bytes()).hexdigest(),
                 'object_sha256': hashlib.sha256(obj.read_bytes()).hexdigest()} for src, obj, _ in jobs],
    'source_identity': json.loads((module / 'out/source-identity.json').read_text()),
    'flags': flags,
}, indent=2) + '\n')
for o in objs:
    print(f'OBJ {o}')
