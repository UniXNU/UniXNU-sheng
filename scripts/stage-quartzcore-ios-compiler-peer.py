#!/usr/bin/env python3
"""Build/stage the owned UI compiler QEMU harness without changing original inputs.

A fresh 64 MiB clone retains the qualified test Data UUID. Only nodes returned
by hdiutil for that clone may be resized. No physical device is contacted.
"""
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
OUT = ROOT / 'build/quartzcore-ios-compiler-peer'
sys.path.insert(0, str(ROOT / 'kits/XnuPrelink'))
from native_prelink import macho


def require(value, reason):
    if not value:
        raise SystemExit(reason)


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(command, timeout=30):
    return subprocess.run([str(x) for x in command], check=True, capture_output=True, text=True, timeout=timeout)


def disk_info(path):
    return plistlib.loads(subprocess.check_output(['diskutil', 'info', '-plist', str(path)], timeout=10))


def sections(path):
    raw = path.read_bytes()
    image = macho(raw)
    ids = [raw[at + 8:at + 24].hex() for command, at, size in image['commands'] if command == 0x1b and size == 24]
    platforms = [struct.unpack_from('<I', raw, at + 8)[0] for command, at, _ in image['commands'] if command == 0x32]
    require(len(ids) == 1 and platforms == [2], 'Actual iOS build identity')
    return dict(macho_uuid=ids[0], platform=2, cpu_subtype=0,
        file_backed_sections={s['name'] + '.' + t['name']: hashlib.sha256(raw[t['fileoff']:t['fileoff'] + t['size']]).hexdigest()
                              for s in image['segments'] for t in s['sections'] if t['fileoff']})


def signature(path):
    run(['codesign', '--verify', '--strict', path])
    details = run(['codesign', '--display', '--verbose=4', path]).stderr
    match = re.search(r'^CDHash=([a-f0-9]{40})$', details, re.M)
    require(match and re.search(r'^Page size=4096$', details, re.M), 'Actual own 4K signature')
    return match[1]


OUT.mkdir(exist_ok=True)
image = OUT / 'ui-compiler-data-64.cdr'
require(not image.exists(), 'Preserve the previous UI compiler Data observation')
require(shutil.disk_usage(ROOT).free > 5 * 1024**3 + 512 * 1024**2, 'Preserve 5 GiB and bounded allowance')
qualified_path = ROOT / 'build/ui-shader-coverage/turnip-graphics/committed-qualified/checkpoint.json'
qualified = json.loads(qualified_path.read_text())
for name, digest in qualified['sources_sha256'].items():
    require(sha(ROOT / name) == digest, 'Exact qualified UI compiler source: ' + name)
source_outputs = ROOT / 'build/ui-shader-coverage/turnip-graphics/ios'
for name, info in qualified['ios_cross_link']['outputs'].items():
    source = source_outputs / name
    require(sha(source) == info['sha256'] and signature(source) == info['cdhash'], 'Exact cross-linked/signed UI worker or loader')
    shutil.copyfile(source, OUT / name)
peer_dir = ROOT / 'build/turnip-ios-compiler-peer'
peer_build = json.loads((peer_dir / 'build-report.json').read_text())
for name, digest in peer_build['sources'].items():
    require(sha(ROOT / name) == digest, 'Exact linked software-peer source: ' + name)
peer = peer_dir / 'libOfflineTurnipCompilerPeer.dylib'
require(sha(peer) == peer_build['outputs'][peer.name]['sha256'], 'Exact own software transport')
shutil.copyfile(peer, OUT / peer.name)
policy = peer_dir / 'compiler-entitlements.plist'
permissions = {'com.apple.private.security.no-container': True, 'com.apple.private.bindfs-allow': True}
require(plistlib.loads(policy.read_bytes()) == permissions, 'Exact own helper permissions, no no-sandbox')
source = ROOT / 'drivers/AdrenoMetal/bridge/quartzcore_cache_ios.cpp'
sdk = subprocess.check_output(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], text=True, timeout=10).strip()
parent = OUT / 'air-native'
run(['xcrun', 'clang++', '-target', 'arm64-apple-ios17.0', '-isysroot', sdk, '-O1', '-fstack-protector-strong',
     '-std=c++23', '-Wall', '-Wextra', '-Werror', '-Wno-deprecated-declarations', source,
     '-framework', 'CoreFoundation', '-framework', 'IOKit', '-o', parent], timeout=60)
run(['codesign', '--force', '--sign', '-', '--pagesize', '4096', '--identifier', 'org.blu-sharky.offline-ui-compiler.parent',
     '--entitlements', policy, parent])
actual = plistlib.loads(subprocess.check_output(['codesign', '-d', '--entitlements', ':-', str(parent)], stderr=subprocess.DEVNULL, timeout=30))
require(actual == permissions, 'Actual parent permissions')
driver = ROOT / 'build/mesa-tu-ios/src/freedreno/vulkan/libvulkan_freedreno.dylib'
driver_identity = sections(driver)
signed_driver = OUT / driver.name
shutil.copyfile(driver, signed_driver)
run(['codesign', '--force', '--sign', '-', '--pagesize', '4096', '--identifier', 'org.blu-sharky.offline-ui-compiler.driver', signed_driver])
require(sections(signed_driver) == driver_identity, 'Every file-backed iOS driver section and UUID unchanged by own signing')
files = [parent, OUT / 'compiler-worker', OUT / 'libOfflineGraphicsCacheControl.dylib', OUT / peer.name, signed_driver]
outputs = {path.name: dict(sha256=sha(path), bytes=path.stat().st_size, cdhash=signature(path), identity=sections(path)) for path in files}
original = (ROOT / 'build/original-static-system-trust/original-static-system.bundle').read_bytes()
require(hashlib.sha256(original).hexdigest() == '7416ca4f8377146626bb284e2133c76bee6cb4e3ee0af5d648b186235a7323a7' and struct.unpack_from('<I', original)[0] == 2,
        'Exact two complete original trust modules')
starts = struct.unpack_from('<2I', original, 4)
hashes = sorted(v['cdhash'] for v in outputs.values())
own = struct.pack('<I16sI', 2, uuid.uuid5(uuid.NAMESPACE_URL, 'offline-ui-compiler:' + ':'.join(hashes)).bytes, len(hashes))
own += b''.join(bytes.fromhex(h) + bytes([2, 0, 0, 0]) for h in hashes)
modules = [own, original[starts[0]:starts[1]], original[starts[1]:]]
require([hashlib.sha256(m).hexdigest() for m in modules[1:]] == [
    'da88460e6e513438d5307e5c526f76497a74754476a24da29f239f0c90120fc2',
    'd121ba89167e03aafd00fa687c30cd8ebc4ccafd10c1408adb2ba3de12403651'], 'Original trust modules unchanged')
offsets, cursor = [], 16
for module in modules:
    offsets.append(cursor)
    cursor += len(module)
(OUT / 'offline-trust.bundle').write_bytes(struct.pack('<4I', 3, *offsets) + b''.join(modules))
seed = ROOT / 'build/owner-data-fixture/owner-data-32.cdr'
seed_report = json.loads(seed.with_name('seed.json').read_text())
require(sha(seed) == seed_report['sha256'] and seed.stat().st_size == 32 * 1024**2, 'Exact own preceding seed')
shutil.copyfile(seed, image)
with image.open('r+b') as file:
    file.truncate(64 * 1024**2)
attached = plistlib.loads(subprocess.check_output(['hdiutil', 'attach', '-nomount', '-readwrite', '-nobrowse', '-plist', str(image)], timeout=30))
physical = attached['system-entities'][0]['dev-entry']
try:
    info = disk_info(physical)
    require(info['VirtualOrPhysical'] == 'Virtual' and info['BusProtocol'] == 'Disk Image' and not info['Internal'] and
            info['TotalSize'] == 64 * 1024**2 and info['WholeDisk'], 'Resize only the exact newly attached image node')
    container = info['APFSContainerReference']
    cinfo = disk_info('/dev/' + container)
    require(cinfo['APFSPhysicalStores'] == [{'APFSPhysicalStore': info['DeviceIdentifier']}], 'Exact own image physical store')
    resized = run(['diskutil', 'apfs', 'resizeContainer', '/dev/' + container, '0'])
    (OUT / 'resize.log').write_text(resized.stdout + resized.stderr)
finally:
    run(['hdiutil', 'detach', physical])
require(image.stat().st_size == 64 * 1024**2 and image.read_bytes()[32:36] == b'NXSB', 'Actual larger raw APFS fixture')
mount = OUT / 'data-mount'
mount.mkdir(exist_ok=True)
attached = plistlib.loads(subprocess.check_output(['hdiutil', 'attach', '-readwrite', '-nobrowse', '-owners', 'off',
                                                 '-mountpoint', str(mount), '-plist', str(image)], timeout=30))
physical = attached['system-entities'][0]['dev-entry']
staged = {}
try:
    info = disk_info(mount)
    require(info['VolumeUUID'] == seed_report['volume_uuid'] and info['TotalSize'] == 64 * 1024**2, 'Actual preserved Data UUID and larger container')
    require(shutil.disk_usage(mount).free > sum(p.stat().st_size for p in files) + 8 * 1024**2, 'Bounded room for both programs and generated caches')
    addons = mount / 'tmp/native-compiler-addons/OfflineTurnipCompiler'
    addons.mkdir(parents=True, exist_ok=True)
    for path in files:
        dest = (mount / 'tmp' if path.name in ('air-native', 'compiler-worker') else addons) / path.name
        shutil.copyfile(path, dest)
        dest.chmod(0o755)
        require(sha(dest) == outputs[path.name]['sha256'], 'Actual staged own binary identity')
        staged[path.name] = str(dest.relative_to(mount))
    (mount / 'tmp/compiler-data-uuid').write_text(seed_report['volume_uuid'])
finally:
    run(['hdiutil', 'detach', physical])
require(sha(seed) == seed_report['sha256'] and sections(driver) == driver_identity, 'Source seed and driver remain unchanged')
report = dict(evidence='own cross-build, signing and mutable staging only; no QEMU or hardware execution',
    stage_script_sha256=sha(Path(__file__)), parent_source_sha256=sha(source), native_metal_revision=subprocess.check_output(['git', '-C', str(ROOT / 'drivers/AdrenoMetal'), 'rev-parse', 'HEAD'], text=True).strip(),
    qualified_host_report_sha256=sha(qualified_path), qualified_host_sources=qualified['sources_sha256'], software_peer_report_sha256=sha(peer_dir / 'build-report.json'),
    outputs=outputs, staged_paths=staged, driver_input_sha256=sha(driver), driver_identity=driver_identity,
    own_parent_permissions=permissions, source_seed_sha256=sha(seed), data_bytes=image.stat().st_size, data_sha256=sha(image),
    data_volume_uuid=seed_report['volume_uuid'], trust_bundle_sha256=sha(OUT / 'offline-trust.bundle'), original_trust_modules_unchanged=True,
    original_os_modified=False, source_driver_modified=False, cache_execution_qualified=False, shader_execution=False, physical_device_tested=False,
    active_native_adreno_4k=False, production_root_authorization=False, hardware_ready=False)
(OUT / 'stage-report.json').write_text(json.dumps(report, indent=2) + '\n')
print('PASS: own larger Data clone preserves UUID; exact iOS UI compiler, peer and loader are staged with own 4K signatures')
print('Original images and trust modules are unchanged; no execution or complete deployment pack is qualified')
