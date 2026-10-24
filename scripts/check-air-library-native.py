#!/usr/bin/env python3
"""Check actual direct original-system metallib compilation under native-4K XNU."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys
ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--fragment-inputs',action='store_true');args=p.parse_args()
OUT=ROOT/('build/air-fragment-inputs-diagnostic' if args.fragment_inputs else 'build/air-library-diagnostic')
manifest=ROOT/('scripts/data/native-air-fragment-inputs.json' if args.fragment_inputs else 'scripts/data/native-air-diagnostic-inputs.json')
metadata=json.loads(manifest.read_text());count=len(metadata['cases'])
sys.path.insert(0,str(ROOT/'kits/XnuPrelink'))
from native_prelink import macho
def require(v,why):
 if not v:raise SystemExit(why)
def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
b=json.loads((OUT/'build-report.json').read_text());data=json.loads((OUT/'data-report.json').read_text())
require(sha(OUT/'air-native')==b['process_sha256']==data['process_sha256'] and b['direct_original_library_input'],'Actual distinct direct-library process')
require(sha(ROOT/'kits/sheng/AirLibraryDiagnostic.cpp')==b['source_sha256'] and sha(manifest)==b['manifest_sha256']==data['manifest_sha256'],'Exact source and shader manifest')
require(b['native_compiler_build_report_sha256']==sha(OUT/'native-compiler-build-report.json'),'Exact linked native compiler build inputs')
compiler=json.loads((OUT/'native-compiler-build-report.json').read_text())
import subprocess
for name,digest in compiler['kit_sources'].items():
 recorded=subprocess.check_output(['git','-C',ROOT/'kits/AirNir','show',b['kit_revision']+':'+name])
 require(hashlib.sha256(recorded).hexdigest()==digest,'Exact recorded native compiler/reader source graph')
require(b['kit_revision']==metadata.get('kit_revision','da7f99e572aca992f5fa1e6b0025c86c622d96c5'),'Exact source-qualified AirNir revision')
expected={'QuartzCore':'606f913adf520420f9b4bb666efb782a68c830f082f1e559e17b20d0d13adbc9','RenderBox':'d76883f626b1527c36fb0940cf27dcade503a55cf2fa05c8f7a247a40ccebec4'}
require({x['name']:x['sha256'] for x in b['original_libraries']}==expected,'Both exact complete original libraries, not extracted bitcode')
raw=(OUT/'air-native').read_bytes();m=macho(raw)
require(m['kind']==2 and [struct.unpack_from('<II',raw,p+8) for cmd,p,_ in m['commands'] if cmd==0x32]==[(2,0x110000)] and b['code_signature_page_bytes']==4096,'Actual ordinary ARM64/iOS17 image with own 4K CodeDirectory')
bundle=(OUT/'offline-trust.bundle').read_bytes();offsets=struct.unpack_from('<3I',bundle,4);original=(ROOT/'build/original-static-system-trust/original-static-system.bundle').read_bytes();old=struct.unpack_from('<2I',original,4)
require(struct.unpack_from('<I',bundle)[0]==3 and bundle[offsets[1]:offsets[2]]==original[old[0]:old[1]] and bundle[offsets[2]:]==original[old[1]:] and bundle[offsets[0]+24:offsets[0]+44].hex()==b['cdhash'],'Own explicit trust plus complete unchanged original modules')
p=ROOT/('build/darwinboot-owner-hvf-air-fragment-inputs-exit/gdb-stop.json' if args.fragment_inputs else 'build/darwinboot-owner-hvf-air-library-exit/gdb-stop.json');d=json.loads(p.read_text());r={k:int(v,16) for k,v in d['system_registers'].items()}
plan=json.loads((ROOT/'build/xnu-prelink/native-owner-air-outer-candidate.json').read_text())
require(d['kernel_sha256']==plan['candidate_sha256']==sha(ROOT/'build/xnu-prelink/native-owner-air-outer-candidate.macho') and d['qemu_accelerator']=='hvf' and not d['bounded_timeout_interrupted'],'Actual bounded exact native candidate')
require(d['stop_symbol']=='_exit' and d['entry_regions']['1:0:8']['bytes']=='4900000000000000' and int(d['x0'],16)==d['actual_initproc_pointer'],'Actual init exit(73) after complete libraries/17 code/ABI/interface checks')
require(d['native_4k_mmu_observed'] and (r['TCR_EL1']>>14)&3==0 and (r['TCR_EL1']>>30)&3==2 and r['SCTLR_EL1']&1 and r['TTBR0_EL1'] and r['TTBR1_EL1'] and set(d['commpage_user64_shifts'].values())=={12},'Actual active hardware 4K and userspace ABI')
require(d['original_amfi_table_owner_observed'] and d['owner_root_binding_audit']['boot_root_accepted'] and d['owner_root_binding_audit']['_native_owner_root_apple_autherr']==80 and d['actual_signature_failures']==dict(_cs_validate_page_bad_hash=0,_cs_validate_page_no_hash=0) and not d['native_virtio_integrity']['_native_virtio_integrity_failures'],'Original AMFI/root/signature/storage checks remain intact')
require(d['trust_cache_bundle_sha256']==sha(OUT/'offline-trust.bundle') and d['scratch_data']['actual_seed_sha256']==data['sha256']==sha(OUT/'compiler-data-32.cdr'),'Actual precise trust/Data inputs')
(OUT/'native-checkpoint.json').write_text(json.dumps(dict(process_sha256=b['process_sha256'],candidate_sha256=d['kernel_sha256'],checkpoint_sha256=sha(p),
 native_user_page_bytes=4096,actual_tcr=r['TCR_EL1'],actual_exit_status=73,full_original_system_libraries=expected,
 original_shader_count=count,original_fragment_input_profiles=5 if args.fragment_inputs else 0,all_output_code_abi_and_interfaces_exact=True,extracted_bitcode_used_by_guest=False,
 native_container_refusal_suite_observed=False,amfi_and_root_preserved=True,gpu_execution=False,metal_device=False,original_desktop=False),indent=2)+'\n')
print(f'PASS: actual native-4K direct whole-library compilation from original readonly OS, all {count} exact outputs and preserved AMFI/root')
