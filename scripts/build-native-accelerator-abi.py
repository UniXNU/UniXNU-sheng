#!/usr/bin/env python3
"""Bind an unpublished own lifecycle class to actual original accelerator constructors."""
import hashlib,json,struct,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];OUT=ROOT/'build/native-offline-accelerator';OUT.mkdir(exist_ok=True)
sys.path.insert(0,str(ROOT/'kits/XnuPrelink'));from native_prelink import macho

def run(cmd,**kw):return subprocess.check_output([str(x) for x in cmd],timeout=90,**kw)
def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
print(run(['python3',ROOT/'scripts/audit-target-accelerator-base.py'],text=True),end='')
print(run(['python3',ROOT/'scripts/audit-target-iogpu-userclient.py'],text=True),end='')
plan=json.loads((ROOT/'build/gpu-analysis/ioaccelerator-base-abi.json').read_text())
print(run(['python3',ROOT/'scripts/build-arm-driver.py','NativeOfflineAccelerator'],text=True),end='')
obj=OUT/'NativeOfflineAccelerator.o';raw=obj.read_bytes();im=macho(raw);st=next(p for c,p,_ in im['commands'] if c==2);at,count,strings,_=struct.unpack_from('<4I',raw,st+8);symbols=[]
for i in range(count):
 ix,kind,section,_,va=struct.unpack_from('<IBBHQ',raw,at+i*16);symbols.append((raw[strings+ix:raw.index(b'\0',strings+ix)].decode(),va,kind))
bindings={'__ZN15IOAcceleratorESC2EPK11OSMetaClass':int(plan['protected_compatible_constructor_aliases'][0],16),
 '__ZN15IOAcceleratorESD2Ev':int(plan['nondeleting_destructor'],16),'__ZN15IOAcceleratorES10gMetaClassE':int(plan['metaclass'],16)}
undefined={n for n,_,k in symbols if k==1 and 'IOAcceleratorES' in n}
if undefined!=set(bindings):raise SystemExit('Only actual audited protected base imports may be referenced')
vtable=next(v for n,v,_ in symbols if n=='__ZTV24NativeOfflineAccelerator');relocations={}
for g in im['segments']:
 for sec in g['sections']:
  if sec['va']<=vtable<sec['va']+sec['size']:
   off,n=struct.unpack_from('<2I',raw,sec['command_offset']+56)
   for i in range(n):
    target,bits=struct.unpack_from('<2I',raw,off+i*8)
    if bits>>27&1 and bits>>28==0 and bits>>25&3==3:relocations[sec['va']+target]=symbols[bits&0xffffff][0]
entries=[relocations.get(vtable+16+i*8) for i in range(168)]
if not all(entries) or vtable+16+168*8 in relocations or [i for i,n in enumerate(entries) if 'NativeOfflineAccelerator' in n]!=[0,1,7]:raise SystemExit('Exact 168-entry own table may override only destructor/metaclass entries')
asm=OUT/'original-bindings.S';asm.write_text(''.join(f'.globl {n}\n.set {n}, {a:#x}\n' for n,a in sorted(bindings.items())))
sdk=run(['xcrun','--sdk','iphoneos','--show-sdk-path'],text=True).strip();run(['xcrun','clang','-target','arm64-apple-ios17.0','-isysroot',sdk,'-c',asm,'-o',OUT/'original-bindings.o'])
llvm=Path(run(['brew','--prefix','llvm'],text=True).strip())/'bin';late=OUT/'NativeOfflineAccelerator.late.o'
run([llvm/'llvm-objcopy','--remove-section=__DATA,__mod_init_func','--globalize-symbol=__GLOBAL__sub_I_NativeOfflineAccelerator.cpp',obj,late])
def code(p):
 b=p.read_bytes();s=next(s for g in macho(b)['segments'] for s in g['sections'] if s['name']=='__text');return b[s['fileoff']:s['fileoff']+s['size']]
if code(obj)!=code(late):raise SystemExit('Only owned initializer registration may change')
record=json.loads((ROOT/'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json').read_text().strip().rstrip(','));source=ROOT/'kits/sheng/AcceleratorABIProbe.cpp';cmd=[];args=record['arguments'];i=0
while i<len(args):
 v=args[i]
 if v==record['file']:cmd.append(str(source))
 elif v=='-o':cmd+=['-o',str(OUT/'AcceleratorABIProbe.o')];i+=1
 elif v=='-Xclang' and args[i+1].startswith('-W'):i+=1
 elif not v.startswith('-W'):cmd.append(v)
 i+=1
cmd+=['-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-unnecessary-virtual-specifier','-Wno-c99-designator','-Wno-reorder-init-list','-Wno-deprecated-enum-enum-conversion','-I',str(ROOT/'.cache/xnu-work/osfmk')];run(cmd,cwd=record['directory'])
original_source=ROOT/'kits/sheng/OriginalGPUClientABIProbe.cpp';original_cmd=[str(v).replace(str(source),str(original_source)).replace(str(OUT/'AcceleratorABIProbe.o'),str(OUT/'OriginalGPUClientABIProbe.o')) for v in cmd];run(original_cmd,cwd=record['directory'])
archive=OUT/'libNativeOfflineAccelerator.late.a';archive.unlink(missing_ok=True);run(['xcrun','ar','rcs',archive,late,OUT/'original-bindings.o',OUT/'AcceleratorABIProbe.o',OUT/'OriginalGPUClientABIProbe.o'])
sources=[ROOT/'drivers/AdrenoMetal/NativeOfflineAccelerator.cpp',ROOT/'drivers/AdrenoMetal/OriginalAcceleratorABI.h',source,original_source]
(OUT/'build-report.json').write_text(json.dumps(dict(source_sha256={str(p.relative_to(ROOT)):sha(p) for p in sources},archive_sha256=sha(archive),
 original_abi_sha256=sha(ROOT/'build/gpu-analysis/ioaccelerator-base-abi.json'),original_client_abi_sha256=sha(ROOT/'build/gpu-analysis/original-iogpu-userclient-abi.json'),bindings={n:hex(a) for n,a in bindings.items()},virtual_entries=168,owned_entries=[0,1,7],
 original_instructions_modified=False,system_device_published=False,runtime_qualified=False,original_desktop=False),indent=2)+'\n')
print('PASS: ordinary ARM64/iOS17 unpublished accelerator lifecycle compiles with -Werror; exact original C2/D2/meta imports and 168-entry inherited ABI')
