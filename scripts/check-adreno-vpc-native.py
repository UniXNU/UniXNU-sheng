#!/usr/bin/env python3
"""Bind the actual native VPC reference run to its sources, image and MMU state."""
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/adreno-vpc-native-tests'
sys.path.insert(0, str(ROOT / 'kits/XnuPrelink'))
from native_prelink import macho


def require(value, why):
    if not value:
        raise SystemExit(why)


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


b = json.loads((OUT / 'build-report.json').read_text())
inputs = json.loads((OUT / 'test-inputs.json').read_text())
data = json.loads((OUT / 'data-report.json').read_text())
require(sha(OUT / 'air-native') == b['process_sha256'] == data['process_sha256']
        and sha(OUT / 'test-inputs.json') == b['manifest_sha256'] == data['manifest_sha256'],
        'Exact signed native process and complete test manifest')
require(inputs['kit_revision'] == '18a3e55192da680b108c973ecde5b8d41f608bee'
        and inputs['positive_profiles'] == 16597 and inputs['transactional_refusals'] == 36,
        'Exact reference suite and kit revision')
for name, digest in inputs['sources'].items():
    require(sha(ROOT / name) == digest, 'Actual native test and encoder sources unchanged')
require(sha(ROOT / 'kits/sheng/AdrenoVPCNativeTests.c') == b['source_sha256'],
        'Exact wrapper with active assertions and both user page-size queries')
require(sha(ROOT / 'scripts/data/adreno-vpc-inputs.json') == inputs['primary_reference_manifest_sha256']
        and json.loads((ROOT / 'scripts/data/adreno-vpc-inputs.json').read_text()) == inputs['primary_reference'],
        'Exact primary-reference manifest')
primary = ROOT / '.cache/mesa-work'
for name, digest in inputs['primary_reference']['files'].items():
    raw = subprocess.check_output(['git', '-C', str(primary), 'show',
                                  inputs['primary_reference']['mesa_revision'] + ':' + name], timeout=30)
    require(hashlib.sha256(raw).hexdigest() == digest, 'Pinned primary reference bytes')
    if (primary / name).exists():
        require(sha(primary / name) == digest, 'Actual primary header/source bytes unchanged')
for name, digest in inputs['generated_header_sha256'].items():
    require(sha(ROOT / 'build/adreno-vpc' / name) == digest, 'Actual generated primary XML headers')
require(sha(ROOT / 'build/adreno-vpc/report.json') == inputs['host_report_sha256'],
        'Same host reference qualification')
raw = (OUT / 'air-native').read_bytes()
m = macho(raw)
require(m['kind'] == 2 and [struct.unpack_from('<II', raw, p + 8)
        for cmd, p, _ in m['commands'] if cmd == 0x32] == [(2, 0x110000)],
        'Actual ordinary ARM64 executable with iOS17 platform/minimum')
details = subprocess.run(['codesign', '--display', '--verbose=4', str(OUT / 'air-native')],
                         capture_output=True, text=True, check=True, timeout=30).stderr
require(re.search(r'^Page size=4096$', details, re.M)
        and re.search(r'^CDHash=([a-f0-9]{40})$', details, re.M)[1] == b['cdhash'],
        'Actual own CodeDirectory hash and signing granule')
subprocess.run(['codesign', '--verify', '--strict', str(OUT / 'air-native')], check=True, timeout=30)
bundle = (OUT / 'offline-trust.bundle').read_bytes()
offsets = struct.unpack_from('<3I', bundle, 4)
original = (ROOT / 'build/original-static-system-trust/original-static-system.bundle').read_bytes()
old = struct.unpack_from('<2I', original, 4)
require(struct.unpack_from('<I', bundle)[0] == 3
        and bundle[offsets[1]:offsets[2]] == original[old[0]:old[1]]
        and bundle[offsets[2]:] == original[old[1]:]
        and bundle[offsets[0] + 24:offsets[0] + 44].hex() == b['cdhash'],
        'Explicit own trust and both unchanged original modules')
p = ROOT / 'build/darwinboot-owner-hvf-adreno-vpc-controls/gdb-stop.json'
d = json.loads(p.read_text())
r = {k: int(v, 16) for k, v in d['system_registers'].items()}
plan = json.loads((ROOT / 'build/xnu-prelink/native-owner-air-outer-candidate.json').read_text())
require(d['kernel_sha256'] == plan['candidate_sha256']
        == sha(ROOT / 'build/xnu-prelink/native-owner-air-outer-candidate.macho')
        and d['qemu_accelerator'] == 'hvf' and not d['bounded_timeout_interrupted'],
        'Actual bounded same-candidate native boot')
require(d['stop_symbol'] == '_exit' and d['entry_regions']['1:0:8']['bytes'] == '5d00000000000000'
        and int(d['x0'], 16) == d['actual_initproc_pointer'],
        'Real init exit(93), after exact suite totals and all live assertions')
require(d['native_4k_mmu_observed'] and (r['TCR_EL1'] >> 14) & 3 == 0
        and (r['TCR_EL1'] >> 30) & 3 == 2 and r['SCTLR_EL1'] & 1
        and r['TTBR0_EL1'] and r['TTBR1_EL1']
        and set(d['commpage_user64_shifts'].values()) == {12},
        'Actual active CPU 4K granules and userspace page-size ABI')
require(d['original_amfi_table_owner_observed'] and d['owner_root_binding_audit']['boot_root_accepted']
        and d['owner_root_binding_audit']['_native_owner_root_apple_autherr'] == 80
        and d['actual_signature_failures'] == dict(_cs_validate_page_bad_hash=0, _cs_validate_page_no_hash=0)
        and not d['native_virtio_integrity']['_native_virtio_integrity_failures'],
        'Original AMFI, Apple root refusal and owner root/signature/storage checks')
require(d['trust_cache_bundle_sha256'] == b['trust_bundle_sha256'] == sha(OUT / 'offline-trust.bundle')
        and d['scratch_data']['actual_seed_sha256'] == data['sha256'] == sha(OUT / 'compiler-data-32.cdr'),
        'Exact actual own trust and Data inputs')
(OUT / 'native-checkpoint.json').write_text(json.dumps(dict(
    process_sha256=b['process_sha256'], candidate_sha256=d['kernel_sha256'],
    checkpoint_sha256=sha(p), actual_exit_status=93, native_user_page_bytes=4096,
    actual_tcr=r['TCR_EL1'], kit_revision=inputs['kit_revision'],
    positive_profiles=16597, transactional_refusals=36, native_assertions_active=True,
    actual_primary_reference_helpers=True, original_amfi_and_root_preserved=True,
    gpu_execution=False, active_gpu_smmu_qualified=False,
    complete_pipeline_state=False, metal_device=False, original_desktop=False), indent=2) + '\n')
print('PASS: native-4K iOS17 VPC encoder matches actual primary helpers across 16597 profiles/36 refusals')
print('Original AMFI/root preserved; GPU execution, active GPU SMMU and complete pipeline remain unqualified')
