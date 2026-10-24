#!/usr/bin/env python3
"""Verify genuine original RenderBox GPU images, repeated swaps and actual QEMU scanout."""
import hashlib,json,re,struct,subprocess,sys,zlib
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
def rgb_pixels(path):
 raw=path.read_bytes();require(raw[:8]==bytes.fromhex('89504e470d0a1a0a') and struct.unpack_from('>IIBBBBB',raw,16)==(800,600,8,2,0,0,0),'Actual bounded RGB8 QMP PNG')
 at=8;compressed=b''
 while at<len(raw):
  n=struct.unpack_from('>I',raw,at)[0];kind=raw[at+4:at+8];data=raw[at+8:at+8+n]
  require(at+12+n<=len(raw) and zlib.crc32(kind+data)&0xffffffff==struct.unpack_from('>I',raw,at+8+n)[0],'Actual PNG chunk CRC/extent')
  if kind==b'IDAT':compressed+=data
  at+=12+n
 decoded=zlib.decompress(compressed);require(len(decoded)==600*2401,'Actual PNG full-frame extent');previous=bytearray(2400);pixels=bytearray()
 for y in range(600):
  filt=decoded[y*2401];row=bytearray(decoded[y*2401+1:(y+1)*2401]);require(filt<=4,'Actual standard PNG filter')
  for x in range(2400):
   left=row[x-3] if x>=3 else 0;above=previous[x];corner=previous[x-3] if x>=3 else 0
   if filt==4:
    p=left+above-corner;dist=[abs(p-v) for v in (left,above,corner)];predict=(left,above,corner)[dist.index(min(dist))]
   else:predict=(0,left,above,(left+above)//2)[filt]
   row[x]=(row[x]+predict)&255
  pixels+=row;previous=row
 return bytes(pixels)
base=ROOT/'build/xnu-prelink/native-owner-renderbox-presentation-candidate';plan=load(base.with_suffix('.json'));kernel=base.with_suffix('.macho');raw=kernel.read_bytes();segments=macho(raw)['segments']
require(sha(kernel)==plan['candidate_sha256'] and sha(ROOT/'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837.unstripped.noctf')==plan['native_sha256'],'Exact current native and immutable candidate')
old=load(ROOT/'build/xnu-prelink/native-owner-offline-gpu-broker-cpu-candidate.json')
require(plan['components']==old['components'] and plan['original_executable_sha256']==old['original_executable_sha256'] and plan['early_security_initializers']==old['early_security_initializers'],'Same 29 original components and actual security initialization')
for module,digest in plan['original_executable_sha256'].items():
 e=next(e for e in plan['extents'] if e['module']==module and e['segment']=='__TEXT_EXEC');at=backed_offset(segments,e['va'],e['size'],len(raw))
 require(hashlib.sha256(raw[at:at+e['size']]).hexdigest()==digest,'Whole original executable unchanged: '+module)
legacy=load(ROOT/'build/native-legacy-display/build-report.json')
for name,digest in legacy['source_sha256'].items():require(sha(ROOT/name)==digest,'Exact compiled display source: '+name)
require(sha(ROOT/'build/native-legacy-display/libNativeLegacyDisplay.late.a')==legacy['archive_sha256'] and legacy['guarded_backend_leaves']==[211,245,246,306,307,338,345],'Actual display archive and remaining truthful guarded contracts')
original=modules((ROOT/'build/original-static-system-trust/original-static-system.bundle').read_bytes());records=[]
expected_rgba=hashlib.sha256((struct.pack('<I',0xff0000ff)*400+struct.pack('<I',0xffff0000)*400)*600).hexdigest()
expected_bgra=[hashlib.sha256((struct.pack('<I',left)*400+struct.pack('<I',0xff0000ff)*400)*600).hexdigest() for left in (0xffff0000,0xffffffff)]
for frames,disabled in ((1,False),(2,False),(2,True)):
 kind='offline-renderbox-present-'+str(frames);folder=ROOT/'build'/kind;tag='retirement-disabled' if disabled else 'enabled'
 fixture=ROOT/'build'/('darwinboot-owner-hvf-'+kind+('-retirement-disabled' if disabled else ''));path=fixture/'gdb-stop.json';d=load(path)
 build=load(folder/'build-report.json');data=load(folder/'data-report.json');manifest=load(folder/'test-inputs.json')
 for name,digest in manifest['sources'].items():require(sha(ROOT/name)==digest,'Exact original-renderer instrument source: '+name)
 require(sha(folder/'air-native')==build['process_sha256']==data['process_sha256'] and sha(folder/'compiler-data-32.cdr')==data['sha256'] and
  sha(folder/'test-inputs.json')==build['manifest_sha256']==data['manifest_sha256'] and sha(folder/'offline-trust.bundle')==build['trust_bundle_sha256'],'Exact actual signed program, Data, trust and manifest')
 require(struct.unpack_from('<4I',(folder/'air-native').read_bytes())==(0xfeedfacf,0x100000c,0,2),'Actual own ordinary ARM64 process')
 for signed in (folder/'air-native',folder/'NativeOfflineMetal.bundle'):
  subprocess.run(['codesign','--verify','--strict',signed],check=True,capture_output=True,timeout=30)
  detail=subprocess.run(['codesign','--display','--verbose=4',signed],check=True,capture_output=True,text=True,timeout=30).stderr
  require(re.search(r'^Page size=4096$',detail,re.M),'Actual own 4K CodeDirectory')
 require(modules((folder/'offline-trust.bundle').read_bytes())[1:]==original,'Both complete original trust modules unchanged')
 require(d['kernel_sha256']==plan['candidate_sha256'] and not d['bounded_timeout_interrupted'] and d['stop_symbol']=='_exception_triage' and
  d['actual_process']['pid']==1 and d['actual_process']['name']=='air-native' and d['native_4k_mmu_observed'] and d['original_amfi_table_owner_observed'] and
  d['owner_root_binding_audit']['boot_root_accepted'] and d['owner_root_binding_audit']['_native_owner_root_apple_autherr']==80 and
  d['actual_signature_failures']==dict(_cs_validate_page_bad_hash=0,_cs_validate_page_no_hash=0) and d['scratch_data']['actual_seed_sha256']==data['sha256'] and
  d['trust_cache_bundle_sha256']==build['trust_bundle_sha256'],'Real own EL0 boundary, active CPU 4K and preserved original AMFI/root/input identities')
 u=d['entry_user_state_candidates'][0];v=struct.unpack('<32Q',bytes.fromhex(u['register_memory']['13:100']['bytes']))
 require(not int(u['cpsr'],16)&31 and u['instruction_bytes'].startswith('000020d4') and v[:8]==(0x52425052,1,4096,4096,800,600,frames,0 if disabled else frames) and v[24] and v[25]==frames,'Actual original renderer full-frame byte checks and real native API results')
 shown=1 if disabled else frames
 for frame in range(shown):
  row=v[8+frame*8:16+frame*8];require(row[0] and row[1]==frame+1 and row[2:5]==(0,0,0),'Real original swap begin/layer/end and incrementing generation tokens')
  require(row[5:7]==((0xe00002d5,0xffffffffffffffff) if disabled else (0,0)),'Actual original wait returns success only with genuine retirement; control stays NotReady')
 l=d['native_legacy_display'];require(l['stage']==10 and not l['guard_slot'] and l['swap_validation_calls']==shown and l['swap_validation_result']==1 and not l['swap_validation_reason'] and
  l['transfer_calls']==shown and not l['transfer_status'] and l['transfer_bytes']==1920000 and l['retirement_calls']==(0 if disabled else frames),'Actual validated transfers and original successful retirement, without bypassing unknown leaves')
 if not disabled:require(l['retired_generation']==l['presented_generation']==l['retired_frames']==frames and not l['retired_current'] and not l['retired_pending'],'Each actual generation retires and releases its real pending/current reference')
 else:require(l['swap_request'],'Actual unretired request returned NotReady on its first query; no later timeout/discard success is counted')
 broker=d['native_gpu_broker'];require(broker['stage']==3 and broker['status']==0 and broker['clients']==broker['closed']==1 and not broker['active'] and
  broker['requests']==(1287 if frames==1 else 1902) and broker['reply_bytes']==(3953258 if frames==1 else 5892970) and not broker['failures'] and not broker['refusals'],'Actual single private GPU session closes cleanly after all real GPU work')
 contexts=broker['actual_cpu_contexts'];require(len(contexts)==1 and contexts[0]['pid']==1 and contexts[0]['native_cpu_4k_accepted'] and
  int(contexts[0]['TCR_EL1'],16)==0x22a51a251a and int(contexts[0]['SCTLR_EL1'],16)&1 and contexts[0]['task']==d['actual_process']['task'],'Actual client CPU TCR/SCTLR/task; no active GPU-SMMU claim')
 hp=folder/('host-'+tag+'-report.json');host=load(hp);provenance=load(folder/('host-'+tag+'-provenance.json'))
 require(provenance['host_exit']==0 and provenance['report_sha256']==sha(hp) and provenance['candidate_sha256']==plan['candidate_sha256'] and
  sha(ROOT/'build/host-metal/host-metal-originals')==provenance['process_sha256'] and sha(ROOT/'kits/sheng/HostMetalDiagnostic.mm')==provenance['source_sha256'],'Exact actual host executable, diagnostic source and report provenance')
 for name,digest in provenance['backend_sources'].items():require(sha(ROOT/name)==digest,'Actual host backend source: '+name)
 require(host['host_device']=='Apple M1' and host['transport_healthy'] and host['session_finished'] and host['opened_sessions']==host['closed_sessions']==1 and
  not host['cpu_pixel_renderer'] and not host['adreno_execution'] and not host['guest_system_device_discovery'] and not host['original_desktop'] and
  not host['aborted_encoders'] and not host['open_encoders'] and not host['pending_enqueues'] and len(host['operations'])==broker['requests'] and all(not o['error'] for o in host['operations']),'Complete actual host ledger, truthful offline scope and real object cleanup')
 require(len(host['completed_commands'])==len(host['enqueued_commands'])==frames+1 and all(r['status']==4 and 0<r['gpu_start_ns']<r['gpu_end_ns'] for r in host['completed_commands']) and
  all(r['status']==1 for r in host['enqueued_commands']),'Original renderer and GPU format/composition passes really submit and complete')
 readbacks=host['texture_readbacks'];require(len(readbacks)==frames+1 and [r['sha256'] for r in readbacks]==[expected_rgba,*expected_bgra[:frames]] and
  all(r['bytes']==1920000 for r in readbacks),'Every original RGBA, GPU-converted BGRA and clipped composition pixel matches independent full-frame hashes')
 uploads=host['texture_uploads'];require(len(uploads)==600*(frames+1) and sum(r['bytes'] for r in uploads)==1920000*(frames+1) and
  all(r['sha256']==hashlib.sha256(bytes(r['bytes'])).hexdigest() for r in uploads),'All initial surface uploads are zero; expected UI pixels are produced by genuine GPU shaders')
 require(len(host['function_specializations'])==4 and all(r['actual_success'] for r in host['function_specializations']) and
  {r['name'] for r in host['function_specializations']}=={'plane__vertex','plane_color_fragment'} and len(host['texture_bindings'])==frames and
  all(r['texture']==readbacks[0]['texture'] and not r['vertex'] for r in host['texture_bindings']),'Original RenderBox shader specializations and actual original output texture bound to GPU format composition')
 scan=d['gop_scanout_observation'];require(scan['provider']=='original-legacy-gop' and scan['bytes']==1920000,'Actual original display-backed GOP resource')
 video=d['actual_boot_video']['pe_video'];require(l['gop_physical']==video['v_baseAddr'] and l['gop_bytes']==1920000 and
  l['gop_width']==video['v_width']==800 and l['gop_height']==video['v_height']==600 and video['v_rowBytes']==3200,'Exact actual firmware GOP resource/mode binding')
 # Compare actual kernel-mapped samples independently of QMP's whole screen.
 words=[0xffff0000 if x<400 else 0xff0000ff for y in range(600) for x in range(800)]
 if frames==2 and not disabled:words=[0xffffffff if x<400 else 0xff0000ff for y in range(600) for x in range(800)]
 expected_memory=struct.pack('<480000I',*words)
 for sample in scan['captures']:
  offset=int(sample['address'],16)-l['gop_virtual'];observed=bytes.fromhex(sample['bytes'])
  require(0<=offset<=1920000-len(observed) and observed==expected_memory[offset:offset+len(observed)],'Actual CPU mapping samples independently equal GPU-produced displayed bytes')
 pages=[p for span in scan['translations'] for p in span['pages']];require(len(pages)==469 and len({p['virtual_address'] for p in pages})==469,'Every actual scanout page walked once')
 for i,page in enumerate(pages):require(page['leaf_level']==3 and page['leaf_bytes']==4096 and page['ap']==0 and page['pxn'] and page['uxn'] and page['accessed'] and
  int(page['virtual_address'],16)==l['gop_virtual']+4096*i and int(page['physical_address'],16)==l['gop_physical']+4096*i,'Actual active native 4K, kernel-only/NX scanout CPU leaf')
 capture=d['qemu_screen_capture'];screen=fixture/capture['file'];require(sha(screen)==capture['sha256'],'Actual unmodified QMP screen identity')
 left=bytes((255,255,255)) if frames==2 and not disabled else bytes((255,0,0));expected=(left*400+bytes((0,0,255))*400)*600
 require(rgb_pixels(screen)==expected,'All 480000 actual displayed pixels match the real GPU frame, including its second-generation change')
 records.append(dict(frames=frames,retirement_disabled=disabled,report_sha256=sha(path),host_report_sha256=sha(hp),screen_sha256=sha(screen),actual_presented_frames=shown,actual_retired_frames=0 if disabled else frames))
(ROOT/'build/native-legacy-display/renderbox-presentation-checkpoint.json').write_text(json.dumps(dict(candidate_sha256=plan['candidate_sha256'],native_sha256=plan['native_sha256'],observations=records,
 actual_original_renderbox_gpu_images=True,actual_gpu_format_composition=True,actual_repeated_original_swap=True,all_displayed_pixels_checked=480000,active_native_cpu_4k_leaves=469,
 pending_control_not_ready=True,physical_vblank=False,display_fence_completed=False,system_metal=False,original_desktop=False,adreno_execution=False,active_native_adreno_4k=False),indent=2)+'\n')
print('PASS: unchanged original RenderBox -> real GPU format composition -> two real original swaps/retirements; all actual QEMU pixels and 469 active CPU 4K leaves')
print('Retirement-disabled control displays only the first actual frame and returns genuine NotReady; system desktop and Adreno remain open')
