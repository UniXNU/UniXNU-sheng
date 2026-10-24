#!/usr/bin/env python3
"""Build an unpublished original IOGPU ABI/required-hook instrument, never a driver."""
import hashlib,json,re,struct,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];OUT=ROOT/'build/native-original-provider';OUT.mkdir(exist_ok=True)
sys.path.insert(0,str(ROOT/'kits/XnuPrelink'));from native_prelink import macho,exports

def run(cmd,**kwargs):return subprocess.check_output([str(v) for v in cmd],timeout=90,**kwargs)
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
print(run(['python3',ROOT/'scripts/audit-target-iogpu-provider.py'],text=True),end='')
abi=ROOT/'build/gpu-analysis/original-iogpu-provider-abi.json';plan=json.loads(abi.read_text());print(run(['python3',ROOT/'scripts/build-arm-driver.py','NativeIOGPUProviderProbe'],text=True),end='')
obj=OUT/'NativeIOGPUProviderProbe.o';raw=obj.read_bytes();im=macho(raw);st=next(p for c,p,_ in im['commands'] if c==2);at,count,strings,_=struct.unpack_from('<4I',raw,st+8);symbols=[]
for i in range(count):
 ix,kind,section,_,va=struct.unpack_from('<IBBHQ',raw,at+16*i);symbols.append((raw[strings+ix:raw.index(b'\0',strings+ix)].decode(),va,kind))
public={'__ZN5IOGPU4freeEv':14,'__ZN5IOGPU15configureReportEP19IOReportChannelListjPvS2_':81,'__ZN5IOGPU12updateReportEP19IOReportChannelListjPvS2_':82,'__ZN5IOGPU5startEP9IOService':86,'__ZN5IOGPU4stopEP9IOService':87,'__ZNK5IOGPU11getWorkLoopEv':111,'__ZN5IOGPU13newUserClientEP4taskPvjPP12IOUserClient':141}
bindings={name:plan['inherited_entries'][i] for name,i in public.items()};bindings.update({'__ZN5IOGPUC2EPK11OSMetaClass':plan['protected_constructor'],'__ZN5IOGPUD2Ev':plan['protected_destructor'],'__ZN5IOGPU10gMetaClassE':plan['metaclass']})
for i in range(168,215):
 if i not in plan['pure_virtual_slots']:bindings['__ZN5IOGPU10abiSlot'+str(i)+'Ev']=plan['inherited_entries'][i]
undefined={name for name,_,kind in symbols if kind==1 and re.match(r'__ZNK?5IOGPU',name)}
if undefined!=set(bindings):raise SystemExit('Only exact derived original base imports may be bound: '+repr(undefined^set(bindings)))
cls='NativeIOGPUProviderProbe';vtable=next(v for name,v,_ in symbols if name=='__ZTV'+str(len(cls))+cls);relocations={}
for g in im['segments']:
 for sec in g['sections']:
  if sec['va']<=vtable<sec['va']+sec['size']:
   off,n=struct.unpack_from('<2I',raw,sec['command_offset']+56)
   for i in range(n):
    target,bits=struct.unpack_from('<2I',raw,off+8*i)
    if bits>>27&1 and bits>>28==0 and bits>>25&3==3:relocations[sec['va']+target]=symbols[bits&0xffffff][0]
entries=[relocations.get(vtable+16+8*i) for i in range(215)];owned=sorted([0,1,7,84,*plan['pure_virtual_slots']])
if not all(entries) or vtable+16+215*8 in relocations or [i for i,name in enumerate(entries) if cls in name]!=owned:raise SystemExit('Exact 215-entry own table and only metadata/registration/non-returning pure hooks required')
reference=ROOT/'build/xnu-prelink/native-owner-renderbox-presentation-candidate.macho';reference_raw=reference.read_bytes()
if hashlib.sha256(reference_raw).hexdigest()!=plan['reference_candidate_sha256']:raise SystemExit('Exact original ABI reference candidate required')
native_names={name:address for address,group in exports(reference_raw,macho(reference_raw)).items() for name in group}
for i,name in enumerate(entries):
 if i in owned:continue
 if bindings.get(name,native_names.get(name))!=plan['inherited_entries'][i]:raise SystemExit('Original inherited slot/address was replaced: '+str(i))
# Every required hook captures hardware registers BEFORE any C++ prologue.
# The explicit descriptor refusal restores the genuine caller LR/SP; all
# other hooks trap if their non-returning helper were unexpectedly to return.
for i in [84,*plan['pure_virtual_slots']]:
 name=entries[i];va=next(v for n,v,_ in symbols if n==name);sec=next(s for g in im['segments'] for s in g['sections'] if s['va']<=va<s['va']+s['size']);o=sec['fileoff']+va-sec['va'];words=struct.unpack_from('<'+('20' if i==170 else '19')+'I',raw,o)
 if words[0:2]!=(0xd10203ff,0xa90007e0):raise SystemExit('Actual naked frame prefix differs: '+str(i))
 if i==170:
  if words[15:]!=(0x910003e0,0x94000000,0xf94027fe,0x910203ff,0xd65f03c0):raise SystemExit('Actual descriptor capture/helper/LR/SP/return encoding differs')
 elif words[15]!=0x52800000|(i<<5) or words[16]!=0x910003e1 or words[17]!=0x94000000 or words[18]!=0xd4200020:raise SystemExit('Actual naked frame/slot/helper/trap encoding differs: '+str(i))
asm=OUT/'original-bindings.S';asm.write_text(''.join(f'.globl {name}\n.set {name}, {address:#x}\n' for name,address in sorted(bindings.items())))
sdk=run(['xcrun','--sdk','iphoneos','--show-sdk-path'],text=True).strip();run(['xcrun','clang','-target','arm64-apple-ios17.0','-isysroot',sdk,'-c',asm,'-o',OUT/'original-bindings.o'])
llvm=Path(run(['brew','--prefix','llvm'],text=True).strip())/'bin';late=OUT/'NativeIOGPUProviderProbe.late.o';run([llvm/'llvm-objcopy','--remove-section=__DATA,__mod_init_func','--globalize-symbol=__GLOBAL__sub_I_NativeIOGPUProviderProbe.cpp',obj,late])
def code(p):
 b=p.read_bytes();s=next(s for g in macho(b)['segments'] for s in g['sections'] if s['name']=='__text');return b[s['fileoff']:s['fileoff']+s['size']]
if code(obj)!=code(late):raise SystemExit('Only own initializer registration may change')
record=json.loads((ROOT/'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json').read_text().strip().rstrip(','));source=ROOT/'kits/sheng/OriginalGPUProviderProbe.cpp';cmd=[];args=record['arguments'];i=0
while i<len(args):
 v=args[i]
 if v==record['file']:cmd.append(str(source))
 elif v=='-o':cmd+=['-o',str(OUT/'OriginalGPUProviderProbe.o')];i+=1
 elif v=='-Xclang' and args[i+1].startswith('-W'):i+=1
 elif not v.startswith('-W'):cmd.append(v)
 i+=1
cmd+=['-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-unnecessary-virtual-specifier','-Wno-c99-designator','-Wno-reorder-init-list','-Wno-deprecated-enum-enum-conversion','-I',str(ROOT/'.cache/xnu-work/osfmk')];run(cmd,cwd=record['directory'])
archive=OUT/'libNativeIOGPUProviderProbe.late.a';archive.unlink(missing_ok=True);run(['xcrun','ar','rcs',archive,late,OUT/'original-bindings.o',OUT/'OriginalGPUProviderProbe.o'])
sources=[source,*(ROOT/'drivers/AdrenoMetal'/name for name in ('OriginalGPUProviderABI.h','OriginalAcceleratorABI.h','NativeIOGPUProviderProbe.cpp'))]
(OUT/'build-report.json').write_text(json.dumps(dict(source_sha256={str(p.relative_to(ROOT)):sha(p) for p in sources},archive_sha256=sha(archive),abi_sha256=sha(abi),bindings={name:hex(va) for name,va in bindings.items()},virtual_entries=215,own_entries=owned,vtable_source_symbols=entries,pure_source_signatures_qualified=False,original_methods_modified=False,normal_publication_blocked=True,runtime_qualified=False,original_desktop=False,adreno_execution=False),indent=2)+'\n')
print('PASS: exact original 215-slot inherited entry bindings and 736-byte storage; all ten required hooks and registration save actual registers and NEVER return success')
print('Unpublished introspection only: opaque tail source signatures, successful original start, normal device/client authorization and GPU execution remain unqualified')
