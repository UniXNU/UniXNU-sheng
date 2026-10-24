#!/usr/bin/env python3
"""Qualify real native Mach shared-event capabilities and original handle lifetime."""
import argparse,hashlib,json,plistlib,re,struct,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];sys.path.insert(0,str(ROOT/'kits/XnuPrelink'))
from native_prelink import macho,backed_offset
def require(value,why):
 if not value:raise SystemExit(why)
def load(path):return json.loads(path.read_text())
def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def modules(raw):
 n=struct.unpack_from('<I',raw)[0];starts=struct.unpack_from('<'+'I'*n,raw,4)
 return [raw[a:b] for a,b in zip(starts,(*starts[1:],len(raw)))]
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--api-validation',action='store_true');p.add_argument('--notifications',action='store_true');p.add_argument('--coding',action='store_true');p.add_argument('--cross-process',action='store_true');a=p.parse_args()
if a.notifications and (a.coding or a.cross_process):p.error('Select notifications or secure coding as independent instruments')
if a.cross_process:a.coding=True
tag='api-validation' if a.api_validation else 'enabled'
kind='offline-metal-shared-cross-process' if a.cross_process else 'offline-metal-shared-coding' if a.coding else 'offline-metal-shared-notifications' if a.notifications else 'offline-metal-shared-events'
folder=ROOT/'build'/kind;fixture=ROOT/('build/darwinboot-owner-hvf-'+kind+('-api-validation' if a.api_validation else ''))
host_path=folder/('host-'+tag+'-report.json');d=load(fixture/'gdb-stop.json');host=load(host_path);provenance=load(folder/('host-'+tag+'-provenance.json'))
build=load(folder/'build-report.json');data=load(folder/'data-report.json');manifest=load(folder/'test-inputs.json')
base=ROOT/'build/xnu-prelink/native-owner-metal-shared-events-candidate';plan=load(base.with_suffix('.json'));raw=base.with_suffix('.macho').read_bytes();segments=macho(raw)['segments']
require(sha(base.with_suffix('.macho'))==plan['candidate_sha256']==d['kernel_sha256'],'Actual immutable candidate identity')
require(sha(ROOT/'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837.unstripped.noctf')==plan['native_sha256'],'Exact current native kernel')
broker_build=load(ROOT/'build/native-offline-gpu/build-report.json')
for rel,digest in broker_build['source_sha256'].items():require(sha(ROOT/rel)==digest,'Exact compiled native broker source: '+rel)
require(sha(ROOT/'build/native-offline-gpu/libNativeOfflineGPU.late.a')==broker_build['archive_sha256'],'Exact compiled private broker archive')
old=load(ROOT/'build/xnu-prelink/native-owner-offline-gpu-broker-cpu-candidate.json')
require(plan['components']==old['components'] and plan['original_executable_sha256']==old['original_executable_sha256'] and plan['early_security_initializers']==old['early_security_initializers'],'All 29 original executable hashes and security initialization preserved')
for module,digest in plan['original_executable_sha256'].items():
 e=next(e for e in plan['extents'] if e['module']==module and e['segment']=='__TEXT_EXEC');at=backed_offset(segments,e['va'],e['size'],len(raw))
 require(hashlib.sha256(raw[at:at+e['size']]).hexdigest()==digest,'Actual whole original executable identity: '+module)
for name,digest in manifest['sources'].items():require(sha(ROOT/name)==digest,'Exact compiled frontend/instrument source: '+name)
require(sha(folder/'air-native')==build['process_sha256']==data['process_sha256'] and sha(folder/'compiler-data-32.cdr')==data['sha256'] and
 sha(folder/'test-inputs.json')==build['manifest_sha256']==data['manifest_sha256'] and sha(folder/'offline-trust.bundle')==build['trust_bundle_sha256'],'Exact signed own process, Data, trust and source manifest')
require(struct.unpack_from('<4I',(folder/'air-native').read_bytes())==(0xfeedfacf,0x100000c,0,2),'Actual ordinary ARM64 process')
for signed in (folder/'air-native',):
 subprocess.run(['codesign','--verify','--strict',signed],check=True,capture_output=True,timeout=30)
 detail=subprocess.run(['codesign','--display','--verbose=4',signed],check=True,capture_output=True,text=True,timeout=30).stderr
 require(re.search(r'^Page size=4096$',detail,re.M),'Actual own 4K CodeDirectory')
require(modules((folder/'offline-trust.bundle').read_bytes())[1:]==modules((ROOT/'build/original-static-system-trust/original-static-system.bundle').read_bytes()),'Both complete original trust modules unchanged')
entitlements=plistlib.loads(subprocess.check_output(['codesign','-d','--entitlements',':-',str(folder/'air-native')],stderr=subprocess.DEVNULL))
require(entitlements==build['entitlements']=={'com.apple.private.security.no-container':True,'com.apple.security.iokit-user-client-class':['NativeOfflineGPUClient','IOSurfaceRootUserClient']},'Exact original-enforced own scoped diagnostic entitlements, without a no-sandbox substitution')
require(not d['bounded_timeout_interrupted'] and d['stop_symbol']=='_exception_triage' and d['actual_process']['pid']==1 and d['actual_process']['name']=='air-native' and
 d['native_4k_mmu_observed'] and d['original_amfi_table_owner_observed'] and d['owner_root_binding_audit']['boot_root_accepted'] and
 d['actual_signature_failures']==dict(_cs_validate_page_bad_hash=0,_cs_validate_page_no_hash=0) and
 d['scratch_data']['actual_seed_sha256']==data['sha256'] and d['trust_cache_bundle_sha256']==build['trust_bundle_sha256'],'Actual original root/AMFI, exact inputs and active native CPU 4K')
u=d['entry_user_state_candidates'][0];v=struct.unpack('<32Q',bytes.fromhex(u['register_memory']['13:100']['bytes']))
require(not int(u['cpsr'],16)&31 and u['instruction_bytes'].startswith('000020d4') and v[:4]==(0x53484556,1,4096,4096) and
 v[4]>1 and v[12]>1 and v[4]!=v[12] and v[5:7]==v[13:15]==(4096,4096) and v[7:9]==(4096,2) and
 v[9]-d['actual_process']['shared_region_slide']==0x187bb31b4 and v[10:12]==((1|(2<<8)|(1<<16)|(1<<24)|(1<<32)) if a.notifications else ((1|(1<<40)|(1<<41)|((1<<42) if a.cross_process else 0)) if a.coding else 1),4) and
 v[15:20]==(8192,1,3,(3|(3<<8)|(1<<16)|(2<<24)|(1<<32)) if a.notifications else (3|(1<<42)) if a.cross_process else 3,4) and v[20:23]==(0,0,1),
 'Two actual reaped children, real original handle retain 2->3->2, stale/unrelated/raw refusals, and all 12288 real GPU pixels')
b=d['native_gpu_broker'];require(b['stage']==3 and not b['status'] and b['clients']==b['closed']==2 and not b['active'] and not b['failures'] and not b['refusals'],'Actual private GPU contexts close without transport/backend failures')
c=b['actual_cpu_contexts'];require(len(c)==2 and {r['pid'] for r in c}=={v[4],v[12]} and len({r['TTBR0_EL1'] for r in c})==len({r['task'] for r in c})==2,'Distinct actual worker tasks and active TTBR0 contexts')
for r in c:
 require(r['native_cpu_4k_accepted'] and int(r['TCR_EL1'],16)==0x22a51a251a and int(r['SCTLR_EL1'],16)&1 and int(r['TTBR0_EL1'],16) and int(r['TTBR1_EL1'],16),'Actual native CPU granules/translation registers in each GPU process')
sessions={r['pid']:r['session'] for r in c};source=sessions[v[4]];peer=sessions[v[12]]
require(d['native_gpu_shared_events']==dict(exports=1,imports=3 if a.cross_process else 2 if a.coding else 1,drops=1,refusals=2,tokens_live=0,ports=1),'Actual kernel OSObject/Mach capability lifetime, exact import/drop and no live token')
require(provenance['host_exit']==0 and provenance['report_sha256']==sha(host_path) and provenance['candidate_sha256']==plan['candidate_sha256'] and
 sha(ROOT/'build/host-metal/host-metal-originals')==provenance['process_sha256'] and sha(ROOT/'kits/sheng/HostMetalDiagnostic.mm')==provenance['source_sha256'],'Exact executed host source/binary/report provenance')
for name,digest in provenance['backend_sources'].items():require(sha(ROOT/name)==digest,'Exact host backend source: '+name)
ops=host['operations'];require(host['host_device']=='Apple M1' and host['transport_healthy'] and host['session_finished'] and host['opened_sessions']==host['closed_sessions']==2 and
 not host['cpu_pixel_renderer'] and not host['adreno_execution'] and not host['guest_system_device_discovery'] and not host['original_desktop'] and not host['live_shared_exports'] and
 not host['pending_submissions'] and not host['failed_commands'] and not host['open_encoders'] and not host['aborted_encoders'] and
 len(ops)==b['requests'] and sum(32+row['result_bytes'] for row in ops)==b['reply_bytes'] and not any(row['error'] for row in ops),'Complete independent host request/reply accounting, real cleanup and explicit offline scope')
require(all(o['request']==i+1 for i,o in enumerate(ops)),'Actual host global framing')
for sid in (source,peer):
 numbers=[o['client_request'] for o in ops if o['session']==sid];require(numbers==list(range(1,len(numbers)+1)),'Internal Mach operations do not desynchronize actual host session framing')
exports=host['shared_event_exports'];expected_operations=['export','import','import','import','drop'] if a.cross_process else ['export','import','import','drop'] if a.coding else ['export','import','drop']
require([e['operation'] for e in exports]==expected_operations and exports[0]['session']==source and exports[-2]['session']==peer and exports[-1]['session']==0 and len({e['export'] for e in exports})==1,'One real exported Metal handle retained through native Mach authority and dropped once')
if a.coding:require(exports[1]['session']==source and exports[1]['event']!=exports[0]['event'],'Original same-process NSXPC-decoded handle actually imports a distinct real event alias')

close=next(o['request'] for o in ops if o['operation']==0x141 and o['session']==source)
imported=next(o['request'] for o in ops if o['operation']==0x13a and o['session']==peer and (not a.cross_process or o['handle']==exports[-2]['event']));dropped=next(o['request'] for o in ops if o['operation']==0x13b)
submits=[o for o in ops if o['operation']==0x12e];require(len(submits)==3 and submits[0]['session']==source and all(o['session']==peer for o in submits[1:]) and
 close<imported<submits[1]['request']<dropped<submits[2]['request'],'Recipient imports and executes after creator close/real waitpid; imported event survives final original handle destruction')
require(len(host['completed_commands'])==len(host['submitted_commands'])==3 and all(r['status']==4 and 0<r['gpu_start_ns']<r['gpu_end_ns'] for r in host['completed_commands']) and
 all(not r['wait_until_completed_called'] and r['actual_status'] in (2,3,4) for r in host['submitted_commands']),'Three actual asynchronous GPU submissions and real completion times')
require(not host['texture_uploads'] and len(host['texture_readbacks'])==3 and all(r['bytes']==16384 for r in host['texture_readbacks']) and
 [r['sha256'] for r in host['texture_readbacks']]==[hashlib.sha256(struct.pack('<I',pixel)*4096).hexdigest() for pixel in (0xff0000ff,0xffff0000,0xff00ff00)],'All real red/blue/green GPU pixels, no CPU-rendered uploads')
events=host['event_operations'];require([(r['signal'],r['value']) for r in events]==[(True,1),(False,1),(True,2),(False,2),(True,3)] and all(r['shared'] for r in events) and
 [r['session'] for r in events]==[source,peer,peer,peer,peer] and events[1]['actual_signaled_value']>=1 and events[3]['actual_signaled_value']>=2,'Actual shared hardware event counters and real GPU wait/signal encoding')
if a.cross_process:
 require(exports[2]['session']==peer and exports[2]['event']!=exports[3]['event'] and events[1]['event']==events[2]['event']==exports[2]['event'] and events[3]['event']==events[4]['event']==exports[3]['event'],'Actual cross-process NSXPC import executes blue GPU work after creator exit; independently Mach-imported alias then waits on that GPU timeline')
 cross_import=next(o['request'] for o in ops if o['operation']==0x13a and o['session']==peer and o['handle']==exports[2]['event']);require(cross_import<close,'Original cross-process NSXPC receives/decodes before creator exits')
require(not any(r['cpu_set'] for r in host['shared_event_values']),'No CPU event signal substitutes for this GPU dependency')
notifications=host['shared_event_notifications']
if a.notifications:
 require(len(notifications)==5 and
  sum(r['actually_fired'] for r in notifications)==3 and sum(not r['actually_fired'] for r in notifications)==2,'Three genuine host Metal listener observations; two never-signaled registrations remain unfired')
else:require(not notifications,'No native listener claim for this independent capability-only instrument')
require([r['actual_label'] for r in host['shared_event_labels']]==['shared GPU timeline','peer GPU timeline'],'Actual host shared labels and copied native label identity')
off_path=ROOT/'build/darwinboot-owner-hvf-offline-metal-shared-events-default-off/gdb-stop.json';off=load(off_path);ob=off['native_gpu_broker']
require(off['kernel_sha256']==plan['candidate_sha256'] and not off['bounded_timeout_interrupted'] and off['stop_symbol']=='original-launchd-main' and
 off['original_launchd_main_executed'] and off['followed_main_entry']['first_instruction_completed'] and off['native_4k_mmu_observed'] and
 off['original_amfi_table_owner_observed'] and off['owner_root_binding_audit']['boot_root_accepted'] and off['actual_signature_failures']==dict(_cs_validate_page_bad_hash=0,_cs_validate_page_no_hash=0) and
 ob['stage']==0 and ob['status']==-1 and ob['clients']==ob['requests']==ob['closed']==0 and not ob['actual_cpu_contexts'] and
 off['native_gpu_shared_events']==dict(exports=0,imports=0,drops=0,refusals=0,tokens_live=0,ports=0),'Same new kernel executes unchanged original launchd main with native CPU 4K/root/AMFI and zero private GPU/capability activity')
lines=(folder/('host-'+tag+'-stderr.log')).read_text().splitlines();require(all(line=='OFFLINE_METAL_READY' or line.endswith('Metal API Validation Enabled') for line in lines),'No unexpected actual host GPU diagnostic')
if a.api_validation:require(provenance['api_validation_requested'] and any(line.endswith('Metal API Validation Enabled') for line in lines),'Actual GPU API validation enabled')
(folder/('checkpoint-api-validation.json' if a.api_validation else 'checkpoint.json')).write_text(json.dumps(dict(candidate_sha256=plan['candidate_sha256'],process_sha256=build['process_sha256'],qemu_report_sha256=sha(fixture/'gdb-stop.json'),host_report_sha256=sha(host_path),actual_gpu_commands=3,pixels_checked=12288,actual_native_workers=2,original_handle_constructor=True,real_mach_refs_qualified=True,peer_imports_after_creator_exit=True,imported_gpu_event_survives_original_handle=True,wrong_and_stale_mach_rights_refused=True,raw_host_identifier_refused=True,active_native_cpu_4k_contexts=2,actual_gpu_api_validation=a.api_validation,native_listener_qualified=a.notifications,actual_native_listener_callbacks=2 if a.notifications else 0,queued_listener_cancelled_without_deadlock=a.notifications,original_secure_coding_qualified=a.coding,original_secure_coding_cross_process=a.cross_process,non_xpc_coder_refused=a.coding,system_metal=False,original_desktop=False,adreno_execution=False,active_native_adreno_4k=False),indent=2)+'\n')
print('PASS: genuine native shared-event Mach authority, original handle reference lifecycle, creator-exit import and all 12288 actual GPU pixels')
if a.notifications:print('PASS: actual GPU listener records deliver twice on original queues; unfired and blocked-queue registrations cancel without fabricated delivery')
if a.coding:print('PASS: original same-process NSXPC shared-handle coding and real import; non-NSXPC coding refuses')
if a.cross_process:print('PASS: genuine endpoint transport, original cross-process NSXPC handle coding and imported GPU work after creator exit')
print('Normal system Metal, original desktop and active Adreno 4K remain unqualified')
