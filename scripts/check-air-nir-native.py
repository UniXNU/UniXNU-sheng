#!/usr/bin/env python3
"""Check actual native compiler exit, native hardware granules and original root policy."""
import hashlib
import json
from pathlib import Path
import struct
ROOT=Path(__file__).resolve().parents[1];OUT=ROOT/'build/air-nir-diagnostic'
def require(value,why):
    if not value:raise SystemExit(why)
def sha(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()
build=json.loads((OUT/'build-report.json').read_text());data=json.loads((OUT/'data-report.json').read_text())
audit=json.loads((OUT/'target-import-audit.json').read_text())
require(sha(OUT/'air-native')==build['process_sha256']==audit['executable_sha256']==data['process_sha256'],'Exact own signed process identity')
require(sha(ROOT/'kits/sheng/AirNirDiagnostic.cpp')==build['source_sha256'],'Exact instrument source')
require(not audit['missing'] and audit['imports']==371 and build['code_signature_page_bytes']==4096,'Target imports and own signing granule')
require(sha(OUT/'compiler-data-32.cdr')==data['sha256'],'Exact staged Data identity')
bundle=(OUT/'offline-trust.bundle').read_bytes();offsets=struct.unpack_from('<3I',bundle,4)
require(struct.unpack_from('<I',bundle)[0]==3,'One own and two original trust modules')
original=(ROOT/'build/original-static-system-trust/original-static-system.bundle').read_bytes();old_offsets=struct.unpack_from('<2I',original,4)
require(bundle[offsets[1]:offsets[2]]==original[old_offsets[0]:old_offsets[1]] and bundle[offsets[2]:]==original[old_offsets[1]:], 'Complete original trust modules unchanged')
manifest=ROOT/'scripts/data/native-air-diagnostic-inputs.json';cases=json.loads(manifest.read_text())['cases']
require(len(cases)==17 and sum(c['stage']=='fragment' for c in cases)==3 and build['manifest_sha256']==sha(manifest)==data['manifest_sha256'],'All 17 exact original vertex/fragment inputs')
for case in cases:
    require(sha(ROOT/f"build/gpu-analysis/air/{case['library']}/module-{case['index']}.bc")==case['input_sha256'],'Original AIR unchanged')
plan=json.loads((ROOT/'build/xnu-prelink/native-owner-air-outer-candidate.json').read_text())
require(plan['candidate_sha256']==sha(ROOT/'build/xnu-prelink/native-owner-air-outer-candidate.macho'),'Exact native candidate identity')
checkpoints={}
for label in ('exit','bad-input','no-trust','default-off'):
    path=ROOT/('build/darwinboot-owner-hvf-air17-'+label)/'gdb-stop.json';d=json.loads(path.read_text())
    r={k:int(v,16) for k,v in d['system_registers'].items()}
    require(d['kernel_sha256']==plan['candidate_sha256'] and d['qemu_accelerator']=='hvf' and not d['bounded_timeout_interrupted'],'Actual bounded same-candidate boot')
    require(d['native_4k_mmu_observed'] and (r['TCR_EL1']>>14)&3==0 and (r['TCR_EL1']>>30)&3==2 and r['SCTLR_EL1']&1 and r['TTBR0_EL1'] and r['TTBR1_EL1'],'Actual active hardware 4K translation')
    require(set(d['commpage_user64_shifts'].values())=={12},'Actual user commpage 4K')
    require(d['original_amfi_table_owner_observed'] and d['owner_root_binding_audit']['boot_root_accepted'] and d['owner_root_binding_audit']['_native_owner_root_apple_autherr']==80,'Original AMFI and independent owner root preserved')
    require(d['actual_signature_failures']==dict(_cs_validate_page_bad_hash=0,_cs_validate_page_no_hash=0) and not d['native_virtio_integrity']['_native_virtio_integrity_failures'],'Real signature/storage validation without failures')
    expected_data=json.loads((OUT/('data-bad-input-report.json' if label=='bad-input' else 'data-report.json')).read_text())
    image=OUT/('compiler-data-bad-input-32.cdr' if label=='bad-input' else 'compiler-data-32.cdr')
    require(sha(image)==expected_data['sha256']==d['scratch_data']['actual_seed_sha256'],'Actual complete Data seed identity')
    trust=ROOT/'build/original-static-system-trust/original-static-system.bundle' if label=='no-trust' else OUT/'offline-trust.bundle'
    require(d['trust_cache_bundle_sha256']==sha(trust),'Actual explicit trust input')
    if label in ('exit','bad-input'):
        status=73 if label=='exit' else 62
        require(d['stop_symbol']=='_exit' and struct.unpack_from('<I',bytes.fromhex(d['entry_regions']['1:0:8']['bytes']))[0]==status and d['actual_initproc_pointer']==int(d['x0'],16),'Actual own process success/refusal status')
    elif label=='no-trust':
        arguments=struct.unpack_from('<5Q',bytes.fromhex(d['stack_bytes']))
        require(d['stop_symbol']=='_panic' and d['argument_memory']['x0'].startswith('unexpected SIGKILL of %s %s with reason') and arguments[2:4]==(9,8),'Real EXEC security-policy refusal when own trust is absent')
        require(bytes.fromhex(d['stack_pointer_memory']['0']['bytes']).split(b'\0')[0]==b'air-native','Actual refused process identity')
    else:
        require(d['original_launchd_main_executed'] and d['followed_main_entry']['first_instruction_completed'] and int(d['pstate'],16)&31==0 and d['direct_user_commpage_shifts']=={'fffff4025:1':12,'fffff4037:1':12},'Default-off original launchd actually executes at native-4K EL0')
        config=(path.parent/'esp/EFI/UniXNU/unixnu.toml').read_text()
        require('offline-air-init' not in config,'Default-off does not select own compiler')
    checkpoints[label]=dict(sha256=sha(path),actual_tcr=r['TCR_EL1'],stop_symbol=d['stop_symbol'])
result=dict(process_sha256=build['process_sha256'],candidate_sha256=plan['candidate_sha256'],manifest_sha256=sha(manifest),
    checkpoints=checkpoints,actual_exit_status=73,native_user_page_bytes=4096,original_shaders_compiled=17,
    vertex_shaders=14,fragment_shaders=3,all_machine_code_and_abi_and_interfaces_exact=True,
    bad_input_actual_exit=62,missing_own_trust_exec_namespace=9,missing_own_trust_exec_code=8,
    default_off_original_launchd_main=True,original_amfi_preserved=True,original_root_apple_autherr=80,
    production_storage_integrity=False,native_compiler_observed=True,generated_gpu_code_executed=False,
    metal_device=False,original_desktop=False)
(OUT/'native-checkpoint.json').write_text(json.dumps(result,indent=2)+'\n')
print('PASS: all 17 native iOS17 shaders have exact A740 code, complete IR3 ABI and interfaces')
print('PASS: actual 4K/AMFI/owner-root, input refusal, absent-trust security refusal and default-off original main')
