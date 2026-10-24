#!/usr/bin/env python3
"""Verify actual original-runtime GPU processes, kernel isolation and CPU 4K."""
import hashlib
import json
from pathlib import Path
import plistlib
import re
import struct
import subprocess
import sys
ROOT=Path(__file__).resolve().parents[1];OUT=ROOT/'build/native-offline-gpu'
sys.path.insert(0,str(ROOT/'kits/XnuPrelink'))
from native_prelink import macho,backed_offset
def require(v,s):
    if not v:raise SystemExit(s)
def load(p):return json.loads(p.read_text())
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
build=load(OUT/'build-report.json')
for name,digest in build['source_sha256'].items():require(sha(ROOT/name)==digest,'Exact compiled broker source '+name)
require(sha(OUT/'libNativeOfflineGPU.late.a')==build['archive_sha256'] and
        sha(ROOT/'build/virtio-mmio-console/libVirtioMmioConsole.a')==build['console_archive_sha256'],'Actual broker/transport archives')
def text_code(p):
    b=p.read_bytes();m=macho(b);s=next(s for g in m['segments'] for s in g['sections'] if s['name']=='__text')
    return b[s['fileoff']:s['fileoff']+s['size']]
require(text_code(OUT/'NativeOfflineGPU.o')==text_code(OUT/'NativeOfflineGPU.late.o'),'Only own initializer registration changes during staging')
kernel=ROOT/'build/xnu-prelink/native-owner-offline-gpu-depth-stencil-candidate.macho';plan=load(kernel.with_suffix('.json'))
require(sha(kernel)==plan['candidate_sha256'] and sha(ROOT/'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837.unstripped.noctf')==plan['native_sha256'],'Exact current native kernel and original-component candidate')
raw=kernel.read_bytes();image=macho(raw)
for module,digest in plan['original_executable_sha256'].items():
    e=next(e for e in plan['extents'] if e['module']==module and e['segment']=='__TEXT_EXEC');o=backed_offset(image['segments'],e['va'],e['size'],len(raw))
    require(hashlib.sha256(raw[o:o+e['size']]).hexdigest()==digest,'Whole original executable preserved '+module)
def modules(b):
    n=struct.unpack_from('<I',b)[0];offsets=struct.unpack_from('<'+'I'*n,b,4)
    return [b[a:z] for a,z in zip(offsets,(*offsets[1:],len(b)))]
original=modules((ROOT/'build/original-static-system-trust/original-static-system.bundle').read_bytes())
observations=[];programs=[];reports=[];codes=[]
for suffix in ('','-without-client','-without-container'):
    directory=ROOT/('build/offline-metal-processes'+suffix);program=load(directory/'build-report.json');data=load(directory/'data-report.json')
    manifest=load(directory/'test-inputs.json');exe=directory/'air-native'
    architecture=struct.unpack_from('<4I',exe.read_bytes())
    require(architecture==(0xfeedfacf,0x100000c,0,2),'Actual ARM64 subtype-0 process, no ARM64e substitution')
    require(sha(exe)==program['process_sha256']==data['process_sha256'] and sha(directory/'test-inputs.json')==program['manifest_sha256']==data['manifest_sha256'] and
            sha(directory/'compiler-data-32.cdr')==data['sha256'] and sha(directory/'offline-trust.bundle')==program['trust_bundle_sha256'],'Exact signed own process, Data, trust and source manifest')
    for name,digest in manifest['sources'].items():require(sha(ROOT/name)==digest,'Exact compiled process/frontend/backend source '+name)
    subprocess.run(['codesign','--verify','--strict',exe],check=True,timeout=30,capture_output=True)
    detail=subprocess.run(['codesign','--display','--verbose=4',exe],capture_output=True,text=True,check=True,timeout=30).stderr
    require(re.search(r'^Page size=4096$',detail,re.M) and program['cdhash'] in detail,'Actual own 4K CodeDirectory')
    expected={'com.apple.private.security.no-container':True,'com.apple.security.iokit-user-client-class':['NativeOfflineGPUClient','IOSurfaceRootUserClient']}
    if suffix=='-without-client':expected.pop('com.apple.security.iokit-user-client-class')
    if suffix=='-without-container':expected.pop('com.apple.private.security.no-container')
    entitlements=plistlib.loads(subprocess.check_output(['codesign','-d','--entitlements',':-',str(exe)],stderr=subprocess.DEVNULL))
    require(entitlements==program['entitlements']==expected and modules((directory/'offline-trust.bundle').read_bytes())[1:]==original,'Only exact own diagnostic permissions; both original trust modules preserved')
    p=ROOT/('build/darwinboot-owner-hvf-offline-metal-processes'+suffix+'-cpu-contexts/gdb-stop.json');d=load(p)
    require(d['kernel_sha256']==plan['candidate_sha256'] and not d['bounded_timeout_interrupted'] and d['stop_symbol']=='_exception_triage' and
            d['native_4k_mmu_observed'] and d['original_amfi_table_owner_observed'] and d['owner_root_binding_audit']['boot_root_accepted'] and
            d['actual_signature_failures']==dict(_cs_validate_page_bad_hash=0,_cs_validate_page_no_hash=0) and
            d['actual_process']['pid']==1 and d['actual_process']['name']=='air-native' and
            d['scratch_data']['actual_seed_sha256']==data['sha256'] and d['trust_cache_bundle_sha256']==program['trust_bundle_sha256'],'Actual original runtime, own EL0 boundary, original AMFI/root and native CPU 4K')
    u=d['entry_user_state_candidates'][0];v=struct.unpack('<32Q',bytes.fromhex(u['register_memory']['13:100']['bytes']))
    require(int(u['cpsr'],16)&31==0 and u['instruction_bytes'].startswith('000020d4') and v[1:4]==(1,4096,4096),'Actual own EL0 breakpoint and native page ABI')
    programs.append(v);observations.append(d);reports.append(p);codes.append(text_code(exe))
require(codes[0]==codes[1]==codes[2],'Every permission control executes identical own machine instructions')
d,denied,container=observations;v,b,c=programs;broker=d['native_gpu_broker']
require(v[0]==0x47505550 and v[4]>1 and v[12]>1 and v[4]!=v[12] and v[5:7]==v[13:15]==(4096,4096) and
        v[7] and v[15] and v[7]!=v[15] and v[8] and v[16] and v[8]!=v[16] and v[9:12]==(4096,0,4) and
        v[17:23]==(4096,1,4,0,0,1),'Two actual successful child processes, original IOSurfaces, whole-frame checks, foreign refusal and real waitpid statuses')
require(broker['stage']==3 and broker['status']==0 and broker['clients']==broker['closed']==2 and not broker['active'] and
        broker['requests']==43 and broker['reply_bytes']==67288 and not broker['failures'] and broker['refusals']==1,'Actual complete broker lifetime and real protocol results')
def cpu_contexts(d,pids):
    rows=d['native_gpu_broker']['actual_cpu_contexts'];require(len(rows)==2 and {r['pid'] for r in rows}==set(pids),'Actual hardware samples belong to the two real worker tasks')
    require(len({r['TTBR0_EL1'] for r in rows})==len({r['task'] for r in rows})==2,'Distinct actual worker TTBR0/task identities')
    for r in rows:
        tcr=int(r['TCR_EL1'],16);require(((tcr>>14)&3)==0 and ((tcr>>30)&3)==2 and int(r['SCTLR_EL1'],16)&1 and
            int(r['TTBR0_EL1'],16) and int(r['TTBR1_EL1'],16) and r['native_cpu_4k_accepted'],'Live CPU TG0/TG1 native 4K and MMU/TTBR state at each actual GPU client')
    return {r['pid']:r['session'] for r in rows}
sessions=cpu_contexts(d,(v[4],v[12]))
require(b[0]==b[4]==b[12]==0x47504641 and b[6]==b[14]==172 and b[20:23]==(172<<8,172<<8,190) and b[26]==1,'Missing client entitlement genuinely refuses both devices; actual failed children reaped')
db=denied['native_gpu_broker'];cpu_contexts(denied,(b[5],b[13]))
require(db['clients']==db['closed']==2 and db['requests']==4 and not db['active'] and not db['failures'] and not db['refusals'],'Original I/O Kit denial leaves no GPU command or live host session')
require(c[0]==0x47504641 and c[22]==189 and c[24]==1 and c[25]==0 and
        container['native_gpu_broker']['clients']==container['native_gpu_broker']['requests']==0 and
        not container['native_gpu_broker']['actual_cpu_contexts'],'Missing service entitlement genuinely returns EPERM from first original spawn, before GPU access')
for denied_case in (denied,container):
    policies=denied_case['registered_mac_policies'];require(policies['read_only'] and any(p['name']=='Sandbox' for p in policies['policies']) and any(p['name']=='AMFI' for p in policies['policies']),'Actual original Sandbox and AMFI remain registered')
host=load(ROOT/'build/offline-metal-processes/host-cpu-contexts-report.json');ops=host['operations']
host_build=load(ROOT/'build/host-metal/qualification.json')
require(sha(ROOT/'build/host-metal/host-metal-originals')==host_build['process_sha256'] and sha(ROOT/'kits/sheng/HostMetalDiagnostic.mm')==host_build['source_sha256'],'Exact actual host process and loader')
for name,digest in host_build['backend_sources'].items():require(sha(ROOT/name)==digest,'Exact compiled host dependency '+name)
require(host['host_device']=='Apple M1' and host['isolated_sessions'] and host['opened_sessions']==host['closed_sessions']==2 and
        not host['cpu_pixel_renderer'] and not host['guest_system_device_discovery'] and not host['adreno_execution'] and not host['original_desktop'] and
        len(ops)==43 and all(o['request']==i+1 for i,o in enumerate(ops)),'Actual real host GPU/session ledger; no CPU renderer or system/Adreno claim')
errors=[o for o in ops if o['error']];require(len(errors)==1 and errors[0]['operation']==0x10d and errors[0]['error']==-2 and errors[0]['session']==sessions[v[12]],'Actual foreign texture read refuses in the second worker namespace')
commands=host['completed_commands'];require(len(commands)==3 and all(r['status']==4 and 0<r['gpu_start_ns']<r['gpu_end_ns'] for r in commands),'Three actual completed GPU submissions and ordered real timestamps')
commits=[o for o in ops if o['operation']==0x10c];close1=next(o['request'] for o in ops if o['operation']==0x141 and o['session']==sessions[v[4]])
require(commits[2]['session']==sessions[v[12]] and commits[2]['request']>close1 and ops[-1]['operation']==0x141,'Second worker genuinely submits GPU work after first namespace has closed')
textures={o['handle']:o['session'] for o in ops if o['operation']==0x104 and not o['error']}
require(textures[v[7]]==sessions[v[4]] and textures[v[15]]==sessions[v[12]],'Actual original surface textures have separate kernel-selected host owners')
readbacks=host['texture_readbacks'];require(len(readbacks)==4 and sum(r['texture']==v[7] for r in readbacks)==1 and sum(r['texture']==v[15] for r in readbacks)==3,'Exact actual whole-surface readback and independent lifetime checks')
for r in readbacks:
    pixel=0xff336699 if r['texture']==v[7] else 0xffcc6633
    require(r['bytes']==16384 and r['sha256']==hashlib.sha256(struct.pack('<I',pixel)*4096).hexdigest(),'Every actual native IOSurface GPU output agrees with complete host readback')
negative_host=load(ROOT/'build/offline-metal-processes-without-client/host-cpu-contexts-report.json')
require(len(negative_host['operations'])==4 and not negative_host['completed_commands'] and
        all(o['operation'] in (0x140,0x141) and not o['error'] for o in negative_host['operations']),'Permission-denied control performs only genuine namespace creation/cleanup')
for case in ('default-off','missing-device'):
    p=ROOT/('build/darwinboot-owner-hvf-offline-metal-processes-cpu-contexts-'+case+'/gdb-stop.json');control=load(p);state=control['native_gpu_broker'];reports.append(p)
    require(control['kernel_sha256']==plan['candidate_sha256'] and not control['bounded_timeout_interrupted'] and
            control['native_4k_mmu_observed'] and not state['clients'] and not state['requests'] and not state['actual_cpu_contexts'],'Real disabled/absent-transport refusal and active kernel CPU 4K')
    if case=='default-off':require(control['stop_symbol']=='original-launchd-main' and state['stage']==0 and state['status']==-1 and
        control['followed_main_entry']['first_instruction_completed'] and control['owner_root_binding_audit']['boot_root_accepted'],'Disabled private broker preserves actual original launchd main and owner root')
    else:require(control['stop_symbol']=='_native_gpu_broker_checkpoint' and state['stage']==1 and state['status']==0xe00002f0-(1<<32),'Missing genuine transport refuses before device publication or user execution')
(OUT/'processes-checkpoint.json').write_text(json.dumps(dict(candidate_sha256=sha(kernel),reports=[sha(p) for p in reports],
    actual_native_gpu_processes=2,actual_host_gpu_completions=3,actual_worker_cpu_4k_contexts=2,
    foreign_resource_refused=True,peer_executes_after_close=True,original_sandbox_preserved=True,
    system_metal=False,original_desktop=False,adreno_execution=False,active_native_adreno_4k=False),indent=2)+'\n')
print('PASS: two actual original-runtime Metal/IOSurface processes, three real GPU completions, real namespace refusal/lifetime and live native CPU 4K in both workers')
print('Both signed permission controls preserve original Sandbox/AMFI; normal Metal discovery, desktop and Adreno remain open')
