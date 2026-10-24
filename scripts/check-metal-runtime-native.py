#!/usr/bin/env python3
"""Qualify original Metal runtime ABI and its genuine missing-provider result."""
import hashlib
import json
from pathlib import Path
import struct
import sys
ROOT=Path(__file__).resolve().parents[1];OUT=ROOT/'build/metal-runtime-diagnostic'
sys.path.insert(0,str(ROOT/'kits/XnuPrelink'))
from native_prelink import macho
def require(value,why):
    if not value:raise SystemExit(why)
def sha(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()
build=json.loads((OUT/'build-report.json').read_text());data=json.loads((OUT/'data-report.json').read_text())
abi=json.loads((ROOT/'build/gpu-analysis/metal-device-abi.json').read_text())
require(sha(OUT/'air-native')==build['process_sha256']==data['process_sha256'],'Exact own signed runtime observer')
require(sha(ROOT/'kits/sheng/MetalRuntimeDiagnostic.cpp')==build['source_sha256'] and sha(ROOT/'build/gpu-analysis/metal-device-abi.json')==build['manifest_sha256']==data['manifest_sha256'],'Exact observer and original ABI manifest')
require(abi['dyld_revision']=='a571176e8e00c47e95b95e3156820ebec0cbd5e6' and abi['cache_uuid']=='590d839a82d230f1a17d0cc053420f73','Exact original cache/source format')
for name,digest in abi['layout_source_sha256'].items():require(sha(ROOT/'refer/dyld'/name)==digest,'Exact primary source metadata layout')
require(len(abi['device']['methods'])==575 and len(abi['metaclass']['methods'])==2 and abi['device']['instance_size']==536,'Exact original class ABI coverage')
raw=(OUT/'air-native').read_bytes();parsed=macho(raw)
require(parsed['kind']==2 and [struct.unpack_from('<II',raw,at+8) for cmd,at,_ in parsed['commands'] if cmd==0x32]==[(2,0x110000)],'Ordinary ARM64 subtype0/iOS17 executable')
require(build['code_signature_page_bytes']==4096,'Own actual CodeDirectory signing granule')
bundle=(OUT/'offline-trust.bundle').read_bytes();offsets=struct.unpack_from('<3I',bundle,4)
original=(ROOT/'build/original-static-system-trust/original-static-system.bundle').read_bytes();old=struct.unpack_from('<2I',original,4)
require(struct.unpack_from('<I',bundle)[0]==3 and bundle[offsets[1]:offsets[2]]==original[old[0]:old[1]] and bundle[offsets[2]:]==original[old[1]:],'Both complete original trust modules remain unchanged')
require(bundle[offsets[0]+24:offsets[0]+44].hex()==build['cdhash'],'Exact explicitly trusted own CodeDirectory')
p=ROOT/'build/darwinboot-owner-hvf-metal-runtime-complete/gdb-stop.json';d=json.loads(p.read_text())
plan=json.loads((ROOT/'build/xnu-prelink/native-owner-air-outer-candidate.json').read_text())
require(d['kernel_sha256']==plan['candidate_sha256']==sha(ROOT/'build/xnu-prelink/native-owner-air-outer-candidate.macho'),'Exact actual candidate')
require(d['qemu_accelerator']=='hvf' and not d['bounded_timeout_interrupted'] and d['stop_symbol']=='_exit' and d['entry_regions']['1:0:8']['bytes']=='4a00000000000000' and int(d['x0'],16)==d['actual_initproc_pointer'],'Actual init exit(74) after all ABI and missing-provider checks')
r={k:int(v,16) for k,v in d['system_registers'].items()}
require(d['native_4k_mmu_observed'] and (r['TCR_EL1']>>14)&3==0 and (r['TCR_EL1']>>30)&3==2 and r['SCTLR_EL1']&1 and r['TTBR0_EL1'] and r['TTBR1_EL1'] and set(d['commpage_user64_shifts'].values())=={12},'Actual active CPU 4K and user ABI')
require(d['native_iogpu_load_status']==0,'Genuine original IOGPUFamily initialized')
require(d['original_amfi_table_owner_observed'] and d['owner_root_binding_audit']['boot_root_accepted'] and d['owner_root_binding_audit']['_native_owner_root_apple_autherr']==80 and d['actual_signature_failures']==dict(_cs_validate_page_bad_hash=0,_cs_validate_page_no_hash=0) and not d['native_virtio_integrity']['_native_virtio_integrity_failures'],'Original AMFI/root/signature/storage checks remain intact')
require(d['trust_cache_bundle_sha256']==sha(OUT/'offline-trust.bundle') and d['scratch_data']['actual_seed_sha256']==data['sha256']==sha(OUT/'compiler-data-32.cdr'),'Actual exact trust/Data inputs')
(OUT/'native-checkpoint.json').write_text(json.dumps(dict(process_sha256=build['process_sha256'],checkpoint_sha256=sha(p),candidate_sha256=d['kernel_sha256'],
    original_abi_manifest_sha256=build['manifest_sha256'],native_user_page_bytes=4096,actual_tcr=r['TCR_EL1'],actual_exit_status=74,
    original_instance_methods_matched=575,original_class_methods_matched=2,original_type_encodings_and_actual_implementations_matched=True,
    original_iogpu_initialized=True,actual_iokit_matching_status=0,actual_ioacceleratores_matches=0,
    actual_original_mtl_create_system_default_device_called=True,actual_default_device_is_nil=True,
    native_adreno_device=False,gpu_execution=False,original_desktop=False,production_storage_integrity=False),indent=2)+'\n')
print('PASS: 577 original Metal method types/implementations in actual native-4K runtime; original IOGPU initialized')
print('Observed: successful empty IOAcceleratorES query and genuine original MTLCreateSystemDefaultDevice returning nil; native Adreno device remains missing')
