#!/usr/bin/env python3
"""Verify native original-runtime Metal calls backed by real GPU objects/commands."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
ROOT=Path(__file__).resolve().parents[1];KIT=ROOT/'drivers/AdrenoMetal'
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--iosurface',action='store_true');p.add_argument('--bgra',action='store_true');a=p.parse_args()
if a.bgra:a.iosurface=True
kind='offline-metal-bgra' if a.bgra else 'offline-metal-iosurface' if a.iosurface else 'offline-metal-api';OUT=ROOT/'build'/kind
clear_pixel=0xff336699 if a.bgra else 0x3f800000
initial_pixel=0x7b3d1f0f if a.bgra else 0x40000000
def require(v,why):
    if not v:raise SystemExit(why)
def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
build=json.loads((OUT/'build-report.json').read_text());inputs=OUT/'test-inputs.json'
require(sha(inputs)==build['manifest_sha256'] and sha(OUT/'air-native')==build['process_sha256'],'Exact own native process/source manifest')
executable=(OUT/'air-native').read_bytes()
magic,cpu,subtype,filetype,commands,command_bytes=struct.unpack_from('<6I',executable)
require((magic,cpu,subtype,filetype)==(0xfeedfacf,0x100000c,0,2) and 32+command_bytes<=len(executable),
        'Actual ordinary ARM64 executable header and bounded load commands')
at=32;deployment=None
for _ in range(commands):
    command,length=struct.unpack_from('<2I',executable,at);require(length>=8 and at+length<=32+command_bytes,'Actual bounded Mach-O command')
    if command==0x32:deployment=struct.unpack_from('<2I',executable,at+8)
    at+=length
require(at==32+command_bytes and deployment==(2,0x110000),'Actual iOS17 deployment without ARM64e substitution')
sources=json.loads(inputs.read_text())['sources']
for name,digest in sources.items():require(sha(ROOT/name)==digest,'Exact observed frontend/backend source '+name)
revision=subprocess.check_output(['git','-C',str(KIT),'rev-parse','HEAD'],text=True).strip()
for name in ('OfflineMetalFrontend.mm','OfflineMetalWire.h','HostMetalCommandServer.mm','HostMetalCommandServer.h'):
    data=subprocess.check_output(['git','-C',str(KIT),'show',revision+':'+name])
    require(hashlib.sha256(data).hexdigest()==sha(KIT/name),'Exact committed native Metal source')
subprocess.run(['codesign','--verify','--strict',str(OUT/'air-native')],check=True,timeout=30)
detail=subprocess.run(['codesign','--display','--verbose=4',str(OUT/'air-native')],capture_output=True,text=True,check=True).stderr
require(re.search(r'^Page size=4096$',detail,re.M) and build['cdhash'] in detail,'Actual own 4K CodeDirectory')
data=json.loads((OUT/'data-report.json').read_text())
require(sha(OUT/'compiler-data-32.cdr')==data['sha256'] and data['process_sha256']==build['process_sha256']
        and not data['original_os_modified'] and data['own_mutable_data'],'Exact staged own executable and unchanged target inputs')
require(sha(OUT/'offline-trust.bundle')==build['trust_bundle_sha256'],'Exact explicit own trust handoff')
trust=(OUT/'offline-trust.bundle').read_bytes();original=(ROOT/'build/original-static-system-trust/original-static-system.bundle').read_bytes()
n=struct.unpack_from('<I',trust)[0];starts=struct.unpack_from('<'+str(n)+'I',trust,4)
on=struct.unpack_from('<I',original)[0];os=struct.unpack_from('<'+str(on)+'I',original,4)
require(n==3 and on==2 and [trust[a:b] for a,b in zip(starts[1:],(*starts[2:],len(trust)))]
        ==[original[a:b] for a,b in zip(os,(*os[1:],len(original)))],'Both original trust modules preserved exactly')
plan=json.loads((ROOT/'build/xnu-prelink/native-owner-console-bounds-candidate.json').read_text())
require(sha(ROOT/'build/xnu-prelink/native-owner-console-bounds-candidate.macho')==plan['candidate_sha256'],'Exact native-4K candidate')
host_build=json.loads((ROOT/'build/host-metal/qualification.json').read_text())
require(host_build['source_sha256']==sha(ROOT/'kits/sheng/HostMetalDiagnostic.mm')
        and host_build['process_sha256']==sha(ROOT/'build/host-metal/host-metal-originals'),'Exact live host process and loader')
for name,digest in host_build['backend_sources'].items():require(sha(ROOT/name)==digest,'Exact compiled backend dependency')
host=json.loads((OUT/'host-report.json').read_text());ops=host['operations']
require(host['host_device'] and not host['cpu_pixel_renderer'] and not host['guest_system_device_discovery']
        and not host['adreno_execution'] and not host['original_desktop'],'Explicit partial frontend/real GPU scope')
sizes={0x100:0,0x101:0,0x102:32,0x103:136,0x104:88,0x105:152,0x106:8,0x107:456,
       0x108:16,0x109:16,0x10a:40,0x10b:8,0x10c:8,0x10d:48,0x10e:8,0x10f:0}
allocating={0x101,0x102,0x103,0x104,0x105,0x106,0x107}
require(20<len(ops)<100 and ops[0]['operation']==0x100 and ops[-1]['operation']==0x10f,'Real device-query/session-finish boundaries')
for i,row in enumerate(ops):
    require(row['request']==i+1 and (row['operation']==0x110 and a.iosurface and 56<row['input_bytes']<=4080 or row['operation'] in sizes and row['input_bytes']==sizes[row['operation']]),
            'Exact actual typed native request framing')
    if not row['error'] and row['operation'] in allocating:require(row['handle']>0,'Real allocated host object identity')
errors=[r for r in ops if r['error']]
require(len(errors)==2 and [(r['operation'],r['error'],r['handle']) for r in errors]==[(0x102,-4,0),(0x103,-4,0)],
        'Actual changed-library identity and unknown-function refusals')
require(sum(r['operation']==0x10a for r in ops)==1 and sum(r['operation']==0x10e for r in ops)>=4,
        'Guest selects exactly one draw and releases real host object references')
require(len(host['completed_commands'])==2 and len(host['texture_readbacks'])==(4 if a.iosurface else 2),'Actual clear/draw GPU submissions and texture readbacks')
for i,(command,texture) in enumerate(zip(host['completed_commands'],host['texture_readbacks'][::2] if a.iosurface else host['texture_readbacks'])):
    expected=clear_pixel if i==0 else 0
    require(command['status']==4 and 0<command['gpu_start_ns']<command['gpu_end_ns']
            and texture['bytes']==16384 and texture['sha256']==hashlib.sha256(struct.pack('<I',expected)*4096).hexdigest(),
            'Real completed GPU work, ordered timing and full actual texture bytes')
if a.iosurface:
    uploads=host['texture_uploads'];require(len(uploads)>=10 and sum(u['bytes'] for u in uploads)==32768,'Complete actual native IOSurface uploads')
    for i in range(2):
        texture=host['texture_readbacks'][i*2]['texture'];source=initial_pixel if i==0 else clear_pixel;covered=set()
        for row in [u for u in uploads if u['texture']==texture]:
            require(row['bytes']==row['width']*row['height']*4 and row['width']>0 and row['height']>0
                and row['x']+row['width']<=64 and row['y']+row['height']<=64
                and row['sha256']==hashlib.sha256(struct.pack('<I',source)*(row['bytes']//4)).hexdigest(),
                'Actual IOSurface bytes copied from native mapping before GPU execution')
            for y in range(row['y'],row['y']+row['height']):
                for x in range(row['x'],row['x']+row['width']):
                    require((x,y) not in covered,'Nonoverlapping actual upload regions');covered.add((x,y))
        require(len(covered)==4096,'Every actual native IOSurface pixel uploaded')
        expected=clear_pixel if i==0 else 0
        for row in host['texture_readbacks'][i*2:i*2+2]:
            require(row['texture']==texture and row['bytes']==16384
                and row['sha256']==hashlib.sha256(struct.pack('<I',expected)*4096).hexdigest(),
                'GPU output returned both to actual IOSurface mapping and explicit getBytes')
    require(sum(row['operation']==0x110 for row in ops)==len(uploads),'Actual descriptor uploads match host operation ledger')
else:require(not host['texture_uploads'],'Ordinary textures do not fabricate IOSurface uploads')

observations={}
for case in ('positive','missing-device','default-off'):
    suffix='' if case=='positive' else '-'+case
    path=ROOT/('build/darwinboot-owner-hvf-'+kind+suffix+'/gdb-stop.json');d=json.loads(path.read_text())
    r={k:int(v,16) for k,v in d['system_registers'].items()};g=d['native_console_globals']
    require(d['kernel_sha256']==plan['candidate_sha256'] and d['qemu_accelerator']=='hvf' and not d['bounded_timeout_interrupted'],
            'Actual bounded same-kernel native observation')
    require(d['trust_cache_bundle_sha256']==build['trust_bundle_sha256']
            and d['scratch_data']['input']['sha256']==data['sha256']
            and d['scratch_data']['actual_seed_sha256']==data['sha256'],
            'Exact current own process/trust/Data fixture in every actual control observation')
    require(d['native_4k_mmu_observed'] and (r['TCR_EL1']>>14)&3==0 and (r['TCR_EL1']>>30)&3==2
            and r['SCTLR_EL1']&1 and r['TTBR0_EL1'] and r['TTBR1_EL1'] and set(d['commpage_user64_shifts'].values())=={12},
            'Actual active native 4K hardware CPU translation and userspace ABI')
    require(d['original_amfi_table_owner_observed'] and d['owner_root_binding_audit']['boot_root_accepted']
            and d['owner_root_binding_audit']['_native_owner_root_apple_autherr']==80
            and d['actual_signature_failures']==dict(_cs_validate_page_bad_hash=0,_cs_validate_page_no_hash=0),
            'Original AMFI/signature checks and independent owner root retained')
    code=None
    if case!='default-off':
        require(d['stop_symbol']=='_exit','Actual native process exit')
        code=struct.unpack_from('<I',bytes.fromhex(d['entry_regions']['1:0:8']['bytes']))[0]
    if case=='positive':
        require(code==(151 if a.bgra else 149 if a.iosurface else 119) and g['_native_console_ready']==1 and not g['_native_console_failures']
                and g['_native_console_tx_requests']==len(ops)
                and g['_native_console_tx_bytes']==sum(16+row['input_bytes'] for row in ops)
                and g['_native_console_rx_bytes']==sum(32+row['result_bytes'] for row in ops),
                'Actual full Metal API draw/readback program exit and matching complete transport ledger')
    else:
        require(not any(g.values()),'No fabricated device, traffic or completion without transport')
        if case=='missing-device':require(code==128,'Explicit frontend creation refuses absent service')
        else:require(d['original_launchd_main_executed'] and d['followed_main_entry']['first_instruction_completed'],'Unchanged default-off original launchd main')
    observations[case]=dict(checkpoint_sha256=sha(path),exit=code,actual_tcr=r['TCR_EL1'],console=g)
(OUT/'native-checkpoint.json').write_text(json.dumps(dict(candidate_sha256=plan['candidate_sha256'],kit_revision=revision,
    process_sha256=build['process_sha256'],source_manifest_sha256=sha(inputs),host_report_sha256=sha(OUT/'host-report.json'),
    observations=observations,real_metal_api_gpu_draw=True,actual_metal_operations=len(ops),real_gpu_pixel_bytes=32768,actual_native_iosurface=a.iosurface,guest_gpu_surface_zero_copy=False,
    original_default_device_still_nil=True,explicit_partial_frontend=True,system_device_discovery=False,
    display_presentation=False,original_desktop=False,adreno_execution=False,active_gpu_smmu_qualified=False),indent=2)+'\n')
print('PASS: original native iOS17 Metal APIs create actual host GPU objects/commands and verify complete texture readback')
print('Library/function refusals, real reference release and device/default-off controls pass; no system Metal/UI/Adreno claim')
