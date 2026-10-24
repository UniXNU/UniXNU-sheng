#!/usr/bin/env python3
"""Qualify same-iOS-driver cache compilation/import in bounded offline QEMU.

The peer executes no shader. This never qualifies Adreno execution, native GPU
translation, a complete UI cache pack, hardware timing or presentation.
"""
import argparse
import hashlib
import json
from pathlib import Path
import plistlib
import re
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/turnip-ios-compiler-peer'
sys.path.insert(0, str(ROOT / 'kits/XnuPrelink'))
from native_prelink import macho, backed_offset

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--observation', type=Path, default=ROOT / 'build/darwinboot-owner-hvf-turnip-compiler-linked-peer')
parser.add_argument('--driver-input', type=Path, default=ROOT / 'build/mesa-tu-ios/src/freedreno/vulkan/libvulkan_freedreno.dylib',
                    help='Exact original input of this preserved observation; never substitute a different build')
args = parser.parse_args()


def require(value, reason):
    if not value:
        raise SystemExit(reason)


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def load(path):
    return json.loads(path.read_text())


build, data = load(OUT / 'build-report.json'), load(OUT / 'data-report.json')
for name, digest in build['sources'].items():
    require(sha(ROOT / name) == digest, 'Exact compiled source: ' + name)
require(sha(ROOT / 'build/turnip-smoke/NativeTurnipShaders.h') == build['shader_fixture_sha256'],
        'Exact compiled authored shader fixture')
for name, info in build['outputs'].items():
    path = OUT / name
    require(sha(path) == info['sha256'], 'Exact own signed output: ' + name)
    raw = path.read_bytes()
    require(struct.unpack_from('<3I', raw) == (0xfeedfacf, 0x100000c, 0), 'Actual ARM64 subtype 0')
    subprocess.run(['codesign', '--verify', '--strict', path], check=True, capture_output=True, timeout=30)
    detail = subprocess.run(['codesign', '--display', '--verbose=4', path], check=True, capture_output=True, text=True, timeout=30).stderr
    require(info['cdhash'] in detail and re.search(r'^Page size=4096$', detail, re.M), 'Actual own 4K CodeDirectory')
    if name in ('air-native', 'compiler-worker'):
        entitlements = plistlib.loads(subprocess.check_output(['codesign', '-d', '--entitlements', ':-', str(path)], stderr=subprocess.DEVNULL, timeout=30))
        require(entitlements == info['entitlements'] == {
            'com.apple.private.security.no-container': True, 'com.apple.private.bindfs-allow': True},
            'Exact own offline helper permissions; no no-sandbox substitution')
require(sha(OUT / 'build-report.json') == data['cross_build_report_sha256'] and
        sha(OUT / 'compiler-data-32.cdr') == data['sha256'] and
        sha(OUT / 'offline-trust.bundle') == data['trust_bundle_sha256'], 'Exact staged own fixture')
driver = args.driver_input.resolve()
signed_driver = OUT / 'libvulkan_freedreno.dylib'
require(sha(driver) == build['ios_driver_input']['sha256'] == data['driver_source_sha256'] and
        sha(signed_driver) == data['driver_own_signed_sha256'], 'Exact original iOS driver input and own-signed copy')
all_sections = []
for path in (driver, signed_driver):
    raw = path.read_bytes()
    image = macho(raw)
    code = {s['name'] + '.' + t['name']: hashlib.sha256(raw[t['fileoff']:t['fileoff'] + t['size']]).hexdigest()
            for s in image['segments'] if s['initprot'] & 4 for t in s['sections']}
    ids = [raw[at + 8:at + 24].hex() for command, at, length in image['commands'] if command == 0x1b and length == 24]
    platforms = [struct.unpack_from('<I', raw, at + 8)[0] for command, at, _ in image['commands'] if command == 0x32]
    all_sections.append({s['name'] + '.' + t['name']: hashlib.sha256(raw[t['fileoff']:t['fileoff'] + t['size']]).hexdigest()
                         for s in image['segments'] for t in s['sections'] if t['fileoff']})
    require(code == data['driver_identity']['code_sections_sha256'] and ids == [data['driver_identity']['macho_uuid']] and
            platforms == [2] and ids == build['ios_driver_input']['macho_uuids'], 'Unchanged executable sections, actual iOS platform and build ID')
require(all_sections[0] == all_sections[1], 'Every file-backed driver section is unchanged by own signing')
subprocess.run(['codesign', '--verify', '--strict', signed_driver], check=True, capture_output=True, timeout=30)
detail = subprocess.run(['codesign', '--display', '--verbose=4', signed_driver], check=True, capture_output=True, text=True, timeout=30).stderr
require(data['driver_cdhash'] in detail and re.search(r'^Page size=4096$', detail, re.M), 'Actual own driver CodeDirectory')

trust = (OUT / 'offline-trust.bundle').read_bytes()
require(struct.unpack_from('<I', trust)[0] == 3, 'Own plus two original trust modules')
offsets = struct.unpack_from('<3I', trust, 4)
modules = [trust[a:z] for a, z in zip(offsets, (*offsets[1:], len(trust)))]
require([hashlib.sha256(x).hexdigest() for x in modules[1:]] == [
    'da88460e6e513438d5307e5c526f76497a74754476a24da29f239f0c90120fc2',
    'd121ba89167e03aafd00fa687c30cd8ebc4ccafd10c1408adb2ba3de12403651'], 'Both complete original trust modules preserved')
hashes = sorted([data['driver_cdhash']] + [v['cdhash'] for v in build['outputs'].values()])
require(struct.unpack_from('<I', modules[0], 20)[0] == len(hashes) and len(modules[0]) == 24 + 24 * len(hashes) and
        [modules[0][o:o + 20].hex() for o in range(24, len(modules[0]), 24)] == hashes and
        all(modules[0][o + 20:o + 24] == bytes([2, 0, 0, 0]) for o in range(24, len(modules[0]), 24)), 'Exact four own signed artifacts')

base = ROOT / 'build/xnu-prelink/native-owner-turnip-compiler-qemu-candidate'
plan = load(base.with_suffix('.json'))
kernel = base.with_suffix('.macho')
require(sha(kernel) == plan['candidate_sha256'] and
        plan['original_sha256'] == 'f18b4df865405f2a287b84977d7aca745e056d656d9fc9df1662970ce2969de7', 'Exact QEMU candidate and supplied original kernel')
raw = kernel.read_bytes()
image = macho(raw)
for name, digest in plan['original_executable_sha256'].items():
    extent = next(e for e in plan['extents'] if e['module'] == name and e['segment'] == '__TEXT_EXEC')
    offset = backed_offset(image['segments'], extent['va'], extent['size'], len(raw))
    require(hashlib.sha256(raw[offset:offset + extent['size']]).hexdigest() == digest, 'Whole original executable preserved: ' + name)
report_path = args.observation.resolve() / 'gdb-stop.json'
d = load(report_path)
require(d['kernel_sha256'] == plan['candidate_sha256'] and not d['bounded_timeout_interrupted'] and
        d['stop_symbol'] == '_exception_triage' and d['native_4k_mmu_observed'] and
        d['original_amfi_table_owner_observed'] and d['owner_root_binding_audit']['boot_root_accepted'] and
        d['actual_signature_failures'] == dict(_cs_validate_page_bad_hash=0, _cs_validate_page_no_hash=0) and
        d['actual_process']['pid'] == 1 and d['actual_process']['name'] == 'air-native' and
        d['scratch_data']['actual_seed_sha256'] == data['sha256'] and d['trust_cache_bundle_sha256'] == data['trust_bundle_sha256'] and
        d['qemu_accelerator'] == 'hvf' and not d['sheng_hardware_tested'], 'Actual bounded QEMU/original runtime, own parent, native CPU 4K and original signing/root')
u = d['entry_user_state_candidates'][0]
require(int(u['cpsr'], 16) & 31 == 0 and u['instruction_bytes'].startswith('000020d4'), 'Actual own EL0 breakpoint')
v = struct.unpack('<48Q', bytes.fromhex(u['register_memory']['d:180']['bytes']))
require(v[:6] == (0x54435051494f5331, 0, 1, 4096, 4096, 4), 'All four own workers must complete successfully')
pids = [v[12 + 3 * i] for i in range(4)]
require(len(set(pids)) == 4 and all(p > 1 for p in pids) and
        all(v[13 + 3 * i] == v[14 + 3 * i] == 0 for i in range(4)), 'Distinct actual successful posix_spawn/waitpid results')
require(v[38] & 33 == 33 and v[39] == v[41] != v[40] == v[42] and v[43:45] == (1, 1), 'Actual read-only union view, exact own RAM-volume identity and original lower file')
cache = bytes.fromhex(u['register_memory']['e:1000']['bytes'])
require(32 < v[6] <= len(cache), 'Complete cache captured within the fixed 4096-byte read bound')
cache = cache[:v[6]]
header = struct.unpack_from('<4I', cache)
require(header == tuple(v[7:11]) and header[:2] == (32, 1) and cache[16:32] == struct.pack('<2Q', *v[24:26]), 'Actual complete Vulkan cache header and UUID')
output = bytes.fromhex(u['register_memory']['f:1000']['bytes'])
require(0 < v[11] < 4095 and v[11] <= len(output), 'Complete bounded actual worker output')
output = output[:v[11]].decode('ascii')
rows = re.findall(r'^PASS: Turnip pipeline cache (produce|consume|uuid-miss|shader-miss); uuid=([0-9a-f]{32}) bytes=(\d+) strict=(\d+) feedback=0x([0-9a-f]+) strict_us=([0-9.]+) compile_us=([0-9.]+); no shader execution$', output, re.M)
require(len(rows) == 4 and [r[0] for r in rows] == ['produce', 'consume', 'uuid-miss', 'shader-miss'], 'Actual complete four-case worker ledger')
for mode, uuid, size, strict, feedback, strict_us, compile_us in rows:
    require(uuid == cache[16:32].hex() and int(size) == len(cache), 'Actual same-driver UUID/cache extent in every worker')
    require(int(strict) == (0 if mode == 'consume' else 1000297000) and
            int(feedback, 16) == (3 if mode == 'consume' else 1 if mode == 'produce' else 0) and
            (float(compile_us) > 0 if mode == 'produce' else float(compile_us) == 0), 'Actual strict hit and genuine UUID/shader miss controls')
(OUT / 'ios-pipeline-cache.bin').write_bytes(cache)
(OUT / 'worker-output.log').write_text(output)
checkpoint = dict(evidence='actual iOS runtime in offline QEMU, software transport, no shader execution',
    observation_sha256=sha(report_path), cross_build_report_sha256=sha(OUT / 'build-report.json'),
    staged_data_report_sha256=sha(OUT / 'data-report.json'), candidate_sha256=sha(kernel),
    ios_driver_input=build['ios_driver_input'], driver_code_and_build_id_unchanged=True,
    cache_sha256=hashlib.sha256(cache).hexdigest(), cache_bytes=len(cache), cache_uuid=cache[16:32].hex(),
    worker_pids=pids, cases=[dict(mode=r[0], strict_result=int(r[3]), feedback=int(r[4], 16),
        qemu_strict_us=float(r[5]), qemu_compile_us=float(r[6])) for r in rows],
    native_cpu_4k_observed=True, worker_page_abi_4k_count=4, worker_live_tcr_sampled=False,
    original_signatures_amfi_and_trust_preserved=True, readonly_own_view=True, mutable_test_backing=True,
    production_root_authorization=False, same_ios_driver_offline_cache_hit=True,
    complete_ui_cache_pack=False, host_cache_portability_qualified=False, shader_execution=False,
    physical_device_tested=False, active_native_adreno_4k=False, hardware_timing_qualified=False,
    hardware_ready=False, stable_120_fps_qualified=False)
(OUT / 'cache-checkpoint.json').write_text(json.dumps(checkpoint, indent=2) + '\n')
print('PASS: same iOS Turnip compiler, four distinct original-runtime processes, strict cache hit and UUID/shader miss controls')
print('No Adreno execution, complete UI cache pack, hardware timing or stable 120 FPS is qualified')
