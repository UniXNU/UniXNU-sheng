#!/usr/bin/env python3
"""Check real native GPU/IOSurface/GOP transfers and all active CPU map pages."""
import hashlib
import argparse
import json
from pathlib import Path
import re
import struct
import subprocess
import sys

parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--resources',action='store_true');parser.add_argument('--compose',action='store_true');args=parser.parse_args()
if args.resources and args.compose:parser.error('Select composition or resource instrument')
ROOT=Path(__file__).resolve().parents[1];case_kind='offline-gop-composition' if args.compose else 'offline-gop-resources' if args.resources else 'offline-gop-presentation';OUT=ROOT/'build'/case_kind
frame_count=3 if args.resources or args.compose else 2
sys.path.insert(0,str(ROOT/'kits/XnuPrelink'))
from native_prelink import macho,backed_offset,exports
def require(value,reason):
    if not value:raise SystemExit(reason)
def sha(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def read_json(path):return json.loads(Path(path).read_text())
def committed(kit,name):
    revision=subprocess.check_output(['git','-C',ROOT/kit,'rev-parse','HEAD'],text=True).strip()
    data=subprocess.check_output(['git','-C',ROOT/kit,'show',revision+':'+name])
    require(hashlib.sha256(data).hexdigest()==sha(ROOT/kit/name),'Exact committed source '+name)
    return revision

build=read_json(OUT/'build-report.json');manifest=read_json(OUT/'test-inputs.json')
require(sha(OUT/'test-inputs.json')==build['manifest_sha256'] and sha(OUT/'air-native')==build['process_sha256'],'Exact native program/source manifest')
for name,digest in manifest['sources'].items():require(sha(ROOT/name)==digest,'Exact source '+name)
metal_revision=None
for name in ['OfflineMetalFrontend.mm','OfflineMetalWire.h','HostMetalCommandServer.mm','HostMetalCommandServer.h']:
    metal_revision=committed('drivers/AdrenoMetal',name)
display_revision=None
for name in ['NativeGOPPresenter.cpp','NativeGOPWire.h','NativeLegacyDisplay.cpp']:
    display_revision=committed('drivers/GopFramebuffer',name)
subprocess.run(['codesign','--verify','--strict',OUT/'air-native'],check=True,timeout=30)
details=subprocess.run(['codesign','--display','--verbose=4',OUT/'air-native'],check=True,capture_output=True,text=True).stderr
require(re.search(r'^Page size=4096$',details,re.M) and build['cdhash'] in details,'Actual 4K own CodeDirectory')
program=(OUT/'air-native').read_bytes();image=macho(program)
require(image['kind']==2,'Ordinary ARM64 subtype-zero program')
deployment=[struct.unpack_from('<2I',program,p+8) for c,p,_ in image['commands'] if c==0x32]
require(deployment==[(2,0x110000)],'Actual iOS17 target')
data=read_json(OUT/'data-report.json')
require(sha(OUT/'compiler-data-32.cdr')==data['sha256'] and data['process_sha256']==build['process_sha256'] and
        data['own_mutable_data'] and not data['original_os_modified'],'Exact own Data/program fixture')
trust=(OUT/'offline-trust.bundle').read_bytes();original=(ROOT/'build/original-static-system-trust/original-static-system.bundle').read_bytes()
require(hashlib.sha256(trust).hexdigest()==build['trust_bundle_sha256'],'Exact trust bundle')
def modules(b):
    count=struct.unpack_from('<I',b)[0];offsets=struct.unpack_from('<'+str(count)+'I',b,4)
    return [b[a:z] for a,z in zip(offsets,(*offsets[1:],len(b)))]
require(len(modules(trust))==3 and len(modules(original))==2 and modules(trust)[1:]==modules(original),'Original trust modules unchanged')

kernel=ROOT/'build/xnu-prelink/native-owner-gop-present-candidate.macho';plan=read_json(kernel.with_suffix('.json'))
raw=kernel.read_bytes();parsed=macho(raw)
require(sha(kernel)==plan['candidate_sha256'],'Exact native kernel candidate')
require(sha(ROOT/'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837.unstripped.noctf')==plan['native_sha256'],'Current source kernel')
driver=read_json(ROOT/'build/native-legacy-display/build-report.json')
for name,digest in driver['source_sha256'].items():require(sha(ROOT/name)==digest,'Exact compiled display source')
require(sha(ROOT/'build/native-legacy-display/libNativeLegacyDisplay.late.a')==driver['archive_sha256'],'Exact display archive')
for family,digest in plan['original_executable_sha256'].items():
    e=next(e for e in plan['extents'] if e['module']==family and e['segment']=='__TEXT_EXEC')
    at=backed_offset(parsed['segments'],e['va'],e['size'],len(raw))
    require(hashlib.sha256(raw[at:at+e['size']]).hexdigest()==digest,'Whole original component executable unchanged')

# Check actual linked own transfer methods, allowing only their real ARM64
# relocation words to differ from the source-compiled object.
obj=(ROOT/'build/native-gop-presenter/NativeGOPPresenter.o').read_bytes();oi=macho(obj)
st=next(p for c,p,_ in oi['commands'] if c==2);sy,n,strings,_=struct.unpack_from('<4I',obj,st+8);symbols=[]
for i in range(n):
    ix,kind,section,_,va=struct.unpack_from('<IBBHQ',obj,sy+i*16)
    symbols.append((obj[strings+ix:obj.index(b'\0',strings+ix)].decode(),kind,section,va))
linked={name:va for va,ns in exports(raw,parsed).items() for name in ns}
sections=[s for g in oi['segments'] for s in g['sections']];method_checks=[]
for method in ['8transfer','19clientMemoryForType','13presentAction']:
    name,kind,section,start=next(s for s in symbols if method in s[0] and s[1]&0xe==0xe)
    sec=sections[section-1];end=min([s[3] for s in symbols if s[2]==section and s[3]>start]+[sec['va']+sec['size']])
    expected=bytearray(obj[sec['fileoff']+start-sec['va']:sec['fileoff']+end-sec['va']]);at=backed_offset(parsed['segments'],linked[name],len(expected),len(raw));actual=bytearray(raw[at:at+len(expected)])
    ro,rc=struct.unpack_from('<2I',obj,sec['command_offset']+56);masked=set()
    for i in range(rc):
        offset,bits=struct.unpack_from('<2I',obj,ro+i*8);position=sec['va']+offset-start
        if 0<=position<len(expected):
            require(position%4==0 and position+4<=len(expected),'Actual aligned ARM64 relocation')
            expected[position:position+4]=actual[position:position+4]=bytes(4);masked.add(position)
    require(expected==actual and len(expected)>4*len(masked),'Actual linked source-compiled transfer method '+name)
    method_checks.append(dict(symbol=name,bytes=len(expected),relocation_words=sorted(masked),masked_sha256=hashlib.sha256(expected).hexdigest()))

host_build=read_json(ROOT/'build/host-metal/qualification.json')
require(host_build['source_sha256']==sha(ROOT/'kits/sheng/HostMetalDiagnostic.mm') and host_build['process_sha256']==sha(ROOT/'build/host-metal/host-metal-originals'),'Exact actual host backend')
for name,digest in host_build['backend_sources'].items():require(sha(ROOT/name)==digest,'Exact host backend source')
host=read_json(OUT/'host-report.json');ops=host['operations']
require(host['host_device'] and not host['cpu_pixel_renderer'] and not host['guest_system_device_discovery'] and
        not host['adreno_execution'] and not host['original_desktop'],'Actual offline GPU scope')
require(len(ops)==(1876 if args.compose else 1865 if args.resources else 1228) and ops[0]['operation']==0x100 and ops[-1]['operation']==0x10f and
        all(r['request']==i+1 for i,r in enumerate(ops)),'Complete ordered actual GPU ledger')
errors=[(r['operation'],r['error']) for r in ops if r['error']]
require(errors==([(0x119,-2),(0x11c,-2)] if args.compose else []),'Actual unsupported multisampling/invalid clipping refusals only')
require(sum(r['operation']==(0x116 if args.resources else 0x10a) for r in ops)==(5 if args.compose else 2 if args.resources else 1) and sum(r['operation']==0x10e for r in ops)>=6,'Actual original draw and real reference release')
require(len(host['completed_commands'])==frame_count+int(args.resources or args.compose) and len(host['texture_readbacks'])==frame_count+2*int(args.compose),'Actual GPU frames and resource/source work')
color=struct.pack('<I',0xff336699)
rows=[color*800,bytes(3200)] if not args.resources else [color*800,color*400+bytes(1600),bytes(3200)]
readbacks=host['texture_readbacks']
if args.compose:
    blue=struct.pack('<I',0xff0000ff);purple=struct.pack('<I',0xff80007f);constant=struct.pack('<I',0xff2000bf)
    rows=[blue*800,blue*400+purple*400,constant*800]
    for readback,pixel in zip(readbacks[:2],[0xff0000ff,0x80800000]):
        require(readback['bytes']==1920000 and readback['sha256']==hashlib.sha256(struct.pack('<I',pixel)*480000).hexdigest(),'Actual GPU-generated input texture pixels')
    readbacks=readbacks[2:]
for i,(command,readback) in enumerate(zip(host['completed_commands'][int(args.resources or args.compose):],readbacks)):
    require(command['status']==4 and 0<command['gpu_start_ns']<command['gpu_end_ns'] and readback['bytes']==1920000 and
            readback['sha256']==hashlib.sha256(rows[i]*600).hexdigest(),'Actual complete GPU pixels and ordered completion')
    uploads=[u for u in host['texture_uploads'] if u['texture']==readback['texture']]
    input_row=rows[i-1] if i else bytes(3200)
    require(len(uploads)==600 and {u['y'] for u in uploads}==set(range(600)),'Every real native IOSurface row uploaded once')
    require(all(u['x']==0 and u['width']==800 and u['height']==1 and u['bytes']==3200 and
                u['sha256']==hashlib.sha256(input_row).hexdigest() for u in uploads),'Actual original surface/previous GPU output becomes host input')
if args.resources:
    half=[1,0,0,2,0,-1,0,1,0,0,1,0,0,0,0,0,0,0,0,0,0];full=list(half);full[0]=2;full[4]=-1
    half_hash=hashlib.sha256(struct.pack('<21f',*half)).hexdigest();full_hash=hashlib.sha256(struct.pack('<21f',*full)).hexdigest()
    index_hash=hashlib.sha256(struct.pack('<6H',0,1,2,0,2,3)).hexdigest()
    require(len(host['buffer_copies'])==1 and host['buffer_copies'][0]['bytes']==84 and
            host['buffer_copies'][0]['source_offset']==host['buffer_copies'][0]['destination_offset']==0,'Actual GPU buffer copy of original uniform ABI')
    copy=host['buffer_copies'][0];source=copy['source'];destination=copy['destination'];writes=host['buffer_uploads'];reads=host['buffer_readbacks']
    require(len(writes)==len(reads)==6 and all(r['offset']==0 for r in writes+reads),'Exact actual buffer synchronization ledger')
    require([(r['bytes'],r['sha256']) for r in writes]==[(84,half_hash),(84,hashlib.sha256(bytes([0x6b])*84).hexdigest()),
            (84,half_hash),(12,index_hash),(84,full_hash),(12,index_hash)],'Native input, GPU-copy destination and changed-after-encoding uniform bytes')
    require([(r['bytes'],r['sha256']) for r in reads]==[(84,half_hash),(84,half_hash),(84,half_hash),(12,index_hash),(84,full_hash),(12,index_hash)],
            'Actual GPU buffer copy/readback and geometry-dependent uniform/index buffers')
    require(writes[0]['buffer']==reads[0]['buffer']==source and all(reads[i]['buffer']==destination for i in [1,2,4]) and
            writes[1]['buffer']==writes[2]['buffer']==writes[4]['buffer']==destination,'The actual copied buffer becomes the actual vertex input')
    command=host['completed_commands'][0]
    require(command['status']==4 and 0<command['gpu_start_ns']<command['gpu_end_ns'] and sum(r['operation']==0x118 for r in ops)==1,'Actual completed blit GPU work')
if args.compose:
    require(len(host['blend_pipelines'])==2 and all(r['vertex']=='read_surf_vert' and r['fragment']=='read_surf_frag' for r in host['blend_pipelines']),
            'Actual original texture-read shaders with two real blend pipelines')
    require(len(host['texture_bindings'])==5 and all(not r['vertex'] and r['index']==0 for r in host['texture_bindings']) and
            [r['texture'] for r in host['texture_bindings']]==[host['texture_readbacks'][i]['texture'] for i in [0,0,1,0,1]],'Actual GPU source textures bound for real fragment reads')
    require(sum(r['operation']==0x11d for r in ops)==3 and sum(r['operation']==0x11c and not r['error'] for r in ops)==4 and
            sum(r['operation']==0x11e for r in ops)==1,'Actual viewport, clipping and constant blend state')
    command=host['completed_commands'][0];require(command['status']==4 and 0<command['gpu_start_ns']<command['gpu_end_ns'],'Actual source-texture GPU work')

observations={}
for case in ['positive','missing-device','default-off']:
    suffix='' if case=='positive' else '-'+case
    path=ROOT/('build/darwinboot-owner-hvf-'+case_kind+suffix+'/gdb-stop.json');d=read_json(path)
    require(d['kernel_sha256']==plan['candidate_sha256'] and not d['bounded_timeout_interrupted'] and d['qemu_accelerator']=='hvf','Same bounded actual candidate')
    regs={k:int(v,16) for k,v in d['system_registers'].items()}
    require(d['native_4k_mmu_observed'] and (regs['TCR_EL1']>>14)&3==0 and (regs['TCR_EL1']>>30)&3==2 and
            regs['SCTLR_EL1']&1 and regs['TTBR0_EL1'] and regs['TTBR1_EL1'] and set(d['commpage_user64_shifts'].values())=={12},'Active CPU/native user 4K')
    require(d['trust_cache_bundle_sha256']==build['trust_bundle_sha256'] and d['scratch_data']['input']['sha256']==data['sha256'] and
            d['scratch_data']['actual_seed_sha256']==data['sha256'],'Exact current program/trust/Data in every case')
    require(d['original_amfi_table_owner_observed'] and d['owner_root_binding_audit']['boot_root_accepted'] and
            d['owner_root_binding_audit']['_native_owner_root_apple_autherr']==80 and
            d['actual_signature_failures']==dict(_cs_validate_page_bad_hash=0,_cs_validate_page_no_hash=0),'Original AMFI/Apple EAUTH and owner-root enforcement')
    console=d['native_console_globals'];gop=d['native_gop_presenter'];video=d['actual_boot_video']['pe_video'];code=None
    if case!='default-off':
        require(d['stop_symbol']=='_exit','Actual native exit')
        code=struct.unpack_from('<I',bytes.fromhex(d['entry_regions']['1:0:8']['bytes']))[0]
        require(gop['ready']==1 and gop['physical']==video['v_baseAddr'] and not gop['failures'],'Real firmware presenter ready')
        legacy=d['native_legacy_display']
        require(legacy['stage']==8 and legacy['released']==1 and not legacy['start_result'] and not legacy['guard_slot'],'Incomplete original display stays unpublished')
    if case=='positive':
        require(code==(167 if args.compose else 163 if args.resources else 161) and console['_native_console_ready']==1 and not console['_native_console_failures'] and
                console['_native_console_tx_requests']==len(ops) and console['_native_console_tx_bytes']==sum(16+r['input_bytes'] for r in ops) and
                console['_native_console_rx_bytes']==sum(32+r['result_bytes'] for r in ops),'Actual full GPU/scanout program and byte-exact transport ledger')
        require(gop['transfers']==frame_count and gop['bytes']==frame_count*1920000 and video['v_width']==800 and video['v_height']==600 and video['v_rowBytes']==3200,'Actual full firmware framebuffer transfers')
        scan=d['gop_scanout_observation'];sample=struct.pack('<I',0xff2000bf)*1024 if args.compose else bytes(4096)
        require(scan['bytes']==1920000 and all(bytes.fromhex(r['bytes'])==sample for r in scan['captures']),'Independent actual final scanout sample bytes')
        pages=[p for span in scan['translations'] for p in span['pages']];base=gop['virtual'];physical=gop['physical']
        require(base%4096==0 and len(pages)==469 and {int(p['virtual_address'],16) for p in pages}==set(range(base,base+1920000,4096)),'Every actual framebuffer mapping page observed')
        require(all(p['leaf_level']==3 and p['leaf_bytes']==4096 and p['ap']==0 and p['pxn'] and p['uxn'] and p['accessed'] and
                    int(p['physical_address'],16)==physical+int(p['virtual_address'],16)-base for p in pages),'Actual native 4K NX CPU mapping points to real GOP physical memory')
    else:
        require(not any(console.values()) and not gop['transfers'] and not gop['bytes'],'No fabricated GPU traffic/presentation without transport')
        if case=='missing-device':require(code==128,'Absent actual GPU refuses before presentation')
        else:require(not any(gop.values()) and d['original_launchd_main_executed'] and d['followed_main_entry']['first_instruction_completed'],'Default-off original launchd and no presenter')
    observations[case]=dict(checkpoint_sha256=sha(path),exit=code,gop=gop,actual_tcr=regs['TCR_EL1'])

(OUT/'native-checkpoint.json').write_text(json.dumps(dict(candidate_sha256=plan['candidate_sha256'],native_sha256=plan['native_sha256'],
    metal_revision=metal_revision,display_revision=display_revision,process_sha256=build['process_sha256'],host_report_sha256=sha(OUT/'host-report.json'),
    observations=observations,linked_method_checks=method_checks,actual_gpu_surface_to_gop_transfer=True,actual_scanout_cpu_pages=469,
    transfer_bytes=frame_count*1920000,actual_buffer_resources=args.resources,actual_gpu_texture_composition=args.compose,hardware_vblank_qualified=False,zero_copy=False,original_display_published=False,
    system_metal_device=False,original_desktop=False,adreno_execution=False,active_gpu_smmu_qualified=False),indent=2)+'\n')
print('PASS: real native GPU -> original IOSurface -> actual 800x600 GOP, '+str(frame_count)+' full transfers/readback; all 469 CPU pages native 4K')
print('Absent/default controls and original AMFI/root pass; system Metal/display integration, desktop and Adreno remain incomplete')
