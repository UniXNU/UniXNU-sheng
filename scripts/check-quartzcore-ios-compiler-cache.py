#!/usr/bin/env python3
"""Verify actual iOS UI compiler/cache results and export the exact compiled pack.

Inspect only a readonly snapshot of the own mutable Data fixture. Match every
exported cache byte against the paused iOS parent's independent manifest.
This qualifies no shader execution, production authenticated addon or deadline.
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
import sys

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/quartzcore-ios-compiler-peer'
sys.path.insert(0, str(ROOT / 'kits/XnuPrelink'))
from native_prelink import macho, backed_offset
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--observation', type=Path, required=True)
parser.add_argument('--export', type=Path, required=True, help='Fresh directory for exact cache bytes and archive')
parser.add_argument('--stage-dir', type=Path, default=OUT, help='Own staged input directory; default is the producer fixture')
args = parser.parse_args()
OUT = args.stage_dir.resolve()
work = args.export.resolve()


def require(value, reason):
    if not value:
        raise SystemExit(reason)


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def load(path):
    return json.loads(path.read_text())


require(not work.exists(), 'Preserve the existing exported pack; use a fresh directory')
require(shutil.disk_usage(ROOT).free > 5 * 1024**3 + 256 * 1024**2, 'Preserve 5 GiB and bounded extraction allowance')
stage_path = OUT / 'stage-report.json'
stage = load(stage_path)
require(sha(ROOT / stage.get('stage_script_relative', 'scripts/stage-quartzcore-ios-compiler-peer.py')) == stage['stage_script_sha256'] and
        sha(ROOT / stage.get('parent_source_relative', 'drivers/AdrenoMetal/bridge/quartzcore_cache_ios.cpp')) == stage['parent_source_sha256'], 'Exact staging/compiled parent source')
first_only = stage.get('first_consumer_only', False)
child_count = 3 if first_only else 4
for name, digest in stage.get('parent_source_dependencies', {}).items():
    require(sha(ROOT / name) == digest, 'Exact shared parent source: ' + name)
for name, digest in stage['qualified_host_sources'].items():
    require(sha(ROOT / name) == digest, 'Exact cross-linked source: ' + name)
for name, info in stage['outputs'].items():
    path = OUT / name
    require(sha(path) == info['sha256'], 'Exact signed own executable: ' + name)
    raw = path.read_bytes()
    require(struct.unpack_from('<3I', raw) == (0xfeedfacf, 0x100000c, 0), 'Actual ordinary ARM64 subtype 0')
    subprocess.run(['codesign', '--verify', '--strict', path], capture_output=True, check=True, timeout=30)
    detail = subprocess.run(['codesign', '--display', '--verbose=4', path], capture_output=True, text=True, check=True, timeout=30).stderr
    require(info['cdhash'] in detail and re.search(r'^Page size=4096$', detail, re.M), 'Actual own 4K CodeDirectory')
    if name in ('air-native', 'compiler-worker'):
        actual = plistlib.loads(subprocess.check_output(['codesign', '-d', '--entitlements', ':-', str(path)], stderr=subprocess.DEVNULL, timeout=30))
        require(actual == stage['own_parent_permissions'] == {'com.apple.private.security.no-container': True,
                'com.apple.private.bindfs-allow': True}, 'Exact own helper permissions; no no-sandbox')
require(sha(OUT / 'ui-compiler-data-64.cdr') == stage['data_sha256'] and sha(OUT / 'offline-trust.bundle') == stage['trust_bundle_sha256'],
        'Exact original staged own fixture and trust input')
driver = ROOT / 'build/mesa-tu-ios/src/freedreno/vulkan/libvulkan_freedreno.dylib'
require(sha(driver) == stage['driver_input_sha256'], 'Exact actual iOS compiler input')
for path in (driver, OUT / driver.name):
    raw = path.read_bytes()
    image = macho(raw)
    actual = {s['name'] + '.' + t['name']: hashlib.sha256(raw[t['fileoff']:t['fileoff'] + t['size']]).hexdigest()
              for s in image['segments'] for t in s['sections'] if t['fileoff']}
    ids = [raw[at + 8:at + 24].hex() for command, at, size in image['commands'] if command == 0x1b and size == 24]
    platforms = [struct.unpack_from('<I', raw, at + 8)[0] for command, at, _ in image['commands'] if command == 0x32]
    require(actual == stage['driver_identity']['file_backed_sections'] and ids == [stage['driver_identity']['macho_uuid']] and platforms == [2],
            'Every original driver section/platform/build ID preserved by own signing')
trust = (OUT / 'offline-trust.bundle').read_bytes()
require(struct.unpack_from('<I', trust)[0] == 3, 'Own and both complete original trust modules')
offsets = struct.unpack_from('<3I', trust, 4)
modules = [trust[a:b] for a, b in zip(offsets, (*offsets[1:], len(trust)))]
require([hashlib.sha256(x).hexdigest() for x in modules[1:]] == [
    'da88460e6e513438d5307e5c526f76497a74754476a24da29f239f0c90120fc2',
    'd121ba89167e03aafd00fa687c30cd8ebc4ccafd10c1408adb2ba3de12403651'], 'Both complete original trust modules unchanged')
hashes = sorted(v['cdhash'] for v in stage['outputs'].values())
require(struct.unpack_from('<I', modules[0], 20)[0] == len(hashes) and len(modules[0]) == 24 + 24 * len(hashes) and
        [modules[0][i:i + 20].hex() for i in range(24, len(modules[0]), 24)] == hashes and
        all(modules[0][i + 20:i + 24] == bytes([2, 0, 0, 0]) for i in range(24, len(modules[0]), 24)), 'Exact five own signed artifacts')
base = ROOT / 'build/xnu-prelink/native-owner-turnip-compiler-qemu-candidate'
plan, kernel = load(base.with_suffix('.json')), base.with_suffix('.macho')
require(sha(kernel) == plan['candidate_sha256'] and plan['original_sha256'] == 'f18b4df865405f2a287b84977d7aca745e056d656d9fc9df1662970ce2969de7',
        'Exact QEMU candidate and supplied original ordinary ARM64 kernel')
raw, image = kernel.read_bytes(), macho(kernel.read_bytes())
for name, digest in plan['original_executable_sha256'].items():
    extent = next(e for e in plan['extents'] if e['module'] == name and e['segment'] == '__TEXT_EXEC')
    at = backed_offset(image['segments'], extent['va'], extent['size'], len(raw))
    require(hashlib.sha256(raw[at:at + extent['size']]).hexdigest() == digest, 'Whole original executable preserved: ' + name)
observation = args.observation.resolve()
report_path = observation / 'gdb-stop.json'
d = load(report_path)
require(d['kernel_sha256'] == plan['candidate_sha256'] and not d['bounded_timeout_interrupted'] and d['stop_symbol'] == '_exception_triage' and
        d['native_4k_mmu_observed'] and d['original_amfi_table_owner_observed'] and d['owner_root_binding_audit']['boot_root_accepted'] and
        d['actual_signature_failures'] == dict(_cs_validate_page_bad_hash=0, _cs_validate_page_no_hash=0) and
        d['actual_process']['pid'] == 1 and d['actual_process']['name'] == 'air-native' and
        d['scratch_data']['actual_seed_sha256'] == stage['data_sha256'] and d['trust_cache_bundle_sha256'] == stage['trust_bundle_sha256'] and
        d['qemu_accelerator'] == 'hvf' and not d['sheng_hardware_tested'], 'Actual bounded QEMU/native CPU4K/original-runtime/signature/root state')
u = d['entry_user_state_candidates'][0]
require(int(u['cpsr'], 16) & 31 == 0 and u['instruction_bytes'].startswith('000020d4'), 'Actual own EL0 checkpoint')
memory = u['register_memory']
parent_memory = bytes.fromhex(memory['d:180']['bytes'])
require(len(parent_memory) == 384, 'Complete bounded parent ledger')
v = struct.unpack('<48Q', parent_memory)
require(v[:6] == ((0x515443504b494f32 if first_only else 0x515443504b494f31), 0, 1, 4096, 4096, child_count) and
        v[6] == (384 if first_only else 128) and v[7] == 9670832 and v[45] == (2 if first_only else 1) and v[47] == 1,
        'Actual own parent mode, complete bounded manifest and post-sync checkpoint')
pids = [v[12 + 3 * i] for i in range(child_count)]
require(len(set(pids)) == child_count and all(p > 1 for p in pids) and all(v[13 + 3 * i] == v[14 + 3 * i] == 0 for i in range(child_count)) and
        (not first_only or v[21:24] == (0, 0, 0)), 'Distinct actual successful original-runtime children with real 4K ABI; no hidden producer slot')
require(v[38] & 33 == 33 and v[39] == v[41] != v[40] == v[42] and v[43:45] == (1, 1), 'Actual readonly union view, exact RAM-volume identity and original lower file')
summary_memory = bytes.fromhex(memory['e:1000']['bytes'])
require(len(summary_memory) >= v[6], 'Complete bounded manifest capture')
summary = summary_memory[:128]
words = struct.unpack('<16Q', summary)
require(words[0] == 0x515455495041434b and words[1] == v[10] and words[6] == v[8] and words[7] == v[9] and
        summary[96:128].hex() == '606f913adf520420f9b4bb666efb782a68c830f082f1e559e17b20d0d13adbc9', 'Actual source/pack manifest in the paused iOS parent')
if first_only:
    initial_report_path = ROOT / stage['initial_pack_report_relative']
    require(sha(initial_report_path) == stage['initial_pack_report_sha256'], 'Exact earlier actual-iOS exported pack provenance')
    initial = load(initial_report_path)
    require(summary_memory[256:384] == summary and v[46] == 1 and
            summary[16:48].hex() == stage['initial_archive_sha256'] == initial['archive_sha256'] and
            summary[64:96].hex() == stage['initial_cache_manifest_sha256'] == initial['cache_manifest_sha256'] and
            words[6] == stage['initial_cache_file_count'] == initial['cache_file_count'] and
            words[7] == stage['initial_cache_bytes'] == initial['cache_bytes'] and stage['producer_runs_in_boot'] == 0,
            'Actual initial-image manifest before the first worker equals the final bytes and exact native producer pack')
require(0 < v[11] < 4095 and v[11] <= len(bytes.fromhex(memory['f:1000']['bytes'])), 'Complete bounded worker log, no truncation')
output = bytes.fromhex(memory['f:1000']['bytes'])[:v[11]].decode('ascii')
pattern = re.compile(r'^PASS: QuartzCore compiler (\S+) pid=(\d+) recipes=(\d+) calls=(\d+) success=(\d+) required=(\d+) valid=(\d+) hits=(\d+) uuid_mutations=(\d+) devices=(\d+) artifacts=(\d+)/(\d+)/(\d+) driver=(\d+)/(\d+)/(\d+)/(\d+) cached_calls=(\d+)/(\d+)/(\d+) device_graphics=(\d+) cold_required=(\d+) cold_object=(\d+) init_submissions=(\d+); no shader execution$', re.M)
rows = pattern.findall(output)
require(len(rows) == child_count and [r[0] for r in rows] == (['consume', 'uuid-miss', 'empty-driver'] if first_only else ['produce', 'consume', 'uuid-miss', 'empty-driver']), 'Actual complete independent compiler-case ledger')
fields = ['pid', 'recipes', 'calls', 'success', 'required', 'valid', 'hits', 'uuid_mutations', 'devices', 'artifact_hits', 'artifact_misses',
          'artifact_writes', 'driver_loads', 'runtime_driver_hits', 'runtime_driver_misses', 'driver_saves', 'cached_calls', 'cached_valid',
          'cached_hits', 'device_graphics', 'cold_required', 'cold_object', 'initialization_submissions']
cases = {row[0]: dict(zip(fields, map(int, row[1:]))) for row in rows}
require([x['pid'] for x in cases.values()] == pids and all(x['devices'] == 1 for x in cases.values()), 'Actual case/PID/device correspondence')
c, wrong, empty = (cases[key] for key in ['consume', 'uuid-miss', 'empty-driver'])
if not first_only:
    p = cases['produce']
    require(p['recipes'] == c['recipes'] == 24 and p['cold_required'] > 0 and p['cold_object'] == 1 and p['artifact_misses'] == p['artifact_writes'] == 72 and p['driver_saves'] == 33,
            'Genuine target-build translation/IR3 production and state-object versus driver cold-refusal control')
else:
    require(c['recipes'] == 24 and c['cold_required'] == c['cold_object'] == 0 and c['pid'] == pids[0], 'The first actual child is the strict consumer; no cold producer ran')
require(c['calls'] == c['success'] == c['valid'] == 65 and c['cached_calls'] == c['cached_valid'] == c['cached_hits'] == c['driver_loads'] == 33 and
        c['required'] == c['artifact_misses'] == c['artifact_writes'] == c['driver_saves'] == 0, 'All actual UI creations succeed with compilation forbidden and real application hits')
require(all(x['recipes'] == 0 and x['calls'] == 2 and x['success'] == x['required'] == 1 and x['cached_hits'] == 0 for x in (wrong, empty)) and
        wrong['uuid_mutations'] > 0 and empty['driver_loads'] == 0, 'Genuine target-build UUID/empty-driver refusals')
capture = d['scratch_data_snapshot']
snapshot = observation / capture['file']
require(capture['file'] == 'scratch-data-snapshot.apfs' and capture['bytes'] == stage['data_bytes'] == 64 * 1024**2 and capture['read_only'] and
        sha(snapshot) == capture['sha256'], 'Exact bounded actual paused Data export')
work.mkdir(parents=True)
mount = OUT / 'snapshot-mount'
mount.mkdir(exist_ok=True)
attached = plistlib.loads(subprocess.check_output(['hdiutil', 'attach', '-readonly', '-imagekey', 'diskimage-class=CRawDiskImage', '-nobrowse', '-owners', 'off', '-mountpoint', str(mount), '-plist', str(snapshot)], timeout=30))
physical = attached['system-entities'][0]['dev-entry']
manifest, driver_records = {}, []
try:
    info = plistlib.loads(subprocess.check_output(['diskutil', 'info', '-plist', str(mount)], timeout=10))
    require(info['VolumeUUID'] == stage['data_volume_uuid'] and info['BusProtocol'] == 'Disk Image', 'Actual exact own exported volume')
    directory, archive = mount / 'tmp/quartzcore-ui.cache', mount / 'tmp/quartzcore-ui.archive'
    require(archive.stat().st_size == words[1] and sha(archive) == summary[16:48].hex(), 'Exported archive exactly matches actual iOS parent digest')
    files = sorted(directory.rglob('*'))
    digest = hashlib.sha256()
    for path in files:
        require(not path.is_symlink(), 'No cache symlink in the owned export')
        if path.is_dir():
            continue
        name = str(path.relative_to(directory))
        require(path.is_file() and path.stat().st_size <= 16 * 1024**2 and len(manifest) < 2048 and
                len(name.encode('ascii')) <= 192 and '..' not in name, 'Bounded regular cache file')
        data = path.read_bytes()
        manifest[name] = dict(bytes=len(data), sha256=hashlib.sha256(data).hexdigest())
        digest.update(struct.pack('<I', len(name)));digest.update(name.encode('ascii'));digest.update(struct.pack('<Q', len(data)));digest.update(hashlib.sha256(data).digest())
        if data.startswith(b'M2VPPLC\0'):
            require(len(data) >= 108 and hashlib.sha256(data[:-32]).digest() == data[-32:], 'Actual driver record checksum')
            length = struct.unpack_from('<Q', data, 36)[0]
            require(len(data) == 44 + length + 32 and struct.unpack_from('<2I', data, 44) == (32, 1) and
                    data[8:16] == data[52:60] and data[20:36] == data[60:76], 'Actual complete original driver cache header and identity')
            driver_records.append(dict(path=name, cache_uuid=data[20:36].hex(), vendor_id=struct.unpack_from('<I', data, 8)[0],
                device_id=struct.unpack_from('<I', data, 12)[0], driver_version=struct.unpack_from('<I', data, 16)[0], vulkan_cache_bytes=length))
    require(len(manifest) == words[6] <= 2048 and sum(x['bytes'] for x in manifest.values()) == words[7] <= 64 * 1024**2 and
            digest.digest() == summary[64:96], 'Every exported producer cache byte matches the actual iOS manifest')
    require(len(driver_records) == 33 and len({x['cache_uuid'] for x in driver_records}) == 1, 'All 33 target-build driver cache records have one actual UUID')
    shutil.copyfile(archive, work / 'ui.archive')
    shutil.copytree(directory, work / 'ui.cache')
finally:
    subprocess.run(['hdiutil', 'detach', physical], check=True, capture_output=True, timeout=30)
require(sha(snapshot) == capture['sha256'], 'The actual Data snapshot remains unchanged after readonly extraction')
(work / 'worker-output.log').write_text(output)
checkpoint = dict(evidence='actual iOS original runtime in offline QEMU, real Turnip/IR3 compilation and cache import; software peer executes no shader',
    checker_sha256=sha(Path(__file__)), stage_report_sha256=sha(stage_path), observation_sha256=sha(report_path), snapshot_sha256=capture['sha256'],
    candidate_sha256=sha(kernel), ios_driver_input_sha256=stage['driver_input_sha256'], ios_driver_uuid=stage['driver_identity']['macho_uuid'],
    original_library_sha256=summary[96:128].hex(), archive_bytes=words[1], archive_sha256=summary[16:48].hex(),
    cache_file_count=words[6], cache_bytes=words[7], cache_manifest_sha256=summary[64:96].hex(), cache_manifest=manifest, driver_records=driver_records,
    actual_pipeline_cache_uuid=driver_records[0]['cache_uuid'], cases=cases, worker_pids=pids,
    actual_native_cpu_4k=True, worker_page_abi_4k_count=child_count, worker_live_tcr_sampled=False, original_signature_amfi_trust_preserved=True,
    readonly_own_extension_view=True, mutable_test_backing=True, parent_called_sync=True, whole_snapshot_fs_consistency_qualified=False,
    extracted_pack_matches_guest_manifest=True, strict_ui_compiler_scope='after device initialization, authored 24 recipes; not every draw variant',
    production_root_authorization=False, selected_recipes_initial_image_consumer_qualified=first_only, producer_runs_in_boot=0 if first_only else 1,
    initial_image_provisioning_qualified=False, complete_ui_variant_coverage=False, shader_execution=False,
    physical_device_tested=False, active_native_adreno_4k=False, hardware_timing_qualified=False, hardware_ready=False, stable_120_fps_qualified=False)
(work / 'checkpoint.json').write_text(json.dumps(checkpoint, indent=2) + '\n')
(OUT / 'ui-cache-checkpoint.json').write_text(json.dumps(checkpoint, indent=2) + '\n')
print('PASS: actual iOS processes, 65 compile-forbidden graphics successes, 33 real shader application-cache hits and UUID/empty-driver refusals')
if first_only:
    print('PASS: first consumer uses the exact initial-image cache bytes; no producer ran in this boot and every cache byte is unchanged')
print('PASS: complete selected-recipe archive and all 153 producer cache files exported byte-for-byte against the paused iOS manifest')
print('No complete UI initial-image pack, authenticated production packaging, Adreno shader execution or stable 120 FPS is qualified')
