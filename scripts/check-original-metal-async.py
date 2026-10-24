#!/usr/bin/env python3
"""Qualify original-runtime asynchronous GPU events, resource visibility and callback lifetime."""
import argparse,hashlib,json,re,struct,subprocess,sys
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
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--primary-only',action='store_true',help='Qualify only the actual async path, without claiming fresh sync/default-off regressions');p.add_argument('--candidate-base',type=Path,default=ROOT/'build/xnu-prelink/native-owner-metal-async-candidate');a=p.parse_args()
folder=ROOT/'build/offline-metal-async';fixture=ROOT/'build/darwinboot-owner-hvf-offline-metal-async'
d=load(fixture/'gdb-stop.json');host=load(folder/'host-enabled-report.json');provenance=load(folder/'host-enabled-provenance.json')
build=load(folder/'build-report.json');data=load(folder/'data-report.json');manifest=load(folder/'test-inputs.json')
base=a.candidate_base.resolve();plan=load(base.with_suffix('.json'));raw=base.with_suffix('.macho').read_bytes();segments=macho(raw)['segments']
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
for signed in (folder/'air-native',folder/'NativeOfflineMetal.bundle'):
 subprocess.run(['codesign','--verify','--strict',signed],check=True,capture_output=True,timeout=30)
 detail=subprocess.run(['codesign','--display','--verbose=4',signed],check=True,capture_output=True,text=True,timeout=30).stderr
 require(re.search(r'^Page size=4096$',detail,re.M),'Actual own 4K CodeDirectory')
require(modules((folder/'offline-trust.bundle').read_bytes())[1:]==modules((ROOT/'build/original-static-system-trust/original-static-system.bundle').read_bytes()),'Both complete original trust modules unchanged')
require(not d['bounded_timeout_interrupted'] and d['stop_symbol']=='_exception_triage' and d['actual_process']['pid']==1 and d['actual_process']['name']=='air-native' and
 d['native_4k_mmu_observed'] and d['original_amfi_table_owner_observed'] and d['owner_root_binding_audit']['boot_root_accepted'] and
 d['owner_root_binding_audit']['_native_owner_root_apple_autherr']==80 and d['actual_signature_failures']==dict(_cs_validate_page_bad_hash=0,_cs_validate_page_no_hash=0) and
 d['scratch_data']['actual_seed_sha256']==data['sha256'] and d['trust_cache_bundle_sha256']==build['trust_bundle_sha256'],'Actual original root/AMFI/inputs and native CPU 4K at real EL0 boundary')
u=d['entry_user_state_candidates'][0];packet=struct.unpack('<32Q',bytes.fromhex(u['register_memory']['13:100']['bytes']));name=bytes.fromhex(u['register_memory']['14:1000']['bytes']).split(b'\0')[0]
require(not int(u['cpsr'],16)&31 and u['instruction_bytes'].startswith('000020d4') and packet[:4]==(0x43414d4c,1,4096,4096) and packet[4] and packet[5:7]==(64,64),'Actual original drawable IOSurface and all 4096 pixels checked')
require(name==b'CAMetalDrawable' and packet[9]-d['actual_process']['shared_region_slide']==0x18956b6bc and packet[10:12]==(7,2),'Actual original CAMetalDrawable present implementation, three refused invalid calls and two actual ordered GPU callbacks')
require(packet[7:9]==(0,0),'Actual first drawable ID and zero presentedTime; no display completion asserted')
b=d['native_gpu_broker'];require(b['stage']==3 and b['status']==0 and b['clients']==b['closed']==1 and not b['active'] and b['requests']>=138 and b['reply_bytes']>=142234 and not b['failures'] and b['refusals']==4,'Actual private session closes, exact GPU transport accounting')
c=b['actual_cpu_contexts'];require(len(c)==1 and c[0]['pid']==1 and c[0]['native_cpu_4k_accepted'] and int(c[0]['TCR_EL1'],16)==0x22a51a251a and int(c[0]['SCTLR_EL1'],16)&1 and c[0]['task']==d['actual_process']['task'],'Actual active client CPU registers/task, independent of GPU SMMU')
require(provenance['host_exit']==0 and provenance['report_sha256']==sha(folder/'host-enabled-report.json') and provenance['candidate_sha256']==plan['candidate_sha256'] and
 sha(ROOT/'build/host-metal/host-metal-originals')==provenance['process_sha256'] and sha(ROOT/'kits/sheng/HostMetalDiagnostic.mm')==provenance['source_sha256'],'Exact actual host executable, source and report provenance')
for name,digest in provenance['backend_sources'].items():require(sha(ROOT/name)==digest,'Exact host backend source: '+name)
require(host['host_device']=='Apple M1' and host['transport_healthy'] and host['session_finished'] and host['opened_sessions']==host['closed_sessions']==1 and
 not host['cpu_pixel_renderer'] and not host['adreno_execution'] and not host['guest_system_device_discovery'] and not host['original_desktop'] and
 not host['aborted_encoders'] and not host['open_encoders'] and not host['pending_enqueues'] and len(host['operations'])==b['requests'] and sum(32+row['result_bytes'] for row in host['operations'])==b['reply_bytes'] and [o['error'] for o in host['operations'] if o['error']]==[-2,-4,-4,-2],'Healthy actual host ledger, cleanup and truthful offline scope')
require(len(host['completed_commands'])==len(host['enqueued_commands'])==4 and all(r['status']==4 and 0<r['gpu_start_ns']<r['gpu_end_ns'] for r in host['completed_commands']) and all(r['status']==1 for r in host['enqueued_commands']),'Both actual GPU submissions and completion timings')
expected=[hashlib.sha256((struct.pack('<I',left)*32+struct.pack('<I',right)*32)*64).hexdigest() for left,right in ((0xff0000ff,0xffff0000),(0xffff0000,0xff0000ff))]
require(len(host['texture_readbacks'])==4 and [r['sha256'] for r in host['texture_readbacks']]==[expected[0],expected[1],expected[1],hashlib.sha256(struct.pack('<I',0xff00ff00)*4096).hexdigest()] and all(r['bytes']==16384 for r in host['texture_readbacks']),'Complete actual original renderer RGBA and GPU-converted drawable BGRA hashes')
require(len(host['texture_uploads'])==10 and sum(r['bytes'] for r in host['texture_uploads'])==32768 and all(r['sha256']==hashlib.sha256(bytes(r['bytes'])).hexdigest() for r in host['texture_uploads']),'Initial surfaces entirely zero; no CPU-created output pixels')
require(len(host['function_specializations'])==4 and all(r['actual_success'] for r in host['function_specializations']) and {r['name'] for r in host['function_specializations']}=={'plane__vertex','plane_color_fragment'} and len(host['texture_bindings'])==2 and host['texture_bindings'][0]['texture']==host['texture_readbacks'][0]['texture'],'Actual original renderer specialization and exact output binding')
require(packet[12:17]==(31,4,1,0x120,1),'Five genuine native fence refusals, four real calls and explicit private/untracked resource metadata')
fences=host['fences_created'];calls=host['fence_operations'];hazards=host['texture_hazard_modes'];copies=host['texture_copies']
require(len(fences)==2 and len(calls)==4 and all(row['session']==1 and row['fence']==fences[0]['fence'] and row['command']==host['event_operations'][0]['command'] for row in calls) and
 [(row['render'],row['update'],row['stages']) for row in calls]==[(True,True,2),(False,False,0),(False,True,0),(True,False,3)] and
 calls[0]['encoder']!=calls[1]['encoder']==calls[2]['encoder']!=calls[3]['encoder'],'Actual render-update -> blit-wait/update -> render-wait on one real GPU command and three distinct encoders')
require(len(hazards)==6 and [(h['requested'],h['actual']) for h in hazards]==[(0,2)]*4+[(1,1)]*2 and hazards[-2]['texture']==host['texture_bindings'][1]['texture'],'Actual backend modes: default resolves to Tracked, explicit private resource is genuinely Untracked')
require(len(copies)==2 and copies[0]['source']==hazards[-2]['texture'] and copies[0]['destination']==copies[1]['source'] and copies[1]['destination']==hazards[-1]['texture'] and copies[0]['source_storage']==2 and copies[0]['destination_storage']==0 and copies[0]['width']==copies[0]['height']==64 and copies[0]['format']==80,'Real private/untracked GPU texture copied into original drawable, then sampled by final actual shader pass')
require(packet[17] in (2,3,4) and packet[18:27]==(4096,2,1,3,2,1,1,4096,1),'Actual native async status, every peer pixel, event protocol, callbacks off caller thread, two reentrant waits and one refused close inside callback')
submits=host['submitted_commands'];events=host['event_operations'];created=host['events_created']
require(len(submits)==4 and all(not row['wait_until_completed_called'] and row['actual_status'] in (2,3,4) for row in submits) and any(row['actual_status']<4 for row in submits) and not host['pending_submissions'] and not host['failed_commands'],'Four actual nonblocking native-runtime submissions including unchanged original RenderBox; resources and callbacks finish with no pending/faulted work')
require(len(created)==1 and len(events)==3 and [(r['signal'],r['value']) for r in events]==[(True,1),(False,1),(True,2)] and all(r['event']==created[0]['event'] and r['session']==1 for r in events) and events[0]['command']==submits[1]['command'] and events[1]['command']==events[2]['command']==submits[2]['command'] and events[1]['actual_producer_status'] in (2,3,4),'Genuine GPU signal/wait chain on two actual native queues; no CPU signal or synthetic fence')
require(len({r['command'] for r in host['completed_commands']})==4 and all(r['observed_by']!='blocking-commit' for r in host['completed_commands']) and all(r['command'] in {q['command'] for q in submits} for r in host['command_observations']) and all(r['actual_status'] in (2,3,4) for r in host['command_observations']),'Actual unique completions and status-query provenance')
require(copies[1]['source_storage']==copies[1]['destination_storage']==0 and copies[1]['width']==copies[1]['height']==64 and copies[1]['format']==80 and {r['texture'] for r in host['texture_readbacks']}=={host['texture_bindings'][0]['texture'],copies[0]['destination'],copies[1]['destination']},'Exact original renderer/drawable/peer resource identities and whole-frame GPU copy')
l=d['native_legacy_display'];require(l['stage']==10 and not l['guard_slot'] and not l['transfer_calls'] and not l['retirement_calls'] and not l['swap_validation_calls'],'Detached layer makes no original framebuffer presentation; zero display completion')
control_path=None;sync_path=None
if not a.primary_only:
 control_path=ROOT/'build/darwinboot-owner-hvf-offline-metal-async-default-off/gdb-stop.json';control=load(control_path);off=control['native_gpu_broker']
 require(control['kernel_sha256']==plan['candidate_sha256'] and not control['bounded_timeout_interrupted'] and control['stop_symbol']=='original-launchd-main' and
  control['original_launchd_main_executed'] and control['followed_main_entry']['first_instruction_completed'] and control['native_4k_mmu_observed'] and
  control['original_amfi_table_owner_observed'] and control['owner_root_binding_audit']['boot_root_accepted'] and control['actual_signature_failures']==dict(_cs_validate_page_bad_hash=0,_cs_validate_page_no_hash=0) and
  off['stage']==0 and off['status']==-1 and off['clients']==off['requests']==off['closed']==0 and not off['actual_cpu_contexts'],'Same kernel default-off genuinely executes original launchd main with root/AMFI/native CPU 4K and no private GPU traffic')
 sync_path=ROOT/'build/darwinboot-owner-hvf-offline-metal-fences/gdb-stop.json';sync=load(sync_path);sync_dir=ROOT/'build/offline-metal-fences';sync_host=load(sync_dir/'host-enabled-report.json');sync_manifest=load(sync_dir/'test-inputs.json');sync_build=load(sync_dir/'build-report.json');sync_data=load(sync_dir/'data-report.json');sync_provenance=load(sync_dir/'host-enabled-provenance.json')
 for name,digest in sync_manifest['sources'].items():require(sha(ROOT/name)==digest,'Exact synchronous regression source: '+name)
 for signed in (sync_dir/'air-native',sync_dir/'NativeOfflineMetal.bundle'):subprocess.run(['codesign','--verify','--strict',signed],check=True,capture_output=True,timeout=30)
 require(sha(sync_dir/'offline-trust.bundle')==sync_build['trust_bundle_sha256'] and modules((sync_dir/'offline-trust.bundle').read_bytes())[1:]==modules((ROOT/'build/original-static-system-trust/original-static-system.bundle').read_bytes()),'Exact synchronous trust and complete original modules')
 require(sha(sync_dir/'air-native')==sync_build['process_sha256']==sync_data['process_sha256'] and sha(sync_dir/'compiler-data-32.cdr')==sync_data['sha256'] and sha(sync_dir/'test-inputs.json')==sync_build['manifest_sha256']==sync_data['manifest_sha256'] and sync['scratch_data']['actual_seed_sha256']==sync_data['sha256'] and sync['trust_cache_bundle_sha256']==sync_build['trust_bundle_sha256'] and sync_provenance['report_sha256']==sha(sync_dir/'host-enabled-report.json') and sync_provenance['process_sha256']==provenance['process_sha256'] and sync_provenance['host_exit']==0,'Exact independently signed synchronous inputs and same actual host backend')
 require(sync['kernel_sha256']==plan['candidate_sha256'] and not sync['bounded_timeout_interrupted'] and sync['stop_symbol']=='_exception_triage' and sync['native_4k_mmu_observed'] and sync['original_amfi_table_owner_observed'] and sync['owner_root_binding_audit']['boot_root_accepted'] and sync['actual_signature_failures']==dict(_cs_validate_page_bad_hash=0,_cs_validate_page_no_hash=0),'Same kernel synchronous regression preserves original root/AMFI/native CPU 4K')
 sv=struct.unpack('<32Q',bytes.fromhex(sync['entry_user_state_candidates'][0]['register_memory']['13:100']['bytes']))
 require(sv[:4]==(0x43414d4c,1,4096,4096) and sv[10:17]==(7,2,31,4,1,0x120,1) and not sync_host['submitted_commands'] and not sync_host['event_operations'] and sync_host['transport_healthy'] and sync_host['session_finished'] and len(sync_host['completed_commands'])==2 and [r['sha256'] for r in sync_host['texture_readbacks']]==expected and len(sync_host['fence_operations'])==4 and not sync_host['pending_submissions'],'Opt-in stays off in existing original drawable/fence path; both original GPU images and controls still pass')
(folder/'checkpoint.json').write_text(json.dumps(dict(candidate_sha256=plan['candidate_sha256'],process_sha256=build['process_sha256'],manifest_sha256=build['manifest_sha256'],qemu_report_sha256=sha(fixture/'gdb-stop.json'),host_report_sha256=sha(folder/'host-enabled-report.json'),default_off_report_sha256=sha(control_path) if control_path else None,actual_original_drawable=True,actual_gpu_fence_calls=4,explicit_untracked_texture=True,native_fence_refusal_controls=5,actual_original_present_request=True,actual_gpu_commands=4,actual_nonblocking_submissions=4,original_renderer_async=True,finish_waited_for_released_command=True,callback_checked_closing_pixels=4096,scheduled_wait_while_callback_lock_held=True,actual_gpu_event_calls=3,peer_pixels_checked=4096,reentrant_callback_waits=2,callback_close_refused=True,synchronous_control_report_sha256=sha(sync_path) if sync_path else None,pixels_checked=4096,presented_time=0,actual_display_completion=False,system_metal=False,original_desktop=False,adreno_execution=False,active_native_adreno_4k=False),indent=2)+'\n')
print('PASS: actual native-runtime asynchronous submissions, GPU events, all drawable/peer pixels and callbacks; reentrant waits and callback-close refusal pass')
print('Detached layer presentedTime is zero; no display completion, normal Metal discovery, desktop or Adreno qualification')
