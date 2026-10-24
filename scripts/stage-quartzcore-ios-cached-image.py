#!/usr/bin/env python3
"""Prepopulate a fresh owned QEMU image with exact iOS-compiled UI cache bytes.

Clone the unrun staged image, never the guest's modified snapshot. Only the own
parent changes; original OS images/signatures and complete trust modules stay
unchanged. This is a selected-recipe test image, not production authorization.
"""
import hashlib
import json
from pathlib import Path
import plistlib
import re
import shutil
import struct
import subprocess
import uuid

ROOT = Path(__file__).resolve().parents[1]
BASE = ROOT / 'build/quartzcore-ios-compiler-peer'
OUT = ROOT / 'build/quartzcore-ios-provisioned-peer'
PACK = BASE / 'exported-ui-pack-final'


def require(value, reason):
    if not value:
        raise SystemExit(reason)


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(command, timeout=30):
    return subprocess.run([str(x) for x in command], check=True, capture_output=True, text=True, timeout=timeout)


OUT.mkdir(exist_ok=True)
image = OUT / 'ui-compiler-data-64.cdr'
require(not image.exists(), 'Preserve the previous provisioned image; do not reuse a guest snapshot')
require(shutil.disk_usage(ROOT).free > 5 * 1024**3 + 256 * 1024**2, 'Preserve 5 GiB and bounded staging allowance')
base_report_path, pack_report_path = BASE / 'stage-report.json', PACK / 'checkpoint.json'
base, pack = json.loads(base_report_path.read_text()), json.loads(pack_report_path.read_text())
require(sha(base_report_path) == pack['stage_report_sha256'], 'Exact verified producer staging report')
require(sha(BASE / 'ui-compiler-data-64.cdr') == base['data_sha256'], 'Exact unrun own initial image, not the paused mutable snapshot')
require(pack['extracted_pack_matches_guest_manifest'] and pack['ios_driver_input_sha256'] == base['driver_input_sha256'] and
        pack['ios_driver_uuid'] == base['driver_identity']['macho_uuid'], 'Exact qualified iOS compiler/cache identity')
require(sha(ROOT / 'drivers/AdrenoMetal/bridge/quartzcore_cache_ios.cpp') == base['parent_source_sha256'], 'Exact shared parent helpers')
for name, digest in base['qualified_host_sources'].items():
    require(sha(ROOT / name) == digest, 'Exact UI compiler source: ' + name)
for name, info in base['outputs'].items():
    require(sha(BASE / name) == info['sha256'], 'Exact own signed binary: ' + name)
    if name != 'air-native':
        shutil.copyfile(BASE / name, OUT / name)
require(sha(PACK / 'ui.archive') == pack['archive_sha256'], 'Exact exported archive')
files = sorted(p for p in (PACK / 'ui.cache').rglob('*') if p.is_file())
require(len(files) == pack['cache_file_count'] <= 2048, 'Exact bounded initial cache inventory')
for path in files:
    require(not path.is_symlink() and path.stat().st_size <= 16 * 1024**2 and
            sha(path) == pack['cache_manifest'][str(path.relative_to(PACK / 'ui.cache'))]['sha256'], 'Exact exported iOS cache file')
source = ROOT / 'drivers/AdrenoMetal/bridge/quartzcore_cache_provisioned_ios.cpp'
parent = OUT / 'air-native'
sdk = subprocess.check_output(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], text=True, timeout=10).strip()
run(['xcrun', 'clang++', '-target', 'arm64-apple-ios17.0', '-isysroot', sdk, '-O1', '-fstack-protector-strong', '-std=c++23',
     '-Wall', '-Wextra', '-Werror', '-Wno-deprecated-declarations', source, '-Wl,-dead_strip', '-Wl,-S',
     '-framework', 'CoreFoundation', '-framework', 'IOKit', '-o', parent], timeout=60)
policy = ROOT / 'build/turnip-ios-compiler-peer/compiler-entitlements.plist'
require(plistlib.loads(policy.read_bytes()) == base['own_parent_permissions'], 'Exact own helper permissions')
run(['codesign', '--force', '--sign', '-', '--pagesize', '4096', '--identifier', 'org.blu-sharky.offline-ui-compiler.provisioned-parent',
     '--entitlements', policy, parent])
run(['codesign', '--verify', '--strict', parent])
details = run(['codesign', '--display', '--verbose=4', parent]).stderr
cdhash = re.search(r'^CDHash=([a-f0-9]{40})$', details, re.M)
require(cdhash and re.search(r'^Page size=4096$', details, re.M) and struct.unpack_from('<3I', parent.read_bytes()) == (0xfeedfacf, 0x100000c, 0), 'Actual ordinary ARM64 own 4K signature')
outputs = dict(base['outputs'])
outputs['air-native'] = dict(sha256=sha(parent), bytes=parent.stat().st_size, cdhash=cdhash[1])
trust = (BASE / 'offline-trust.bundle').read_bytes()
require(sha(BASE / 'offline-trust.bundle') == base['trust_bundle_sha256'] and struct.unpack_from('<I', trust)[0] == 3, 'Exact preceding trust input')
offsets = struct.unpack_from('<3I', trust, 4)
hashes = sorted(v['cdhash'] for v in outputs.values())
own = struct.pack('<I16sI', 2, uuid.uuid5(uuid.NAMESPACE_URL, 'offline-initial-ui-cache:' + ':'.join(hashes)).bytes, len(hashes))
own += b''.join(bytes.fromhex(h) + bytes([2, 0, 0, 0]) for h in hashes)
modules = [own, trust[offsets[1]:offsets[2]], trust[offsets[2]:]]
require([hashlib.sha256(x).hexdigest() for x in modules[1:]] == [
    'da88460e6e513438d5307e5c526f76497a74754476a24da29f239f0c90120fc2',
    'd121ba89167e03aafd00fa687c30cd8ebc4ccafd10c1408adb2ba3de12403651'], 'Both complete original trust modules unchanged')
new_offsets, cursor = [], 16
for module in modules:
    new_offsets.append(cursor)
    cursor += len(module)
(OUT / 'offline-trust.bundle').write_bytes(struct.pack('<4I', 3, *new_offsets) + b''.join(modules))
shutil.copyfile(BASE / 'ui-compiler-data-64.cdr', image)
mount = OUT / 'data-mount'
mount.mkdir(exist_ok=True)
attached = plistlib.loads(subprocess.check_output(['hdiutil', 'attach', '-readwrite', '-nobrowse', '-owners', 'off', '-mountpoint', str(mount), '-plist', str(image)], timeout=30))
physical = attached['system-entities'][0]['dev-entry']
try:
    info = plistlib.loads(subprocess.check_output(['diskutil', 'info', '-plist', str(mount)], timeout=10))
    require(info['VolumeUUID'] == base['data_volume_uuid'] and info['TotalSize'] == 64 * 1024**2 and info['BusProtocol'] == 'Disk Image', 'Exact own cloned Data')
    require(not (mount / 'tmp/quartzcore-ui.cache').exists() and not (mount / 'tmp/quartzcore-ui.archive').exists() and
            not (mount / 'tmp/quartzcore-compiler-worker.log').exists(), 'Source image has never run the UI producer suite')
    shutil.copyfile(parent, mount / 'tmp/air-native')
    (mount / 'tmp/air-native').chmod(0o755)
    shutil.copyfile(PACK / 'ui.archive', mount / 'tmp/quartzcore-ui.archive')
    shutil.copytree(PACK / 'ui.cache', mount / 'tmp/quartzcore-ui.cache')
    for path in files:
        relative = path.relative_to(PACK / 'ui.cache')
        require(sha(mount / 'tmp/quartzcore-ui.cache' / relative) == pack['cache_manifest'][str(relative)]['sha256'], 'Actual initial-image cache byte')
    for name, location in base['staged_paths'].items():
        require(sha(mount / location) == outputs[name]['sha256'], 'Actual cloned executable identity')
    require(sha(mount / 'tmp/quartzcore-ui.archive') == pack['archive_sha256'], 'Actual initial-image archive identity')
finally:
    run(['hdiutil', 'detach', physical])
require(sha(BASE / 'ui-compiler-data-64.cdr') == base['data_sha256'], 'The preceding unrun image remains unchanged')
report = dict(base)
report.update(evidence='fresh own initial-image staging only; no execution', first_consumer_only=True,
    stage_script_relative='scripts/stage-quartzcore-ios-cached-image.py', stage_script_sha256=sha(Path(__file__)),
    parent_source_relative=str(source.relative_to(ROOT)), parent_source_sha256=sha(source),
    parent_source_dependencies={'drivers/AdrenoMetal/bridge/quartzcore_cache_ios.cpp': base['parent_source_sha256']},
    native_metal_revision=subprocess.check_output(['git', '-C', str(ROOT / 'drivers/AdrenoMetal'), 'rev-parse', 'HEAD'], text=True).strip(),
    outputs=outputs, base_unrun_image_sha256=base['data_sha256'], data_sha256=sha(image), trust_bundle_sha256=sha(OUT / 'offline-trust.bundle'),
    initial_pack_report_relative=str(pack_report_path.relative_to(ROOT)), initial_pack_report_sha256=sha(pack_report_path),
    initial_archive_sha256=pack['archive_sha256'], initial_cache_manifest_sha256=pack['cache_manifest_sha256'],
    initial_cache_file_count=pack['cache_file_count'], initial_cache_bytes=pack['cache_bytes'],
    initial_cache_uuid=pack['actual_pipeline_cache_uuid'], producer_runs_in_boot=0,
    cache_execution_qualified=False, initial_image_selected_recipes_qualified=False, production_root_authorization=False)
(OUT / 'stage-report.json').write_text(json.dumps(report, indent=2) + '\n')
print('PASS: exact iOS-generated cache bytes prepopulated in a fresh own image; consumer-only parent is signed and staged')
print('No producer executes in the new parent; execution, complete UI coverage and production packaging remain unqualified')
