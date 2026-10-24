#!/usr/bin/env python3
"""Build the initial display provider and its explicit offline original-client bridge."""
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys

ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'build/gop-framebuffer'
sys.path.insert(0,str(ROOT/'kits/XnuPrelink'))
from native_prelink import macho,backed_offset,exports
def run(cmd,**kw):return subprocess.check_output([str(x) for x in cmd],timeout=90,**kw)
def require(v,why):
    if not v:raise SystemExit(why)
def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
print(run(['python3',ROOT/'scripts/build-arm-driver.py','NativeMobileDisplay'],text=True),end='')
candidate=ROOT/'build/xnu-prelink/native-owner-mobile-graphics-candidate.macho'
require(sha(candidate)=='aded28d3c61dafaf7b8a7c96b14427a798e9e95470ae6f287bcd038c07626798','Exact qualified original parent candidate')
b=candidate.read_bytes();m=macho(b);names={n:v for v,ns in exports(b,m).items() for n in ns}
def read(v,n):
    at=backed_offset(m['segments'],v,n,len(b));return b[at:at+n]
def branch(pc):
    w=struct.unpack('<I',read(pc,4))[0];require(w&0xfc000000==0x94000000,'Actual original constructor call')
    o=w&0x3ffffff;return pc+((o-(1<<26) if o&(1<<25) else o)*4)
def resolve_stub(v):
    x,y,z=struct.unpack('<3I',read(v,12))
    require(x&0x9f00001f==0x90000010 and y&0xffc003ff==0xf9400210 and z==0xd61f0200,'Actual original import veneer')
    imm=((x>>5)&0x7ffff)<<2|((x>>29)&3)
    if imm&(1<<20):imm-=1<<21
    pointer=(v&~4095)+(imm<<12)+((y>>10)&4095)*8
    return struct.unpack('<Q',read(pointer,8))[0]
require(resolve_stub(branch(0xfffffff00577b348))==names['__ZN9IOServiceC2EPK11OSMetaClass'],'Original protected C2 really calls native IOService C2')
require(resolve_stub(branch(0xfffffff00577b37c))==names['__ZN9IOServiceD2Ev'],'Original base D2 really calls native IOService D2')
# Verify compiled dispatch layout separately from the still-unclassified
# entry signatures. Only three extension entries currently have behavior.
obj=OUT/'NativeMobileDisplay.o';raw=obj.read_bytes();parsed=macho(raw)
at=next(p for cmd,p,_ in parsed['commands'] if cmd==2)
sy,count,st,_=struct.unpack_from('<4I',raw,at+8);symbols=[]
for i in range(count):
    ix,kind,_,_,value=struct.unpack_from('<IBBHQ',raw,sy+i*16)
    symbols.append((raw[st+ix:raw.index(b'\0',st+ix)].decode(),value,kind))
vtable=next(v for n,v,_ in symbols if n=='__ZTV19NativeMobileDisplay');relocs={}
for segment in parsed['segments']:
    for section in segment['sections']:
        if section['va']<=vtable<section['va']+section['size']:
            ro,rc=struct.unpack_from('<II',raw,section['command_offset']+56)
            for i in range(rc):
                address,word=struct.unpack_from('<II',raw,ro+i*8)
                if word>>27&1 and word>>28==0 and word>>25&3==3:relocs[section['va']+address]=symbols[word&0xffffff][0]
compiled=[relocs.get(vtable+16+i*8) for i in range(262)]
require(all(compiled) and vtable+16+262*8 not in relocs,'Exact 262-entry dispatch layout')
own=[]
for i,name in enumerate(compiled):
    if name.startswith(('__ZN19NativeMobileDisplay','__ZNK19NativeMobileDisplay')):own.append(i)
    else:
        require(i<168 and name in names and struct.unpack('<Q',read(0xfffffff005f35da0+i*8,8))[0]==names[name],
                'Actual inherited native parent dispatch entry '+str(i))
require(set(own)==set(range(168,262))|{0,1,7,21},'Only intended base overrides and guarded extension layout')
require('beforeExternalMethod' in compiled[182] and 'releaseClientState' in compiled[193] and 'getDisplayDimensions' in compiled[249],
        'Actual selected provider entry placement')
bindings={'__ZN19IOMobileFramebufferC2EPK11OSMetaClass':0xfffffff00577b318,
          '__ZN19IOMobileFramebufferD2Ev':0xfffffff00577b364,
          '__ZN19IOMobileFramebuffer10gMetaClassE':0xfffffff006b313d8}
undefined={n for n,_,k in symbols if k==1 and 'IOMobileFramebuffer' in n}
require(undefined==set(bindings),'No unqualified original-family imports')
asm=OUT/'original-bindings.S';asm.write_text(''.join(f'.globl {n}\n.set {n}, {v:#x}\n' for n,v in bindings.items()))
sdk=run(['xcrun','--sdk','iphoneos','--show-sdk-path'],text=True).strip()
run(['xcrun','clang','-target','arm64-apple-ios17.0','-isysroot',sdk,'-Werror','-c',asm,'-o',OUT/'original-bindings.o'])
llvm=Path(run(['brew','--prefix','llvm'],text=True).strip())/'bin'
late=OUT/'NativeMobileDisplay.late.o'
run([llvm/'llvm-objcopy','--remove-section=__DATA,__mod_init_func',
     '--globalize-symbol=__GLOBAL__sub_I_NativeMobileDisplay.cpp',obj,late])
late_raw=late.read_bytes();late_parsed=macho(late_raw)
def text(blob,info):
    s=next(s for g in info['segments'] for s in g['sections'] if s['name']=='__text')
    return blob[s['fileoff']:s['fileoff']+s['size']]
require(text(raw,parsed)==text(late_raw,late_parsed) and not any(s['name']=='__mod_init_func' for g in late_parsed['segments'] for s in g['sections']),
        'Only own initializer registration is deferred; own executable instructions unchanged')
recipe=ROOT/'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'
r=json.loads(recipe.read_text().strip().rstrip(','));args=r['arguments'];cmd=[];i=0
source=ROOT/'kits/sheng/MobileDisplayProviderProbe.cpp'
while i<len(args):
    v=args[i]
    if v==r['file']:cmd.append(str(source))
    elif v=='-o':cmd.extend(['-o',str(OUT/'MobileDisplayProviderProbe.o')]);i+=1
    elif v=='-Xclang' and args[i+1].startswith('-W'):i+=1
    elif not v.startswith('-W'):cmd.append(v)
    i+=1
cmd+=['-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-unnecessary-virtual-specifier',
      '-Wno-c99-designator','-Wno-reorder-init-list','-Wno-deprecated-enum-enum-conversion','-I',str(ROOT/'.cache/xnu-work/osfmk')]
run(cmd,cwd=r['directory'])
archive=OUT/'libGopFramebuffer.late.a';archive.unlink(missing_ok=True)
run(['xcrun','ar','rcs',archive,late,OUT/'original-bindings.o',OUT/'MobileDisplayProviderProbe.o'])
sources=[ROOT/'drivers/GopFramebuffer/NativeMobileDisplay.cpp',ROOT/'drivers/GopFramebuffer/OriginalMobileFramebufferABI.h',source]
(OUT/'build-report.json').write_text(json.dumps(dict(
    kit_revision=run(['git','-C',ROOT/'drivers/GopFramebuffer','rev-parse','HEAD'],text=True).strip(),
    source_sha256={str(p.relative_to(ROOT)):sha(p) for p in sources},
    original_parent_candidate_sha256=sha(candidate),compiled_object_sha256=sha(obj),
    late_object_sha256=sha(late),archive_sha256=sha(archive),
    bindings={n:hex(v) for n,v in bindings.items()},compiled_virtual_entries=262,
    implemented_extension_entries=[182,193,249],unclassified_guarded_entries=[i for i in range(168,262) if i not in (182,193,249)],
    original_client_selector=8,original_provider_dimensions_entry=249,
    original_provider_cleanup_entry=193,own_registration_deferred=True,
    original_code_modified=False,runtime_qualified=False,display_published=False,
    presentation=False,gpu_execution=False,original_desktop=False),indent=2)+'\n')
print('Built guarded initial provider and exact original bindings; full display contract remains incomplete')
