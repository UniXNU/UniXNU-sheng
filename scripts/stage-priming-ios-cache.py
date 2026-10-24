#!/usr/bin/env python3
"""Stage the exact iOS priming worker in a fresh own QEMU Data clone.

--pack prepopulates actual iOS-exported bytes and selects a consumer-only boot.
No physical device, original code modification or production root acceptance.
"""
import argparse
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
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--pack', type=Path, help='Exact verified actual-iOS priming export; first consumer, no producer')
args = parser.parse_args()
OUT = args.output.resolve()


def require(value, reason):
    if not value:
        raise SystemExit(reason)


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(command, timeout=30):
    return subprocess.run([str(x) for x in command], check=True, capture_output=True, text=True, timeout=timeout)


require(not OUT.exists(), 'Preserve an existing staged fixture; choose a fresh directory')
require(shutil.disk_usage(ROOT).free > 5 * 1024**3 + 256 * 1024**2, 'Preserve 5 GiB and bounded allowance')
base_path = BASE / 'stage-report.json'
base = json.loads(base_path.read_text())
require(sha(BASE / 'ui-compiler-data-64.cdr') == base['data_sha256'], 'Exact unrun own initial image; never clone the mutable guest snapshot')
qualified_path = ROOT / 'build/priming-cache/qualified-ios-link/checkpoint.json'
qualified = json.loads(qualified_path.read_text())
for name, digest in qualified['sources_sha256'].items():
    require(sha(ROOT / name) == digest, 'Exact qualified compiler instrument source: ' + name)
for name, digest in qualified['runtime_patches_sha256'].items():
    require(sha(ROOT / 'drivers/AdrenoMetal/bridge/patches' / name) == digest, 'Exact qualified runtime patch')
require(sha(ROOT / 'build/adreno-metal-bridge/ios/libadreno_metal_bridge.a') == qualified['ios_cross_link']['bridge_sha256'], 'Exact cross-linked iOS bridge')
OUT.mkdir(parents=True)
for name, info in qualified['ios_cross_link']['outputs'].items():
    source = ROOT / 'build/priming-cache/ios' / name
    require(sha(source) == info['sha256'], 'Exact signed priming worker/loader')
    shutil.copyfile(source, OUT / name)
for name in ['libOfflineTurnipCompilerPeer.dylib','libvulkan_freedreno.dylib']:
    require(sha(BASE / name) == base['outputs'][name]['sha256'], 'Exact unchanged software peer and own-signed compiler')
    shutil.copyfile(BASE / name, OUT / name)
peer_build = json.loads((ROOT / 'build/turnip-ios-compiler-peer/build-report.json').read_text())
for name, digest in peer_build['sources'].items():
    require(sha(ROOT / name) == digest, 'Exact linked software peer source')
source = ROOT / 'drivers/AdrenoMetal/bridge/priming_cache_ios.cpp'
shared = ROOT / 'drivers/AdrenoMetal/bridge/quartzcore_cache_ios.cpp'
require(sha(shared) == base['parent_source_sha256'], 'Exact shared volume/capture helpers')
sdk = subprocess.check_output(['xcrun','--sdk','iphoneos','--show-sdk-path'],text=True,timeout=10).strip()
parent = OUT / 'air-native'
run(['xcrun','clang++','-target','arm64-apple-ios17.0','-isysroot',sdk,'-O1','-std=c++23','-fstack-protector-strong','-Wall','-Wextra','-Werror','-Wno-deprecated-declarations',source,
     '-Wl,-dead_strip','-Wl,-S','-framework','CoreFoundation','-framework','IOKit','-o',parent],60)
policy = ROOT / 'build/turnip-ios-compiler-peer/compiler-entitlements.plist'
permissions = {'com.apple.private.security.no-container':True,'com.apple.private.bindfs-allow':True}
require(plistlib.loads(policy.read_bytes()) == permissions, 'Exact own helper permissions; no no-sandbox')
run(['codesign','--force','--sign','-','--pagesize','4096','--identifier','org.blu-sharky.offline-priming-cache.parent','--entitlements',policy,parent])
files = [parent, OUT/'compiler-worker', OUT/'libOfflinePrimingCacheControl.dylib', OUT/'libOfflineTurnipCompilerPeer.dylib', OUT/'libvulkan_freedreno.dylib']
outputs = {}
for path in files:
    run(['codesign','--verify','--strict',path])
    details = run(['codesign','--display','--verbose=4',path]).stderr
    cdhash = re.search(r'^CDHash=([a-f0-9]{40})$',details,re.M)
    require(cdhash and re.search(r'^Page size=4096$',details,re.M) and struct.unpack_from('<3I',path.read_bytes()) == (0xfeedfacf,0x100000c,0), 'Actual own ordinary ARM64 4K signature')
    if path.name in ['air-native','compiler-worker']:
        actual = plistlib.loads(subprocess.check_output(['codesign','-d','--entitlements',':-',str(path)],stderr=subprocess.DEVNULL,timeout=30))
        require(actual == permissions, 'Actual helper entitlements')
    outputs[path.name] = dict(sha256=sha(path),bytes=path.stat().st_size,cdhash=cdhash[1])
trust = (BASE/'offline-trust.bundle').read_bytes()
require(sha(BASE/'offline-trust.bundle') == base['trust_bundle_sha256'] and struct.unpack_from('<I',trust)[0]==3,'Exact old combined trust input')
offsets = struct.unpack_from('<3I',trust,4)
hashes = sorted(info['cdhash'] for info in outputs.values())
own = struct.pack('<I16sI',2,uuid.uuid5(uuid.NAMESPACE_URL,'offline-priming-cache:'+':'.join(hashes)).bytes,len(hashes))
own += b''.join(bytes.fromhex(h)+bytes([2,0,0,0]) for h in hashes)
modules = [own,trust[offsets[1]:offsets[2]],trust[offsets[2]:]]
require([hashlib.sha256(x).hexdigest() for x in modules[1:]] == ['da88460e6e513438d5307e5c526f76497a74754476a24da29f239f0c90120fc2','d121ba89167e03aafd00fa687c30cd8ebc4ccafd10c1408adb2ba3de12403651'],'Both complete original trust modules unchanged')
starts,cursor = [],16
for module in modules:
    starts.append(cursor);cursor += len(module)
(OUT/'offline-trust.bundle').write_bytes(struct.pack('<4I',3,*starts)+b''.join(modules))
image = OUT/'ui-compiler-data-64.cdr'
shutil.copyfile(BASE/'ui-compiler-data-64.cdr',image)
pack, pack_path = None, None
if args.pack:
    pack_path = args.pack.resolve()/'checkpoint.json';pack = json.loads(pack_path.read_text())
    require(pack['actual_ios_priming_cache_hit'] and pack['ios_driver_input_sha256']==base['driver_input_sha256'] and
            pack['runtime_patches_sha256']==qualified['runtime_patches_sha256'],'Exact verified iOS/compiler/runtime priming pack')
    for name, info in pack['cache_manifest'].items():
        path = args.pack.resolve()/'cache'/name
        require(path.is_file() and not path.is_symlink() and path.stat().st_size<16*1024**2 and sha(path)==info['sha256'],'Exact initial priming cache byte')
    require(len(pack['cache_manifest'])==1 and sha(args.pack.resolve()/'priming.record')==pack['cache_record_sha256'],'Exact single priming record')
mount = OUT/'data-mount';mount.mkdir()
attached = plistlib.loads(subprocess.check_output(['hdiutil','attach','-readwrite','-nobrowse','-owners','off','-mountpoint',str(mount),'-plist',str(image)],timeout=30))
physical = attached['system-entities'][0]['dev-entry'];staged={}
try:
    info = plistlib.loads(subprocess.check_output(['diskutil','info','-plist',str(mount)],timeout=10))
    require(info['VolumeUUID']==base['data_volume_uuid'] and info['TotalSize']==64*1024**2 and info['BusProtocol']=='Disk Image','Exact own cloned Data volume')
    require(not (mount/'tmp/priming.cache').exists() and not (mount/'tmp/priming.archive').exists() and not (mount/'tmp/priming-worker.log').exists(),'Unrun priming fixture')
    old = mount/base['staged_paths']['libOfflineGraphicsCacheControl.dylib']
    require(sha(old)==base['outputs'][old.name]['sha256'],'Only remove the exact superseded own test loader in this clone')
    old.unlink()
    addons = mount/'tmp/native-compiler-addons/OfflineTurnipCompiler'
    for path in files:
        dest = (mount/'tmp' if path.name in ['air-native','compiler-worker'] else addons)/path.name
        shutil.copyfile(path,dest);dest.chmod(0o755);require(sha(dest)==outputs[path.name]['sha256'],'Actual staged signed executable')
        staged[path.name] = str(dest.relative_to(mount))
    if pack:
        shutil.copytree(args.pack.resolve()/'cache',mount/'tmp/priming.cache')
        shutil.copyfile(args.pack.resolve()/'priming.record',mount/'tmp/priming.archive')
        (mount/'tmp/priming-first-consumer').write_text('priming-first-v1\n')
        for name, entry in pack['cache_manifest'].items():
            require(sha(mount/'tmp/priming.cache'/name)==entry['sha256'],'Actual initial-image priming cache byte')
finally:
    run(['hdiutil','detach',physical])
require(sha(BASE/'ui-compiler-data-64.cdr')==base['data_sha256'],'Source unrun image remains unchanged')
report = dict(base)
report.update(evidence='owned iOS priming fixture cross-build/signing/staging only',record_kind='device-priming',first_consumer_only=bool(pack),
    stage_script_relative=str(Path(__file__).relative_to(ROOT)),stage_script_sha256=sha(Path(__file__)),parent_source_relative=str(source.relative_to(ROOT)),parent_source_sha256=sha(source),
    parent_source_dependencies={str(shared.relative_to(ROOT)):sha(shared)},qualified_host_sources=qualified['sources_sha256'],runtime_patches_sha256=qualified['runtime_patches_sha256'],
    runtime_base_revision=qualified['runtime_base_revision'],ios_bridge_sha256=qualified['ios_cross_link']['bridge_sha256'],outputs=outputs,staged_paths=staged,
    data_sha256=sha(image),trust_bundle_sha256=sha(OUT/'offline-trust.bundle'),own_parent_permissions=permissions,
    native_metal_revision=subprocess.check_output(['git','-C',str(ROOT/'drivers/AdrenoMetal'),'rev-parse','HEAD'],text=True).strip(),
    initial_pack_report_relative=str(pack_path.relative_to(ROOT)) if pack_path else None,initial_pack_report_sha256=sha(pack_path) if pack_path else None,
    initial_cache_manifest_sha256=pack['cache_manifest_sha256'] if pack else None,initial_record_sha256=pack['cache_record_sha256'] if pack else None,
    producer_runs_in_boot=0 if pack else 1,cache_execution_qualified=False,production_root_authorization=False,hardware_ready=False)
(OUT/'stage-report.json').write_text(json.dumps(report,indent=2)+'\n')
print('PASS: new priming instrument staged in an owned fresh image; original OS/trust/source inputs unchanged')
print('Mode:', 'first strict consumer, no producer' if pack else 'cold refusal, producer, strict consumer, UUID refusal')
