#!/usr/bin/env python3
"""Check actual native iOS17 IOKit/DMA/GPU round-trip and refusal controls."""
import hashlib
import json
import math
from pathlib import Path
import struct
import subprocess
import sys
ROOT=Path(__file__).resolve().parents[1];OUT=ROOT/'build/offline-gpu-transport'
sys.path.insert(0,str(ROOT/'kits/XnuPrelink'))
from native_prelink import macho,exports,backed_offset
def require(v,why):
    if not v:raise SystemExit(why)
def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
build=json.loads((OUT/'build-report.json').read_text());inputs=OUT/'test-inputs.json'
require(sha(inputs)==build['manifest_sha256'],'Exact own instrument source/input manifest')
for name,digest in json.loads(inputs.read_text())['sources'].items():require(sha(ROOT/name)==digest,'Exact own source '+name)
require(sha(OUT/'air-native')==build['process_sha256'] and sha(OUT/'offline-trust.bundle')==build['trust_bundle_sha256'],
        'Exact explicitly signed own process and trust handoff')
require(sha(OUT/'compiler-data-32.cdr')==json.loads((OUT/'data-report.json').read_text())['sha256']
        and json.loads((OUT/'data-report.json').read_text())['process_sha256']==build['process_sha256'],'Exact own mutable Data fixture')
kit=ROOT/'drivers/VirtioMmioConsole';revision=subprocess.check_output(['git','-C',str(kit),'rev-parse','HEAD'],text=True).strip()
require(hashlib.sha256(subprocess.check_output(['git','-C',str(kit),'show',revision+':NativeVirtioConsole.cpp'])).hexdigest()
        ==sha(kit/'NativeVirtioConsole.cpp'),'Exact committed native transport')
candidate=ROOT/'build/xnu-prelink/native-owner-console-bounds-candidate.macho'
plan=json.loads(candidate.with_suffix('.json').read_text());raw=candidate.read_bytes();image=macho(raw)
require(sha(candidate)==plan['candidate_sha256'] and plan['source_inputs_modified'] is False,'Exact native original-component candidate')
for extent in plan['extents']:
    if extent['segment']=='__TEXT_EXEC':
        at=backed_offset(image['segments'],extent['va'],extent['size'],len(raw))
        require(hashlib.sha256(raw[at:at+extent['size']]).hexdigest()==plan['original_executable_sha256'][extent['module']],
                'Complete original component executable unchanged')
def symbols(data,parsed):
    st=next(p for cmd,p,_ in parsed['commands'] if cmd==2)
    offset,count,strings,_=struct.unpack_from('<4I',data,st+8);result=[]
    for i in range(count):
        ix,kind,section,_,va=struct.unpack_from('<IBBHQ',data,offset+i*16)
        if not kind&0xe0 and kind&0xe==0xe and section:
            result.append((data[strings+ix:data.index(b'\0',strings+ix)].decode(),section,va))
    return result
names={name:va for va,group in exports(raw,image).items() for name in group}
for name,_,va in symbols(raw,image):
    if 'NativeVirtioConsole' in name:
        require(name not in names or names[name]==va,'Unambiguous actual linked transport symbol');names[name]=va
obj=(ROOT/'build/virtio-mmio-console/NativeVirtioConsole.o').read_bytes();oi=macho(obj);os=symbols(obj,oi)
object_st=next(p for cmd,p,_ in oi['commands'] if cmd==2)
object_nlist,object_count,object_strings,_=struct.unpack_from('<4I',obj,object_st+8)
sections=[s for segment in oi['segments'] for s in segment['sections']];checks=[]
for name,section,start in os:
    sec=sections[section-1]
    if 'NativeVirtioConsole' not in name or sec['name']!='__text':continue
    end=min([va for _,s,va in os if s==section and va>start]+[sec['va']+sec['size']]);size=end-start
    source=bytearray(obj[sec['fileoff']+start-sec['va']:sec['fileoff']+end-sec['va']])
    require(name in names,'Actual source-compiled driver method is linked '+name)
    at=backed_offset(image['segments'],names[name],size,len(raw));linked=bytearray(raw[at:at+size])
    ro,rc=struct.unpack_from('<II',obj,sec['command_offset']+56);masked=[]
    for j in range(rc):
        offset,bits=struct.unpack_from('<II',obj,ro+j*8);position=sec['va']+offset-start
        if 0<=position<size:
            require(position%4==0 and position+4<=size,'Aligned actual driver instruction relocation')
            kind=bits>>28
            if kind==10:continue # ARM64_RELOC_ADDEND pairs with the actual instruction relocation.
            mask={2:0x03ffffff,3:0x60ffffe0,4:0x003ffc00,5:0x60ffffe0,6:0x003ffc00}.get(kind)
            require(mask is not None,'Known ARM64 instruction relocation kind')
            sw=struct.unpack_from('<I',source,position)[0];lw=struct.unpack_from('<I',linked,position)[0]
            if kind==6 and (sw&0xffc00000)==0xf9400000 and (lw&0xffc00000)==0x91000000:
                # ld64 legitimately relaxes GOT LDR to a direct ADD. Qualify
                # the real paired ADRP/ADD address against the relocation's
                # actual imported symbol before normalizing that one opcode.
                require(position>=4 and bits&(1<<27) and (sw&1023)==(lw&1023),'Exact GOT-load relaxation registers')
                index=bits&0xffffff;require(index<object_count,'Actual GOT symbol index')
                string=struct.unpack_from('<I',obj,object_nlist+index*16)[0]
                target=obj[object_strings+string:obj.index(b'\0',object_strings+string)].decode()
                previous=struct.unpack_from('<I',raw,at+position-4)[0];reg=(lw>>5)&31
                require(previous&0x9f00001f==0x90000000|reg and target in names,'Actual paired ADRP and linked target')
                immediate=((previous>>5)&0x7ffff)<<2|((previous>>29)&3)
                if immediate&(1<<20):immediate-=1<<21
                actual=((names[name]+position-4)&~4095)+(immediate<<12)+((lw>>10)&4095)
                require(actual==names[target],'Actual relaxed GOT import resolves its exact symbol')
                struct.pack_into('<I',source,position,(sw&1023)|0x91000000)
            for data in (source,linked):
                word=struct.unpack_from('<I',data,position)[0]
                struct.pack_into('<I',data,position,word&~mask)
            masked.append(position)
    require(source==linked and size>=4,'Actual linked own instructions '+name)
    checks.append(dict(symbol=name,bytes=size,masked_sha256=hashlib.sha256(source).hexdigest()))
require(len(checks)>=20,'Concrete native driver and userclient methods checked')
observations={}
for case in ('positive','missing-device','default-off'):
    suffix='' if case=='positive' else '-'+case
    path=ROOT/('build/darwinboot-owner-hvf-offline-gpu-transport'+suffix+'/gdb-stop.json')
    d=json.loads(path.read_text());r={k:int(v,16) for k,v in d['system_registers'].items()}
    require(d['kernel_sha256']==plan['candidate_sha256'] and d['qemu_accelerator']=='hvf' and not d['bounded_timeout_interrupted'],
            'Actual bounded same-candidate QEMU observation')
    require(d['native_4k_mmu_observed'] and (r['TCR_EL1']>>14)&3==0 and (r['TCR_EL1']>>30)&3==2
            and r['SCTLR_EL1']&1 and r['TTBR0_EL1'] and r['TTBR1_EL1']
            and set(d['commpage_user64_shifts'].values())=={12},'Actual native 4K CPU translations and user ABI')
    require(d['original_amfi_table_owner_observed'] and d['owner_root_binding_audit']['boot_root_accepted']
            and d['owner_root_binding_audit']['_native_owner_root_apple_autherr']==80
            and d['actual_signature_failures']==dict(_cs_validate_page_bad_hash=0,_cs_validate_page_no_hash=0),
            'Real original AMFI/signature checks and independent owner root, original Apple EAUTH80 preserved')
    g=d['native_console_globals'];exit_code=None
    for name,size in (('ready',4),('tx_requests',4),('rx_requests',4),('failures',4),('tx_bytes',8),('rx_bytes',8),('mmio',8)):
        key='_native_console_'+name;request=f'{names[key]:x}:{size:x}'
        require(int.from_bytes(bytes.fromhex(d['memory'][request]),'little')==g[key],'Actual driver state from real linked symbols')
    if case!='default-off':
        require(d['stop_symbol']=='_exit','Real native process exit')
        exit_code=struct.unpack_from('<I',bytes.fromhex(d['entry_regions']['1:0:8']['bytes']))[0]
    if case=='positive':
        require(exit_code==94 and g['_native_console_ready']==1 and g['_native_console_tx_requests']==2
                and g['_native_console_tx_bytes']==32 and g['_native_console_rx_bytes']==32864
                and g['_native_console_rx_requests']>=10 and not g['_native_console_failures'] and g['_native_console_mmio'],
                'Real signed native request/refusal suite, complete GPU replies and exact success exit')
    else:
        require(not any(g.values()),'Absent device has no readiness, invented replies or queue traffic')
        if case=='missing-device':require(exit_code==97,'Real absent service refusal before IOServiceOpen')
        else:require(d['original_launchd_main_executed'] and d['followed_main_entry']['first_instruction_completed'],'Default-off genuine original launchd main')
    observations[case]=dict(checkpoint_sha256=sha(path),actual_tcr=r['TCR_EL1'],exit=exit_code,console=g)
host=json.loads((OUT/'host-report.json').read_text());host_build=json.loads((ROOT/'build/host-metal/qualification.json').read_text())
require(host_build['source_sha256']==sha(ROOT/'kits/sheng/HostMetalDiagnostic.mm')
        and host_build['process_sha256']==sha(ROOT/'build/host-metal/host-metal-originals'),'Exact observed real host GPU source/process')
require(host['actual_gpu_pixel_bytes_sent']==32768 and not host['cpu_pixel_renderer'] and not host['guest_metal_device']
        and not host['adreno_execution'] and not host['original_desktop'],'Actual host-GPU transport scope')
require(host['original_libraries'][0]['air_slice_sha256']=='c43c90ff007a7d525707ff48c94bc3621f0f2df79db475ce98b8209cd6cfe890'
        and [r['function_count'] for r in host['original_libraries']]==[167,73]
        and all(r['all_module_hashes_checked'] and r['air_slice_only'] and r['all_names_matched'] for r in host['original_libraries']),
        'Exact unchanged original AIR-bearing libraries, no native GPU slice')
for i,row in enumerate(host['transport_observations']):
    expected=0x3f800000 if i==0 else 0
    require(row['original_shader_draw']==bool(i) and row['verified_pixels']==4096 and row['expected_pixel_bits']==expected
            and row['command_status']==4 and all(math.isfinite(row[k]) for k in ('gpu_start','gpu_end'))
            and 0<row['gpu_start']<row['gpu_end']
            and row['readback_sha256']==hashlib.sha256(struct.pack('<I',expected)*4096).hexdigest(),
            'Actual completed host GPU control/draw timestamps and full pixel readback')
require(len(host['transport_observations'])==2,'Exactly two actual GPU operations')
(OUT/'native-checkpoint.json').write_text(json.dumps(dict(candidate_sha256=plan['candidate_sha256'],kit_revision=revision,
    native_process_sha256=build['process_sha256'],source_inputs_sha256=sha(inputs),
    linked_driver_methods=checks,observations=observations,host_report_sha256=sha(OUT/'host-report.json'),
    actual_guest_host_gpu_round_trip=True,actual_gpu_pixel_bytes=32768,native_cpu_page_bytes=4096,
    guest_metal_device=False,display_presentation=False,original_desktop=False,adreno_execution=False,
    active_gpu_smmu_qualified=False,production_storage_integrity=False),indent=2)+'\n')
print('PASS: native iOS17 IOKit/DMA -> actual host GPU original AIR -> full native readback; bounds/device/default-off controls')
print('Original AMFI/owner root/native CPU 4K preserved; guest Metal, presentation, Adreno and desktop remain incomplete')
