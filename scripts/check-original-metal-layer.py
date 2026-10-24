#!/usr/bin/env python3
"""Qualify an actual original drawable and present request, without claiming display."""
import hashlib,json,re,struct,subprocess,sys
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
folder=ROOT/'build/offline-metal-layer';fixture=ROOT/'build/darwinboot-owner-hvf-offline-metal-layer'
d=load(fixture/'gdb-stop.json');host=load(folder/'host-enabled-report.json');provenance=load(folder/'host-enabled-provenance.json')
build=load(folder/'build-report.json');data=load(folder/'data-report.json');manifest=load(folder/'test-inputs.json')
base=ROOT/'build/xnu-prelink/native-owner-accelerator-abi-candidate';plan=load(base.with_suffix('.json'));raw=base.with_suffix('.macho').read_bytes();segments=macho(raw)['segments']
require(sha(base.with_suffix('.macho'))==plan['candidate_sha256']==d['kernel_sha256'],'Actual immutable candidate identity')
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
b=d['native_gpu_broker'];require(b['stage']==3 and b['status']==0 and b['clients']==b['closed']==1 and not b['active'] and b['requests']==94 and b['reply_bytes']==107850 and not b['failures'] and not b['refusals'],'Actual private session closes, exact GPU transport accounting')
c=b['actual_cpu_contexts'];require(len(c)==1 and c[0]['pid']==1 and c[0]['native_cpu_4k_accepted'] and int(c[0]['TCR_EL1'],16)==0x22a51a251a and int(c[0]['SCTLR_EL1'],16)&1 and c[0]['task']==d['actual_process']['task'],'Actual active client CPU registers/task, independent of GPU SMMU')
require(provenance['host_exit']==0 and provenance['report_sha256']==sha(folder/'host-enabled-report.json') and provenance['candidate_sha256']==plan['candidate_sha256'] and
 sha(ROOT/'build/host-metal/host-metal-originals')==provenance['process_sha256'] and sha(ROOT/'kits/sheng/HostMetalDiagnostic.mm')==provenance['source_sha256'],'Exact actual host executable, source and report provenance')
for name,digest in provenance['backend_sources'].items():require(sha(ROOT/name)==digest,'Exact host backend source: '+name)
require(host['host_device']=='Apple M1' and host['transport_healthy'] and host['session_finished'] and host['opened_sessions']==host['closed_sessions']==1 and
 not host['cpu_pixel_renderer'] and not host['adreno_execution'] and not host['guest_system_device_discovery'] and not host['original_desktop'] and
 not host['aborted_encoders'] and not host['open_encoders'] and not host['pending_enqueues'] and len(host['operations'])==94 and all(not o['error'] for o in host['operations']),'Healthy actual host ledger, cleanup and truthful offline scope')
require(len(host['completed_commands'])==len(host['enqueued_commands'])==2 and all(r['status']==4 and 0<r['gpu_start_ns']<r['gpu_end_ns'] for r in host['completed_commands']) and all(r['status']==1 for r in host['enqueued_commands']),'Both actual GPU submissions and completion timings')
expected=[hashlib.sha256((struct.pack('<I',left)*32+struct.pack('<I',right)*32)*64).hexdigest() for left,right in ((0xff0000ff,0xffff0000),(0xffff0000,0xff0000ff))]
require(len(host['texture_readbacks'])==2 and [r['sha256'] for r in host['texture_readbacks']]==expected and all(r['bytes']==16384 for r in host['texture_readbacks']),'Complete actual original renderer RGBA and GPU-converted drawable BGRA hashes')
require(len(host['texture_uploads'])==10 and sum(r['bytes'] for r in host['texture_uploads'])==32768 and all(r['sha256']==hashlib.sha256(bytes(r['bytes'])).hexdigest() for r in host['texture_uploads']),'Initial surfaces entirely zero; no CPU-created output pixels')
require(len(host['function_specializations'])==4 and all(r['actual_success'] for r in host['function_specializations']) and {r['name'] for r in host['function_specializations']}=={'plane__vertex','plane_color_fragment'} and len(host['texture_bindings'])==1 and host['texture_bindings'][0]['texture']==host['texture_readbacks'][0]['texture'],'Actual original renderer specialization and exact output binding')
l=d['native_legacy_display'];require(l['stage']==10 and not l['guard_slot'] and not l['transfer_calls'] and not l['retirement_calls'] and not l['swap_validation_calls'],'Detached layer makes no original framebuffer presentation; zero display completion')
(folder/'checkpoint.json').write_text(json.dumps(dict(candidate_sha256=plan['candidate_sha256'],process_sha256=build['process_sha256'],manifest_sha256=build['manifest_sha256'],qemu_report_sha256=sha(fixture/'gdb-stop.json'),host_report_sha256=sha(folder/'host-enabled-report.json'),actual_original_drawable=True,actual_original_present_request=True,actual_gpu_commands=2,pixels_checked=4096,presented_time=0,actual_display_completion=False,system_metal=False,original_desktop=False,adreno_execution=False,active_native_adreno_4k=False),indent=2)+'\n')
print('PASS: actual original CAMetalDrawable, two GPU completions, all drawable pixels, genuine present request and three invalid-call refusals')
print('Detached layer presentedTime is zero; no display completion, normal Metal discovery, desktop or Adreno qualification')
