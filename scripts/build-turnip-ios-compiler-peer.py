#!/usr/bin/env python3
"""Cross-build a QEMU-only iOS compiler peer; never build a deployment peer.

The iOS Turnip library is an existing input, left untouched. The authored
software transport and compiler-only fixture are separately signed outputs.
This script neither starts QEMU nor contacts or stages a physical device.
"""
import hashlib
import json
import plistlib
from pathlib import Path
import re
import shutil
import struct
import subprocess
import uuid

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/turnip-ios-compiler-peer'
KIT = ROOT / 'drivers/Adreno740'
SIM = KIT / 'tests/hostsim'
SMMU = ROOT / 'kits/ArmSmmu4K'
TURNIP = ROOT / 'build/mesa-tu-ios/src/freedreno/vulkan/libvulkan_freedreno.dylib'
if shutil.disk_usage(ROOT).free < 5 * 1024**3 + 512 * 1024**2:
    raise SystemExit('Preserve 5 GiB and a bounded cross-build allowance')
if not TURNIP.is_file() or not (ROOT / 'build/turnip-smoke/NativeTurnipShaders.h').is_file():
    raise SystemExit('Build the iOS Turnip library and own shader fixtures first')
OUT.mkdir(exist_ok=True)
(OUT / 'build-report.json').unlink(missing_ok=True)


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(command):
    subprocess.run([str(x) for x in command], check=True, timeout=90)


def check_macho(path):
    raw = path.read_bytes()
    if struct.unpack_from('<3I', raw) != (0xfeedfacf, 0x100000c, 0):
        raise SystemExit('Ordinary ARM64 subtype 0 required: ' + path.name)
    cursor, platforms, identities = 32, [], []
    for _ in range(struct.unpack_from('<I', raw, 16)[0]):
        command, length = struct.unpack_from('<2I', raw, cursor)
        if length < 8 or cursor + length > len(raw):
            raise SystemExit('Invalid Mach-O command extent')
        if command == 0x32:
            platforms.append(struct.unpack_from('<I', raw, cursor + 8)[0])
        if command == 0x1b:
            identities.append(raw[cursor + 8:cursor + 24].hex())
        cursor += length
    if platforms != [2]:
        raise SystemExit('Actual iOS platform required: ' + path.name)
    return dict(sha256=sha(path), platform=2, cpu_subtype=0, macho_uuids=identities)


identity = check_macho(TURNIP)
sdk = subprocess.check_output(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], text=True).strip()
flags = ['-target', 'arm64-apple-ios17.0', '-isysroot', sdk, '-O1', '-fPIC', '-fstack-protector-strong']
include = OUT / 'include'
for name in ['IOKit/IOTypes.h', 'IOKit/IOReturn.h', 'IOKit/IOBufferMemoryDescriptor.h', 'IOKit/IOLib.h', 'kern/task.h']:
    path = include / name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text('#include "XnuMemoryFixture.h"\n')
objects, sources = [], []
for name in ['ArmSmmu4KContext', 'ArmSmmu4KProgram', 'ArmSmmu4KLive', 'ArmSmmu4KStreams', 'ArmSmmu4KMmio', 'AdrenoPM4']:
    source = (ROOT / 'kits/AdrenoPM4' if name == 'AdrenoPM4' else SMMU) / (name + '.c')
    obj = OUT / (name + '.o')
    run(['xcrun', 'clang', *flags, '-std=c11', '-c', source, '-o', obj])
    objects.append(obj)
    sources.append(source)
for name in ['host_gpu_service', 'NativeAdrenoMemory', 'NativeAdrenoSmmuMemory', 'NativeAdrenoVm', 'NativeAdrenoRing',
             'NativeAdrenoSubmit', 'NativeAdrenoUserClient', 'NativeAdrenoTimer', 'NativeAdrenoSmmuContext', 'NativeAdrenoGpuContext']:
    source = (SIM if name == 'host_gpu_service' else KIT) / (name + '.cpp')
    obj = OUT / (name + '.o')
    run(['xcrun', 'clang++', *flags, '-std=c++23', '-I', include, '-I', SIM.parent, '-I', KIT, '-I', SMMU,
         '-I', ROOT / 'kits/AdrenoPM4', '-c', source, '-o', obj])
    objects.append(obj)
    sources.append(source)
shim = SIM / 'host_iokit_shim.cpp'
obj = OUT / 'iokit_interposition.o'
run(['xcrun', 'clang++', *flags, '-std=c++17', '-c', shim, '-o', obj])
objects.append(obj)
sources.extend([shim, SIM / 'pipeline_cache.cpp', SIM / 'pipeline_cache_ios.cpp'])
peer = OUT / 'libOfflineTurnipCompilerPeer.dylib'
run(['xcrun', 'clang++', *flags, '-dynamiclib', '-install_name', '@rpath/' + peer.name, *objects,
     '-framework', 'IOKit', '-framework', 'CoreFoundation', '-o', peer])
exe = OUT / 'air-native'
worker = OUT / 'compiler-worker'
program = ['xcrun', 'clang++', *flags, '-std=c++23', '-Wall', '-Wextra', '-Werror', '-Wno-unused-const-variable',
     '-I', ROOT / '.cache/mesa-work/include', '-I', ROOT / 'build/turnip-smoke', SIM / 'pipeline_cache_ios.cpp',
     '-framework', 'CoreFoundation', '-framework', 'IOKit']
run([*program, '-o', exe])
run([*program, '-Wl,-needed_library,' + str(peer), '-Wl,-rpath,/System/Library/Extensions/OfflineTurnipCompiler', '-o', worker])
hashes, signed = [], {}
entitlements = {'com.apple.private.security.no-container': True, 'com.apple.private.bindfs-allow': True}
policy = OUT / 'compiler-entitlements.plist'
policy.write_bytes(plistlib.dumps(entitlements))
for path in [peer, exe, worker]:
    sign = ['codesign', '--force', '--sign', '-', '--pagesize', '4096', '--identifier', 'org.blu-sharky.offline-turnip-compiler.' + path.stem]
    if path in (exe, worker):
        sign += ['--entitlements', policy]
    run([*sign, path])
    run(['codesign', '--verify', '--strict', path])
    details = subprocess.run(['codesign', '--display', '--verbose=4', path], check=True, capture_output=True, text=True).stderr
    match = re.search(r'^CDHash=([0-9a-f]{40})$', details, re.M)
    if not match or not re.search(r'^Page size=4096$', details, re.M):
        raise SystemExit('Actual own 4K CodeDirectory required')
    hashes.append(match[1])
    signed[path.name] = dict(**check_macho(path), cdhash=match[1], code_signature_page_bytes=4096)
    if path in (exe, worker):
        actual = subprocess.check_output(['codesign', '-d', '--entitlements', ':-', str(path)], stderr=subprocess.DEVNULL)
        if plistlib.loads(actual) != entitlements:
            raise SystemExit('Exact own service entitlement required')
        signed[path.name]['entitlements'] = entitlements
# A trust module for own peer/program only; the existing signed driver needs
# its own existing hash when a QEMU fixture is staged. Original modules are
# appended unchanged by that staging step, not replaced by this module.
hashes.sort()
module = struct.pack('<I16sI', 2, uuid.uuid5(uuid.NAMESPACE_URL, 'offline-turnip-compiler:' + ':'.join(hashes)).bytes, len(hashes))
module += b''.join(bytes.fromhex(h) + bytes([2, 0, 0, 0]) for h in hashes)
(OUT / 'own-trust-module.bin').write_bytes(module)
if check_macho(TURNIP) != identity:
    raise SystemExit('The iOS driver input changed')
sources.append(KIT / 'tests/XnuMemoryFixture.h')
sources.extend(KIT.glob('*.h'))
sources.extend(SMMU.glob('*.h'))
sources.extend((ROOT / 'kits/AdrenoPM4').glob('*.h'))
report = dict(evidence='cross-build only; QEMU-only software transport, no shader execution or physical device access',
              sources={str(p.relative_to(ROOT)): sha(p) for p in sources}, outputs=signed,
              source_revisions={str(path.relative_to(ROOT)): subprocess.check_output(
                  ['git', '-C', str(path), 'rev-parse', 'HEAD'], text=True).strip()
                  for path in (KIT, SMMU, ROOT / 'kits/AdrenoPM4')},
              shader_fixture_sha256=sha(ROOT / 'build/turnip-smoke/NativeTurnipShaders.h'),
              ios_driver_input=identity, ios_driver_modified=False, own_trust_module_sha256=sha(OUT / 'own-trust-module.bin'),
              native_cpu_4k_observed=False, native_gpu_4k_observed=False, cache_import_observed=False,
              physical_device_tested=False, deployment_peer=False, hardware_ready=False)
(OUT / 'build-report.json').write_text(json.dumps(report, indent=2) + '\n')
print('PASS: QEMU-only compiler peer and separate-process cache instrument cross-built/signed as ARM64 subtype 0/iOS17')
print('The iOS Turnip input is unchanged; no cache execution or GPU execution is qualified')
