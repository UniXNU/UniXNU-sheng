#!/usr/bin/env python3
"""Stage only own QEMU compiler code into a cloned mutable Data fixture."""
import hashlib
import json
from pathlib import Path
import plistlib
import re
import shutil
import struct
import subprocess
import sys
import uuid

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/turnip-ios-compiler-peer'
sys.path.insert(0, str(ROOT / 'kits/XnuPrelink'))
from native_prelink import macho


def require(value, reason):
    if not value:
        raise SystemExit(reason)


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def code_identity(path):
    raw = path.read_bytes()
    image = macho(raw)  # also requires ordinary ARM64 subtype 0
    code = {s['name'] + '.' + t['name']: hashlib.sha256(raw[t['fileoff']:t['fileoff'] + t['size']]).hexdigest()
            for s in image['segments'] if s['initprot'] & 4 for t in s['sections']}
    ids = [raw[at + 8:at + 24].hex() for command, at, length in image['commands'] if command == 0x1b and length == 24]
    platforms = [struct.unpack_from('<I', raw, at + 8)[0] for command, at, _ in image['commands'] if command == 0x32]
    require(code and len(ids) == 1 and platforms == [2], 'Exact iOS code/build identity required')
    return dict(code_sections_sha256=code, macho_uuid=ids[0], platform=2, cpu_subtype=0)


def own_signature(path):
    subprocess.run(['codesign', '--verify', '--strict', path], check=True, capture_output=True, timeout=30)
    detail = subprocess.run(['codesign', '--display', '--verbose=4', path], check=True, capture_output=True, text=True, timeout=30).stderr
    match = re.search(r'^CDHash=([a-f0-9]{40})$', detail, re.M)
    require(match and re.search(r'^Page size=4096$', detail, re.M), 'Own 4K CodeDirectory required')
    return match[1]


build = json.loads((OUT / 'build-report.json').read_text())
for name, digest in build['sources'].items():
    require(sha(ROOT / name) == digest, 'Exact cross-built source: ' + name)
require(sha(ROOT / 'build/turnip-smoke/NativeTurnipShaders.h') == build['shader_fixture_sha256'],
        'Exact authored shader fixture')
require('drivers/Adreno740/NativeAdrenoVmAlias.h' in build['sources'], 'Complete current VM header dependency required')
for name, info in build['outputs'].items():
    require(sha(OUT / name) == info['sha256'] and own_signature(OUT / name) == info['cdhash'], 'Exact own signed cross-build')
driver = ROOT / 'build/mesa-tu-ios/src/freedreno/vulkan/libvulkan_freedreno.dylib'
require(sha(driver) == build['ios_driver_input']['sha256'], 'Exact iOS driver input')
identity = code_identity(driver)
image = OUT / 'compiler-data-32.cdr'
require(not image.exists(), 'Preserve the existing compiler Data observation')
require(shutil.disk_usage(ROOT).free > 5 * 1024**3 + 128 * 1024**2, 'Preserve 5 GiB free')
signed_driver = OUT / 'libvulkan_freedreno.dylib'
shutil.copyfile(driver, signed_driver)
subprocess.run(['codesign', '--force', '--sign', '-', '--pagesize', '4096', '--identifier',
                'org.blu-sharky.offline-turnip-compiler.driver', signed_driver], check=True, timeout=30)
require(code_identity(signed_driver) == identity and sha(driver) == build['ios_driver_input']['sha256'],
        'Own signing must preserve driver code and build ID; source input remains unchanged')
driver_cdhash = own_signature(signed_driver)
hashes = sorted([driver_cdhash] + [v['cdhash'] for v in build['outputs'].values()])
own = struct.pack('<I16sI', 2, uuid.uuid5(uuid.NAMESPACE_URL, 'offline-ios-compiler:' + ':'.join(hashes)).bytes, len(hashes))
own += b''.join(bytes.fromhex(h) + bytes([2, 0, 0, 0]) for h in hashes)
original = (ROOT / 'build/original-static-system-trust/original-static-system.bundle').read_bytes()
require(hashlib.sha256(original).hexdigest() == '7416ca4f8377146626bb284e2133c76bee6cb4e3ee0af5d648b186235a7323a7',
        'Exact original OS/SystemOS trust bundle')
require(struct.unpack_from('<I', original)[0] == 2, 'Two complete original trust modules')
starts = struct.unpack_from('<2I', original, 4)
modules = [own, original[starts[0]:starts[1]], original[starts[1]:]]
require([hashlib.sha256(x).hexdigest() for x in modules[1:]] == [
    'da88460e6e513438d5307e5c526f76497a74754476a24da29f239f0c90120fc2',
    'd121ba89167e03aafd00fa687c30cd8ebc4ccafd10c1408adb2ba3de12403651'], 'Complete original modules unchanged')
offsets, cursor = [], 4 + 4 * len(modules)
for module in modules:
    offsets.append(cursor)
    cursor += len(module)
trust = struct.pack('<4I', len(modules), *offsets) + b''.join(modules)
(OUT / 'offline-trust.bundle').write_bytes(trust)
seed = ROOT / 'build/owner-data-fixture/owner-data-32.cdr'
seed_report = json.loads(seed.with_name('seed.json').read_text())
require(sha(seed) == seed_report['sha256'], 'Exact own initial Data seed')
shutil.copyfile(seed, image)
mount = OUT / 'data-mount'
mount.mkdir(exist_ok=True)
attached = plistlib.loads(subprocess.check_output(['hdiutil', 'attach', image, '-readwrite', '-nobrowse', '-owners', 'off',
                                                 '-mountpoint', mount, '-plist'], timeout=30))
device = attached['system-entities'][0]['dev-entry']
staged = {}
try:
    info = plistlib.loads(subprocess.check_output(['diskutil', 'info', '-plist', mount], timeout=30))
    require(info['VolumeUUID'] == seed_report['volume_uuid'], 'Cloned Data volume identity')
    files = [OUT / name for name in build['outputs']] + [signed_driver]
    require(shutil.disk_usage(mount).free > sum(p.stat().st_size for p in files) + 1024**2, 'Sufficient fixture space')
    (mount / 'tmp').mkdir(exist_ok=True)
    addons = mount / 'tmp/native-compiler-addons/OfflineTurnipCompiler'
    addons.mkdir(parents=True, exist_ok=True)
    for path in files:
        destination = (mount / 'tmp' if path.name in ('air-native', 'compiler-worker') else addons) / path.name
        shutil.copyfile(path, destination)
        destination.chmod(0o755)
        require(sha(destination) == sha(path), 'Staged own binary identity')
        staged[path.name] = dict(sha256=sha(path), bytes=path.stat().st_size, path=str(destination.relative_to(mount)))
    (mount / 'tmp/compiler-data-uuid').write_text(seed_report['volume_uuid'])
finally:
    subprocess.run(['hdiutil', 'detach', device], check=True, timeout=30)
report = dict(bytes=image.stat().st_size, sha256=sha(image), seed_sha256=seed_report['sha256'],
              volume_uuid=seed_report['volume_uuid'], staged=staged, cross_build_report_sha256=sha(OUT / 'build-report.json'),
              driver_source_sha256=sha(driver), driver_own_signed_sha256=sha(signed_driver), driver_cdhash=driver_cdhash,
              driver_identity=identity, driver_code_and_build_id_unchanged=True, trust_bundle_sha256=sha(OUT / 'offline-trust.bundle'),
              original_trust_modules_unchanged=True, original_os_modified=False, own_mutable_data=True,
              qemu_observed=False, compiler_cache_qualified=False, deployment_peer=False, hardware_ready=False)
(OUT / 'data-report.json').write_text(json.dumps(report, indent=2) + '\n')
print('PASS: own compiler/peer and same-code iOS driver staged in a cloned Data fixture; original images/trust modules unchanged')
