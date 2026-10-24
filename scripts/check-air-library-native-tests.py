#!/usr/bin/env python3
"""Qualify actual native original-libc container refusal and guard execution."""
import hashlib
import json
from pathlib import Path
import struct
import sys
ROOT=Path(__file__).resolve().parents[1];OUT=ROOT/'build/air-library-native-tests'
sys.path.insert(0,str(ROOT/'kits/XnuPrelink'))
from native_prelink import macho
def require(v,why):
 if not v:raise SystemExit(why)
def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
b=json.loads((OUT/'build-report.json').read_text());inputs=json.loads((OUT/'test-inputs.json').read_text());data=json.loads((OUT/'data-report.json').read_text())
require(sha(OUT/'air-native')==b['process_sha256']==data['process_sha256'] and sha(OUT/'test-inputs.json')==b['manifest_sha256']==data['manifest_sha256'],'Exact signed native test and complete source/input graph')
require(inputs['kit_revision']=='da7f99e572aca992f5fa1e6b0025c86c622d96c5' and inputs['positive_profiles']==12 and inputs['transactional_refusals']==4456,'Exact qualified generic suite')
for name,digest in inputs['sources'].items():require(sha(ROOT/name)==digest,'Native test/reader sources unchanged')
require(sha(ROOT/'kits/sheng/AirLibraryNativeTests.c')==b['source_sha256'],'Exact active-assertion native wrapper')
raw=(OUT/'air-native').read_bytes();m=macho(raw)
require(m['kind']==2 and [struct.unpack_from('<II',raw,p+8) for cmd,p,_ in m['commands'] if cmd==0x32]==[(2,0x110000)] and b['code_signature_page_bytes']==4096,'Actual ordinary ARM64/iOS17 image and own 4K CodeDirectory')
bundle=(OUT/'offline-trust.bundle').read_bytes();offsets=struct.unpack_from('<3I',bundle,4);original=(ROOT/'build/original-static-system-trust/original-static-system.bundle').read_bytes();old=struct.unpack_from('<2I',original,4)
require(struct.unpack_from('<I',bundle)[0]==3 and bundle[offsets[1]:offsets[2]]==original[old[0]:old[1]] and bundle[offsets[2]:]==original[old[1]:] and bundle[offsets[0]+24:offsets[0]+44].hex()==b['cdhash'],'Explicit own trust; both original trust modules unchanged')
p=ROOT/'build/darwinboot-owner-hvf-air-library-controls/gdb-stop.json';d=json.loads(p.read_text());r={k:int(v,16) for k,v in d['system_registers'].items()};plan=json.loads((ROOT/'build/xnu-prelink/native-owner-air-outer-candidate.json').read_text())
require(d['kernel_sha256']==plan['candidate_sha256']==sha(ROOT/'build/xnu-prelink/native-owner-air-outer-candidate.macho') and d['qemu_accelerator']=='hvf' and not d['bounded_timeout_interrupted'],'Actual bounded same-candidate native boot')
require(d['stop_symbol']=='_exit' and d['entry_regions']['1:0:8']['bytes']=='5a00000000000000' and int(d['x0'],16)==d['actual_initproc_pointer'],'Actual init exit(90), after live assertions and exact suite counts')
require(d['native_4k_mmu_observed'] and (r['TCR_EL1']>>14)&3==0 and (r['TCR_EL1']>>30)&3==2 and r['SCTLR_EL1']&1 and r['TTBR0_EL1'] and r['TTBR1_EL1'] and set(d['commpage_user64_shifts'].values())=={12},'Actual active hardware granules and user ABI 4K')
require(d['original_amfi_table_owner_observed'] and d['owner_root_binding_audit']['boot_root_accepted'] and d['owner_root_binding_audit']['_native_owner_root_apple_autherr']==80 and d['actual_signature_failures']==dict(_cs_validate_page_bad_hash=0,_cs_validate_page_no_hash=0) and not d['native_virtio_integrity']['_native_virtio_integrity_failures'],'Genuine original AMFI/root/signature/storage checks')
require(d['trust_cache_bundle_sha256']==sha(OUT/'offline-trust.bundle') and d['scratch_data']['actual_seed_sha256']==data['sha256']==sha(OUT/'compiler-data-32.cdr'),'Actual precise own trust/Data inputs')
(OUT/'native-checkpoint.json').write_text(json.dumps(dict(process_sha256=b['process_sha256'],candidate_sha256=d['kernel_sha256'],checkpoint_sha256=sha(p),
 actual_exit_status=90,native_user_page_bytes=4096,actual_tcr=r['TCR_EL1'],positive_profiles=12,transactional_refusals=4456,
 original_library_functions=240,actual_readonly_and_guard_vm_calls_passed=True,independent_user_pte_guard_walk=False,
 native_assertions_active=True,original_amfi_and_root_preserved=True,gpu_execution=False,metal_device=False,original_desktop=False),indent=2)+'\n')
print('PASS: 12 profiles, 4456 transactional refusals and both original readonly/guarded libraries under actual native-4K iOS17 libc')
print('Preserved genuine AMFI/owner-root; user guard PTEs were not independently walked')
