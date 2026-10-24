#!/usr/bin/env python3
"""Qualify original UI recipe caches with real Turnip and a software transport.

The loader independently counts actual graphics compiler calls and their
feedback. Cache consumers forbid compilation; negative controls cannot silently
warm themselves. No shader pixels, physical GPU or system deadline is qualified.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]
KIT = ROOT / 'drivers/AdrenoMetal'
OUT = ROOT / 'build/ui-shader-coverage/turnip-graphics'
DRIVER = ROOT / 'build/mesa-tu/src/freedreno/vulkan/libvulkan_freedreno.dylib'
PEER = ROOT / 'build/turnip-darwin-host/libhostiokit.dylib'
LIBRARY = ROOT / '.cache/firmware/target-volumes/os-mount/System/Library/Frameworks/QuartzCore.framework/default.metallib'
BRIDGE = ROOT / 'build/adreno-metal-bridge/host/libadreno_metal_bridge.a'
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', type=Path, required=True, help='Fresh directory; preserve actual archive, cache, logs and identities')
parser.add_argument('--cross-build-ios', action='store_true', help='Also link/sign the same instrument for iOS; does not run it')
args = parser.parse_args()
work = args.output.resolve()


def require(value, reason):
    if not value:
        raise SystemExit(reason)


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def identity(path):
    raw = path.read_bytes()
    require(struct.unpack_from('<3I', raw) == (0xfeedfacf, 0x100000c, 0), 'Actual ordinary ARM64 image')
    cursor, uuids, platforms, minimum = 32, [], [], []
    for _ in range(struct.unpack_from('<I', raw, 16)[0]):
        command, size = struct.unpack_from('<2I', raw, cursor)
        require(size >= 8 and cursor + size <= len(raw), 'Bounded Mach-O load command')
        if command == 0x1b:
            uuids.append(raw[cursor + 8:cursor + 24].hex())
        if command == 0x32:
            platforms.append(struct.unpack_from('<I', raw, cursor + 8)[0])
            minimum.append(struct.unpack_from('<I', raw, cursor + 12)[0])
        cursor += size
    require(len(uuids) == 1 and len(platforms) == 1, 'Exact image build/platform identity')
    return dict(sha256=sha(path), uuid=uuids[0], platform=platforms[0], minimum_os=minimum[0], cpu_subtype=0)


def manifest(path):
    files = sorted(p for p in path.rglob('*') if p.is_file())
    require(0 < len(files) <= 2048 and sum(p.stat().st_size for p in files) < 64 * 1024**2, 'Bounded cache pack')
    return {str(p.relative_to(path)): dict(bytes=p.stat().st_size, sha256=sha(p)) for p in files}


require(not work.exists(), 'Preserve the existing qualification; use a fresh output directory')
require(shutil.disk_usage(ROOT).free > 5 * 1024**3 + 256 * 1024**2, 'Preserve 5 GiB and bounded allowance')
for p in [DRIVER, PEER, LIBRARY, BRIDGE]:
    require(p.is_file(), 'Build the real host driver/peer/bridge and mount the supplied target first')
require(sha(LIBRARY) == '606f913adf520420f9b4bb666efb782a68c830f082f1e559e17b20d0d13adbc9', 'Exact original library input')
OUT.mkdir(parents=True, exist_ok=True)
work.mkdir(parents=True)
loader, program = OUT / 'libOfflineGraphicsCacheControl.dylib', OUT / 'quartzcore_cache'
flags = ['xcrun', 'clang++', '-O1', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-Wno-deprecated-declarations']
subprocess.run([*flags, '-dynamiclib', '-I', ROOT / '.cache/mesa-work/include', KIT / 'bridge/graphics_cache_loader.cpp', '-o', loader], check=True, timeout=60)
subprocess.run([*flags, KIT / 'bridge/quartzcore_cache.mm', BRIDGE, '-Wl,-dead_strip', '-Wl,-S',
                '-framework', 'Foundation', '-framework', 'Metal', '-framework', 'CoreFoundation', '-framework', 'IOKit', '-o', program], check=True, timeout=90)
ios_link = None
if args.cross_build_ios:
    target = OUT / 'ios'
    target.mkdir(exist_ok=True)
    sdk = subprocess.check_output(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], text=True, timeout=10).strip()
    peer_dir = ROOT / 'build/turnip-ios-compiler-peer'
    peer = peer_dir / 'libOfflineTurnipCompilerPeer.dylib'
    peer_build = json.loads((peer_dir / 'build-report.json').read_text())
    require(sha(peer) == peer_build['outputs'][peer.name]['sha256'], 'Exact separately signed software peer')
    for name, digest in peer_build['sources'].items():
        require(sha(ROOT / name) == digest, 'Exact software peer compiled source: ' + name)
    bridge = ROOT / 'build/adreno-metal-bridge/ios/libadreno_metal_bridge.a'
    policy = peer_dir / 'compiler-entitlements.plist'
    import plistlib
    require(plistlib.loads(policy.read_bytes()) == {'com.apple.private.security.no-container': True,
            'com.apple.private.bindfs-allow': True}, 'Exact own offline helper permissions')
    ios_flags = [*flags, '-target', 'arm64-apple-ios17.0', '-isysroot', sdk, '-fstack-protector-strong']
    ios_loader, worker = target / loader.name, target / 'compiler-worker'
    subprocess.run([*ios_flags, '-dynamiclib', '-install_name', '@rpath/' + loader.name, '-I', ROOT / '.cache/mesa-work/include',
                    KIT / 'bridge/graphics_cache_loader.cpp', '-o', ios_loader], check=True, timeout=60)
    subprocess.run([*ios_flags, KIT / 'bridge/quartzcore_cache.mm', bridge, '-Wl,-dead_strip', '-Wl,-S',
                    '-Wl,-needed_library,' + str(peer), '-Wl,-rpath,/System/Library/Extensions/OfflineTurnipCompiler',
                    '-framework', 'Foundation', '-framework', 'Metal', '-framework', 'CoreFoundation', '-framework', 'IOKit',
                    '-o', worker], check=True, timeout=120)
    signed = {}
    for path in [ios_loader, worker]:
        sign = ['codesign', '--force', '--sign', '-', '--pagesize', '4096', '--identifier', 'org.blu-sharky.offline-ui-compiler.' + path.stem]
        if path == worker:
            sign += ['--entitlements', str(policy)]
        subprocess.run([*sign, str(path)], check=True, timeout=30)
        subprocess.run(['codesign', '--verify', '--strict', str(path)], check=True, timeout=30)
        details = subprocess.run(['codesign', '--display', '--verbose=4', str(path)], capture_output=True, text=True, check=True, timeout=30).stderr
        cdhash = re.search(r'^CDHash=([0-9a-f]{40})$', details, re.M)
        require(cdhash and re.search(r'^Page size=4096$', details, re.M), 'Actual own 4K CodeDirectory')
        info = identity(path)
        require(info['platform'] == 2 and info['minimum_os'] == 0x110000, 'Actual iOS17 platform')
        signed[path.name] = dict(**info, bytes=path.stat().st_size, cdhash=cdhash[1], signature_page_bytes=4096)
    ios_link = dict(evidence='cross-link and own-signature verification only; no iOS execution', outputs=signed,
                   bridge_sha256=sha(bridge), software_peer_sha256=sha(peer), software_peer_report_sha256=sha(peer_dir / 'build-report.json'),
                   worker_has_no_sandbox_entitlement=False, original_os_modified=False, production_root_authorization=False)
env = {k: v for k, v in os.environ.items() if not k.startswith(('IR3_', 'FD_MESA_', 'NIR_', 'TU_', 'HOSTSIM_', 'MESA_', 'NMB_TEST_'))}
env.update(DYLD_INSERT_LIBRARIES=str(PEER), NMB_TEST_GRAPHICS_CACHE_LOADER=str(loader), NMB_TEST_QUARTZCORE_LIBRARY=str(LIBRARY))
pattern = re.compile(r'^PASS: QuartzCore compiler (\S+) pid=(\d+) recipes=(\d+) calls=(\d+) success=(\d+) required=(\d+) valid=(\d+) hits=(\d+) uuid_mutations=(\d+) devices=(\d+) artifacts=(\d+)/(\d+)/(\d+) driver=(\d+)/(\d+)/(\d+)/(\d+) cached_calls=(\d+)/(\d+)/(\d+) device_graphics=(\d+) cold_required=(\d+) cold_object=(\d+) init_submissions=(\d+); no shader execution$', re.M)
fields = ['pid', 'recipes', 'calls', 'success', 'required', 'valid', 'hits', 'uuid_mutations', 'devices', 'artifact_hits',
          'artifact_misses', 'artifact_writes', 'driver_loads', 'runtime_driver_hits', 'runtime_driver_misses', 'driver_saves',
          'cached_calls', 'cached_valid', 'cached_hits', 'device_graphics', 'cold_required', 'cold_object', 'initialization_submissions']
cases, original_manifest = {}, None
prefix = work / 'ui-pack'
for mode in ['produce', 'consume', 'uuid-miss', 'empty-driver']:
    run = subprocess.run([str(program), str(DRIVER), str(prefix), mode], env=env, capture_output=True, text=True, timeout=120)
    log = work / (mode + '.log')
    log.write_text(run.stdout + run.stderr)
    shutil.copyfile(log, OUT / ('last-' + mode + '.log'))
    match = pattern.search(run.stdout)
    require(run.returncode == 0 and match and match[1] == mode, 'Actual UI compiler/cache control failed: ' + mode + '; see preserved log')
    values = dict(zip(fields, map(int, match.groups()[1:])))
    require(values['devices'] == 1 and values['pid'] > 1 and values['calls'] > 0, 'Real device and positive compiler call observations')
    if mode == 'produce':
        require(values['recipes'] == 24 and values['cold_required'] > 0 and values['artifact_misses'] == values['artifact_writes'] == 72 and values['driver_saves'] > 0, 'Actual translation/IR3 production and compile-forbidden cold refusal')
        original_manifest = manifest(Path(str(prefix) + '.cache'))
    elif mode == 'consume':
        require(values['recipes'] == 24 and values['required'] == values['artifact_misses'] == values['artifact_writes'] == values['driver_saves'] == 0 and
                values['calls'] == values['success'] == values['valid'] and values['cached_calls'] == values['cached_valid'] == values['cached_hits'] > 0,
                'Every actual UI driver creation succeeds while compilation is forbidden, with real application-hit feedback for every shader cache')
    else:
        require(values['recipes'] == 0 and values['required'] > 0 and values['cached_hits'] == 0 and
                (values['uuid_mutations'] > 0 if mode == 'uuid-miss' else values['driver_loads'] == 0), 'Genuine UUID/empty-driver refusal')
    require(manifest(Path(str(prefix) + '.cache')) == original_manifest, 'Consumed/negative controls preserve every producer cache byte')
    values['log_sha256'] = sha(log)
    cases[mode] = values
    print(match[0], flush=True)
require(len({v['pid'] for v in cases.values()}) == 4, 'Four distinct fresh processes')
archive = Path(str(prefix) + '.archive')
report = dict(evidence='actual host Turnip/IR3 compiler, software transport; no shader execution',
    checker_sha256=sha(Path(__file__)), native_metal_revision=subprocess.check_output(['git', '-C', str(KIT), 'rev-parse', 'HEAD'], text=True).strip(),
    sources_sha256={str(p.relative_to(ROOT)): sha(p) for p in [KIT / 'bridge/quartzcore_cache.mm', KIT / 'bridge/graphics_cache_loader.cpp', KIT / 'bridge/NativeMetalObjects.rs']},
    mesa_base_revision=subprocess.check_output(['git', '-C', str(ROOT / '.cache/mesa-work'), 'rev-parse', 'HEAD'], text=True).strip(),
    mesa_pipeline_source_sha256=sha(ROOT / '.cache/mesa-work/src/freedreno/vulkan/tu_pipeline.cc'),
    mesa_patch_sha256={name: sha(ROOT / name) for name in json.loads((ROOT / 'scripts/data/adreno-compiler-inputs.json').read_text())['patches']},
    original_library_sha256=sha(LIBRARY), host_bridge_sha256=sha(BRIDGE), driver=identity(DRIVER), peer=identity(PEER),
    loader=identity(loader), process=identity(program), ios_cross_link=ios_link, archive_sha256=sha(archive), archive_bytes=archive.stat().st_size,
    cache_manifest=original_manifest, cases=cases, compile_control_enabled_by_test_loader=True,
    compilation_forbidden_scope='UI recipe Vulkan creations after device initialization; not device initialization',
    graphics_library_runtime_hit_counters_incomplete=True, complete_ui_variant_coverage=False, ios_execution=False,
    shader_execution=False, physical_device_tested=False, active_native_adreno_4k=False, hardware_ready=False, stable_120_fps_qualified=False)
(work / 'checkpoint.json').write_text(json.dumps(report, indent=2) + '\n')
(OUT / 'quartzcore-turnip-checkpoint.json').write_text(json.dumps(report, indent=2) + '\n')
print('PASS: unchanged target UI recipes have compile-forbidden host Turnip hits and UUID/empty-driver refusals')
print('Device initialization still makes a graphics pipeline and a software-peer submission; this is not an initial-image or frame-deadline qualification')
