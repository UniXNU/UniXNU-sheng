#!/usr/bin/env python3
"""Qualify original Sandbox exact, parent and unrelated signed client policies."""
import argparse,hashlib,json,plistlib,re,struct,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];sys.path.insert(0,str(ROOT/'kits/XnuPrelink'))
from native_prelink import macho,backed_offset
def require(value,why):
 if not value:raise SystemExit(why)
def load(path):return json.loads(path.read_text())
def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def modules(raw):
 count=struct.unpack_from('<I',raw)[0];starts=struct.unpack_from('<'+'I'*count,raw,4)
 return [raw[a:b] for a,b in zip(starts,(*starts[1:],len(raw)))]
def text_code(path):
 raw=path.read_bytes();section=next(s for g in macho(raw)['segments'] for s in g['sections'] if s['name']=='__text')
 return raw[section['fileoff']:section['fileoff']+section['size']]
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--candidate-base',type=Path,default=ROOT/'build/xnu-prelink/native-owner-original-provider-probe-candidate');args=p.parse_args()
base=args.candidate_base.resolve();plan=load(base.with_suffix('.json'));raw=base.with_suffix('.macho').read_bytes();segments=macho(raw)['segments']
require(sha(base.with_suffix('.macho'))==plan['candidate_sha256'] and sha(ROOT/'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837.unstripped.noctf')==plan['native_sha256'],'Exact unchanged candidate and native kernel')
broker_build=load(ROOT/'build/native-offline-gpu/build-report.json')
for rel,digest in broker_build['source_sha256'].items():require(sha(ROOT/rel)==digest,'Exact compiled broker source '+rel)
require(sha(ROOT/'build/native-offline-gpu/libNativeOfflineGPU.late.a')==broker_build['archive_sha256'],'Exact real broker archive')
old=load(ROOT/'build/xnu-prelink/native-owner-offline-gpu-broker-cpu-candidate.json')
require(plan['components']==old['components'] and plan['original_executable_sha256']==old['original_executable_sha256'] and plan['early_security_initializers']==old['early_security_initializers'],'All original components/security initializers retained')
for module,digest in plan['original_executable_sha256'].items():
 extent=next(e for e in plan['extents'] if e['module']==module and e['segment']=='__TEXT_EXEC');at=backed_offset(segments,extent['va'],extent['size'],len(raw))
 require(hashlib.sha256(raw[at:at+extent['size']]).hexdigest()==digest,'Whole actual original executable '+module)
original_trust=modules((ROOT/'build/original-static-system-trust/original-static-system.bundle').read_bytes())
audit=load(ROOT/'build/gpu-analysis/original-sandbox-client-class.json')
require(audit['original_sandbox_text_sha256']==plan['original_executable_sha256']['com.apple.security.sandbox'],'Independent original Sandbox analysis identity')
codes=[];results=[];processes=[]
for policy,client in (('full','NativeOfflineGPUClient'),('parent-client','IOUserClient'),('unrelated-client','OSArray')):
 kind='offline-metal-processes'+('' if policy=='full' else '-'+policy);folder=ROOT/'build'/kind;fixture=ROOT/'build'/('darwinboot-owner-hvf-'+kind)
 build=load(folder/'build-report.json');data=load(folder/'data-report.json');manifest=load(folder/'test-inputs.json');exe=folder/'air-native'
 require(sha(exe)==build['process_sha256']==data['process_sha256'] and sha(folder/'compiler-data-32.cdr')==data['sha256'] and sha(folder/'test-inputs.json')==build['manifest_sha256']==data['manifest_sha256'] and sha(folder/'offline-trust.bundle')==build['trust_bundle_sha256'],'Exact signed own program, Data, trust and manifest '+policy)
 for name,digest in manifest['sources'].items():require(sha(ROOT/name)==digest,'Exact compiled own source '+name)
 require(struct.unpack_from('<4I',exe.read_bytes())==(0xfeedfacf,0x100000c,0,2),'Actual ordinary ARM64 subtype0 own program')
 subprocess.run(['codesign','--verify','--strict',exe],check=True,capture_output=True,timeout=30)
 detail=subprocess.run(['codesign','--display','--verbose=4',exe],check=True,capture_output=True,text=True,timeout=30).stderr
 require(re.search(r'^Page size=4096$',detail,re.M) and build['cdhash'] in detail,'Actual own 4K CodeDirectory')
 signed=plistlib.loads(subprocess.check_output(['codesign','-d','--entitlements',':-',str(exe)],stderr=subprocess.DEVNULL))
 require(signed==build['entitlements']=={'com.apple.private.security.no-container':True,'com.apple.security.iokit-user-client-class':[client,'IOSurfaceRootUserClient']},'Actual complete signed entitlement set '+policy)
 require(modules((folder/'offline-trust.bundle').read_bytes())[1:]==original_trust,'Original trust modules preserved')
 d=load(fixture/'gdb-stop.json');host=load(folder/'host-enabled-report.json');provenance=load(folder/'host-enabled-provenance.json');broker=d['native_gpu_broker']
 require(d['kernel_sha256']==plan['candidate_sha256'] and not d['bounded_timeout_interrupted'] and d['qemu_accelerator']=='hvf' and d['native_4k_mmu_observed'] and not d['sheng_hardware_tested'] and d['original_amfi_table_owner_observed'] and d['original_crypto_initialized_observed'] and d['owner_root_binding_audit']['boot_root_accepted'] and d['actual_signature_failures']==dict(_cs_validate_page_bad_hash=0,_cs_validate_page_no_hash=0) and d['actual_process']['pid']==1 and d['actual_process']['name']=='air-native' and d['scratch_data']['actual_seed_sha256']==data['sha256'] and d['trust_cache_bundle_sha256']==build['trust_bundle_sha256'],'Actual preserved original runtime, AMFI/root/signatures and native CPU4K '+policy)
 policies=d['registered_mac_policies'];require(policies['read_only'] and {r['name'] for r in policies['policies']}=={'AppleImage4','AMFI','Sandbox'},'Actual original registered security policies')
 sandbox=next(r for r in policies['policies'] if r['name']=='Sandbox')
 require(sandbox['configuration']==audit['policy_conf'] and sandbox['operations']==audit['policy_ops'],'Actual original Sandbox policy conf/ops match independent analysis')
 u=d['entry_user_state_candidates'][0];v=struct.unpack('<32Q',bytes.fromhex(u['register_memory']['13:100']['bytes']))
 require(not int(u['cpsr'],16)&31 and u['instruction_bytes'].startswith('000020d4') and v[1:4]==(1,4096,4096),'Actual own EL0 result and native page ABI')
 denied=policy=='unrelated-client';pids=(v[5],v[13]) if denied else (v[4],v[12]);contexts=broker['actual_cpu_contexts']
 require(len(contexts)==2 and {r['pid'] for r in contexts}==set(pids) and len({r['TTBR0_EL1'] for r in contexts})==len({r['task'] for r in contexts})==2,'Two distinct actual worker tasks and TTBR0s')
 for row in contexts:require(row['native_cpu_4k_accepted'] and (int(row['TCR_EL1'],16)>>14)&3==0 and (int(row['TCR_EL1'],16)>>30)&3==2 and int(row['SCTLR_EL1'],16)&1 and int(row['TTBR0_EL1'],16) and int(row['TTBR1_EL1'],16),'Actual active native CPU TG0/TG1 4K and MMU/TTBR state')
 require(broker['stage']==3 and not broker['status'] and broker['clients']==broker['closed']==2 and not broker['active'] and not broker['failures'],'Actual complete native broker cleanup')
 require(not any(d['native_gpu_shared_events'].values()) and not d['native_original_provider']['stage'] and not d['native_original_provider']['allocated'],'No unrelated capability/provider allocation')
 require(provenance['host_exit']==0 and provenance['api_validation_requested'] and provenance['report_sha256']==sha(folder/'host-enabled-report.json') and provenance['candidate_sha256']==plan['candidate_sha256'] and sha(ROOT/'build/host-metal/host-metal-originals')==provenance['process_sha256'],'Exact independent actual host process/report')
 qualification=ROOT/'build/host-metal/qualification.json';require(sha(qualification)==provenance['qualification_sha256'] and sha(ROOT/'kits/sheng/HostMetalDiagnostic.mm')==provenance['source_sha256'],'Actual compiled host qualification and source')
 for name,digest in provenance['backend_sources'].items():require(sha(ROOT/name)==digest,'Exact actual host dependency '+name)
 ops=host['operations'];require(host['transport_healthy'] and host['session_finished'] and host['opened_sessions']==host['closed_sessions']==2 and not host['cpu_pixel_renderer'] and not host['adreno_execution'] and not host['guest_system_device_discovery'] and not host['original_desktop'] and not host['failed_commands'] and not host['open_encoders'] and not host['aborted_encoders'] and not host['pending_submissions'] and len(ops)==broker['requests'] and sum(32+o['result_bytes'] for o in ops)==broker['reply_bytes'],'Independent real host result/lifetime ledger')
 require(all(o['request']==i+1 for i,o in enumerate(ops)),'Exact global host protocol sequence')
 for row in contexts:
  sequence=[o['client_request'] for o in ops if o['session']==row['session']];require(sequence==list(range(1,len(sequence)+1)),'Exact per-worker actual protocol sequence')
 if denied:
  require(v[0]==v[4]==v[12]==0x47504641 and v[6]==v[14]==172 and v[20:23]==(172<<8,172<<8,190) and v[26]==1,'Both actual children refused device access and were reaped')
  require(broker['requests']==4 and broker['reply_bytes']==128 and not broker['refusals'] and not host['completed_commands'] and not host['texture_readbacks'] and all(o['operation'] in (0x140,0x141) and not o['error'] for o in ops),'Original authorization refusal permits only actual namespace create/close, zero GPU commands')
 else:
  require(v[0]==0x47505550 and pids[0]>1 and pids[1]>1 and pids[0]!=pids[1] and v[5:7]==v[13:15]==(4096,4096) and v[7] and v[15] and v[7]!=v[15] and v[8] and v[16] and v[8]!=v[16] and v[9:12]==(4096,0,4) and v[17:23]==(4096,1,4,0,0,1),'Actual successful worker pixels, IOSurface identity, foreign refusal and child exit zero')
  require(broker['requests']==43 and broker['reply_bytes']==67304 and broker['refusals']==1,'Actual complete current broker protocol accounting')
  sessions={r['pid']:r['session'] for r in contexts};errors=[o for o in ops if o['error']]
  require(len(errors)==1 and errors[0]['operation']==0x10d and errors[0]['error']==-2 and errors[0]['session']==sessions[v[12]],'Actual foreign-resource denial in second worker')
  commands=host['completed_commands'];require(len(commands)==3 and all(r['status']==4 and 0<r['gpu_start_ns']<r['gpu_end_ns'] for r in commands),'Three actual GPU completions and real ordered timestamps')
  close=next(o['request'] for o in ops if o['operation']==0x141 and o['session']==sessions[v[4]]);commits=[o for o in ops if o['operation']==0x10c]
  require(commits[2]['session']==sessions[v[12]] and commits[2]['request']>close and ops[-1]['operation']==0x141,'Peer GPU executes after first namespace closes')
  reads=host['texture_readbacks'];require(len(reads)==4 and sum(r['texture']==v[7] for r in reads)==1 and sum(r['texture']==v[15] for r in reads)==3,'Actual complete independent whole-surface readbacks')
  for row in reads:
   require(row['texture'] in (v[7],v[15]),'Actual known worker target')
   pixel=0xff336699 if row['texture']==v[7] else 0xffcc6633
   require(row['bytes']==16384 and row['sha256']==hashlib.sha256(struct.pack('<I',pixel)*4096).hexdigest(),'All actual GPU surface pixels independently verified')
 errors=(folder/'host-enabled-stderr.log').read_text();require('Metal API Validation Enabled' in errors and not any(s in errors for s in ('Validation Error','assertion failed','failed assertion','failed validation','Execution of the command buffer was aborted')),'Actual Metal API Validation enabled and clean')
 codes.append(text_code(exe));processes.append(build['process_sha256']);results.append(dict(policy=policy,allowed=not denied,report_sha256=sha(fixture/'gdb-stop.json'),host_report_sha256=provenance['report_sha256'],process_sha256=build['process_sha256'],requests=broker['requests'],reply_bytes=broker['reply_bytes'],gpu_completions=len(host['completed_commands'])))
require(codes[0]==codes[1]==codes[2] and len(set(processes))==3,'Identical actual own machine instructions, three independently different signed programs')
(ROOT/'build/native-offline-gpu/client-policy-checkpoint.json').write_text(json.dumps(dict(candidate_sha256=plan['candidate_sha256'],native_sha256=plan['native_sha256'],sandbox_analysis_sha256=sha(ROOT/'build/gpu-analysis/original-sandbox-client-class.json'),observations=results,original_policies_preserved=True,identical_own_text=True,system_app_authorization_qualified=False,normal_device_discovery=False,original_desktop=False,adreno_execution=False,active_native_adreno_4k=False),indent=2)+'\n')
print('PASS: actual original Sandbox permits exact/real parent class, refuses unrelated class; identical own code, signed policies, original AMFI/root, native4K worker contexts and real GPU/cleanup controls')
print('Own private offline authorization only; original system apps, normal Metal discovery, desktop and Adreno remain unqualified')
