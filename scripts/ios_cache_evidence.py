#!/usr/bin/env python3
"""Shared immutable-input, original-signing/root and native-CPU4K observation checks.

Own offline cache instruments only. This module never starts a VM or hardware,
mounts images, authorizes production roots, or qualifies physical GPU execution.
"""
import hashlib
import json
from pathlib import Path
import plistlib
import re
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'kits/XnuPrelink'))
from native_prelink import macho, backed_offset


def require(value, reason):
    if not value:
        raise SystemExit(reason)


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def load(path):
    return json.loads(path.read_text())


def validate(stage_dir, observation):
    OUT = stage_dir.resolve()
    stage_path = OUT / 'stage-report.json'
    stage = load(stage_path)
    require(sha(ROOT / stage.get('stage_script_relative', 'scripts/stage-quartzcore-ios-compiler-peer.py')) == stage['stage_script_sha256'] and
            sha(ROOT / stage.get('parent_source_relative', 'drivers/AdrenoMetal/bridge/quartzcore_cache_ios.cpp')) == stage['parent_source_sha256'], 'Exact staging/compiled parent source')
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
    observation = observation.resolve()
    report_path = observation / 'gdb-stop.json'
    d = load(report_path)
    require(d['kernel_sha256'] == plan['candidate_sha256'] and not d['bounded_timeout_interrupted'] and d['stop_symbol'] == '_exception_triage' and
            d['native_4k_mmu_observed'] and d['original_amfi_table_owner_observed'] and d['owner_root_binding_audit']['boot_root_accepted'] and
            d['actual_signature_failures'] == dict(_cs_validate_page_bad_hash=0, _cs_validate_page_no_hash=0) and
            d['actual_process']['pid'] == 1 and d['actual_process']['name'] == 'air-native' and
            d['scratch_data']['actual_seed_sha256'] == stage['data_sha256'] and d['trust_cache_bundle_sha256'] == stage['trust_bundle_sha256'] and
            d['qemu_accelerator'] == 'hvf' and not d['sheng_hardware_tested'], 'Actual bounded QEMU/native CPU4K/original-runtime/signature/root state')
    for name, digest in stage.get('runtime_patches_sha256', {}).items():
        require(sha(ROOT / 'drivers/AdrenoMetal/bridge/patches' / name) == digest, 'Exact compiled runtime patch: ' + name)
    return dict(stage=stage, observation=d, user=d['entry_user_state_candidates'][0],
                stage_report_sha256=sha(stage_path), observation_sha256=sha(report_path), candidate_sha256=sha(kernel))
