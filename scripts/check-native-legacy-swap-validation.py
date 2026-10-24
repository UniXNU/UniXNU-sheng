#!/usr/bin/env python3
"""Qualify real original IOSurface validation and actual negative controls."""
import hashlib
import json
from pathlib import Path
import struct
import sys
ROOT=Path(__file__).resolve().parents[1];OUT=ROOT/'build/native-legacy-display'
sys.path.insert(0,str(ROOT/'kits/XnuPrelink'))
from native_prelink import macho,backed_offset
def require(v,s):
    if not v:raise SystemExit(s)
def load(p):return json.loads(p.read_text())
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
build=load(OUT/'build-report.json')
for name,digest in build['source_sha256'].items():require(sha(ROOT/name)==digest,'Exact compiled source '+name)
require(sha(OUT/'libNativeLegacyDisplay.late.a')==build['archive_sha256'] and
        build['owned_entries']==[0,1,7,14,84,86,87,141,155,184,188,211,213,245,246,288,296,306,307,337,338,345],
        'Exact archive and one newly qualified mandatory validation override')
kernel=ROOT/'build/xnu-prelink/native-owner-legacy-display-validate-candidate.macho';plan=load(kernel.with_suffix('.json'))
raw=kernel.read_bytes();image=macho(raw)
require(sha(kernel)==plan['candidate_sha256'] and sha(ROOT/'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837.unstripped.noctf')==plan['native_sha256'],'Exact current kernel')
for module,digest in plan['original_executable_sha256'].items():
    e=next(e for e in plan['extents'] if e['module']==module and e['segment']=='__TEXT_EXEC')
    o=backed_offset(image['segments'],e['va'],e['size'],len(raw))
    require(hashlib.sha256(raw[o:o+e['size']]).hexdigest()==digest,'Whole original executable unchanged '+module)
paths=[ROOT/('build/'+name+'/gdb-stop.json') for name in (
    'darwinboot-owner-hvf-offline-legacy-swap-driver-pm-validate',
    'darwinboot-owner-hvf-offline-legacy-swap-driver-pm',
    'darwinboot-owner-hvf-legacy-display-start-validate-geometry',
    'darwinboot-owner-hvf-offline-legacy-swap-validation-disabled-driver-pm')]
observations=[load(p) for p in paths]
for d in observations:
    l=d['native_legacy_display']
    require(d['kernel_sha256']==plan['candidate_sha256'] and not d['bounded_timeout_interrupted'] and
            d['native_4k_mmu_observed'] and d['original_amfi_table_owner_observed'] and
            d['owner_root_binding_audit']['boot_root_accepted'] and
            d['actual_signature_failures']==dict(_cs_validate_page_bad_hash=0,_cs_validate_page_no_hash=0) and
            l['stage']==10 and not l['guard_slot'] and l['pm_callbacks']==1 and l['pm_ordinal']==1,
            'Actual common native 4K, original AMFI/root, callback and no unknown backend leaf')
positive,returned,negative,disabled=observations
require(positive['stop_symbol']=='_native_legacy_swap_validation_checkpoint' and
        int(positive['x0'],16)==1 and int(positive['x1'],16)==0,'Actual validator accepts the genuine full-frame resource')
request=bytes.fromhex(positive['entry_regions']['2:0:a80']['bytes']);surface=bytes.fromhex(positive['entry_regions']['3:0:100']['bytes'])
require(struct.unpack_from('<4I',request,17)==struct.unpack_from('<4I',request,81)==(0,0,800,600) and
        struct.unpack_from('<4Q',request,2624)==(int(positive['x3'],16),0,0,0) and
        struct.unpack_from('<3Q',surface,88)==(800,600,3200) and struct.unpack_from('<I',surface,128)[0]==0x42475241 and
        struct.unpack_from('<Q',surface,144)[0]==1920000 and struct.unpack_from('<I',surface,152)[0]==0 and
        struct.unpack_from('<Q',surface,48)[0], 'Actual original rectangles, IOSurface identity, format and descriptor')
def payload(d):
    require(d['stop_symbol']=='_exception_triage' and d['actual_process']['name']=='air-native' and d['actual_process']['pid']==1,'Actual own process boundary')
    u=d['entry_user_state_candidates'][0]
    require(int(u['cpsr'],16)&31==0 and u['instruction_bytes'].startswith('000020d4'),'Actual owned EL0 observation instruction')
    return struct.unpack('<16Q',bytes.fromhex(u['register_memory']['13:80']['bytes']))
for d,status,result,reason in [(returned,0,1,0),(disabled,0xe00002c2,0,1)]:
    v=payload(d);l=d['native_legacy_display']
    require(v[:6]==(0x47505357,1,4096,4096,800,600) and v[7:12]==(480000,0,1,0,status) and
            v[13]>=24000000 and v[14:]==(1,4) and l['swap_validation_calls']==1 and
            l['swap_validation_result']==result and l['swap_validation_reason']==reason,'Real GPU pixels, exact original End status and explicit gate')
v=payload(negative);l=negative['native_legacy_display']
require(v[:7]==(0x46424745,1,4096,4096,1,1,0) and v[7] and v[8] and
        v[9:]==(0,1,0,0xe00002c2,799,800,1) and l['swap_validation_calls']==1 and
        l['swap_validation_result']==0 and l['swap_validation_reason']==3,'Actual unsupported 799-to-800 source geometry is refused')
own=ROOT/'build/framebuffer-geometry-diagnostic';b=load(own/'build-report.json');data=load(own/'data-report.json')
require(sha(ROOT/'kits/sheng/FramebufferGeometryDiagnostic.cpp')==b['source_sha256'] and
        sha(own/'air-native')==b['process_sha256']==data['process_sha256'] and
        sha(own/'compiler-data-32.cdr')==data['sha256']==negative['scratch_data']['actual_seed_sha256'] and
        sha(own/'offline-trust.bundle')==b['trust_bundle_sha256']==negative['trust_cache_bundle_sha256'], 'Exact signed/staged negative geometry instrument')
for prefix in ['host-driver-pm','host-disabled-driver-pm']:
    host=load(ROOT/'build/offline-legacy-swap'/f'{prefix}-report.json')
    require(host['host_device']=='Apple M1' and not host['cpu_pixel_renderer'] and len(host['operations'])==609 and
            all(not o['error'] and o['request']==i+1 for i,o in enumerate(host['operations'])) and
            host['operations'][-1]['operation']==0x10f and len(host['completed_commands'])==1 and
            host['completed_commands'][0]['status']==4 and host['texture_readbacks'][0]['sha256']==
            hashlib.sha256(struct.pack('<I',0xff336699)*480000).hexdigest(),'Actual complete GPU session in both full-frame runs')
require(returned['qemu_screen_capture']['sha256']=='e90f3f3195332e37f9a95d7416dc79ca2fa63d08ed3e170f71704a5678000aaf' and
        sha(paths[1].parent/'screen.png')==returned['qemu_screen_capture']['sha256'],'Actual scanout remains the inspected loader frame')
(OUT/'swap-validation-checkpoint.json').write_text(json.dumps(dict(candidate_sha256=sha(kernel),reports=[sha(p) for p in paths],
    full_gpu_surface_accepted=True,cropped_surface_refused=True,default_gate_refused=True,actual_frame_presented=False,
    system_metal=False,original_desktop=False,adreno_execution=False),indent=2)+'\n')
print('PASS: real full-frame GPU IOSurface accepted, genuine cropped and disabled controls refused')
print('Original End returns success with enabled validation; actual presentation still requires the backend queue')
