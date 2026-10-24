#!/usr/bin/env python3
"""Audit the native compiler and all runtime imports against the unchanged target cache."""
import argparse
import hashlib
import json
import mmap
from pathlib import Path
import struct
import subprocess
import sys
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'kits/XnuPrelink'))
from native_prelink import macho
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--diagnostic',action='store_true',help='Audit the independently signed offline compiler instrument')
args=parser.parse_args()
OUT=ROOT/('build/air-nir-diagnostic' if args.diagnostic else 'build/air-nir-ios')

def require(v,why):
    if not v:raise SystemExit(why)
def sha(raw):return hashlib.sha256(raw).hexdigest()
def ordinary_ios(raw,kind):
    parsed=macho(raw)
    versions=[struct.unpack_from('<II',raw,at+8) for cmd,at,_ in parsed['commands'] if cmd==0x32]
    require(parsed['kind']==kind and versions==[(2,0x110000)],'Require actual ordinary ARM64 / iOS17 Mach-O objects')
    return parsed
# Independently inspect every actual LLVM object, not merely build flags or the
# outer archive name. Darwin extended member names are length-prefixed in AR.
llvm_report=json.loads((ROOT/'build/llvm-ios/build-report.json').read_text())
objects=0
for name,info in llvm_report['archives'].items():
    raw=(ROOT/'build/llvm-ios/lib'/name).read_bytes()
    require(sha(raw)==info['sha256'] and raw.startswith(b'!<arch>\n'),'Native LLVM archive identity')
    at=8
    while at<len(raw):
        h=raw[at:at+60];require(len(h)==60 and h[58:60]==b'`\n','Complete archive member header')
        size=int(h[48:58]);at+=60;member=raw[at:at+size];require(len(member)==size,'Complete archive member')
        label=h[:16].decode().strip()
        if label.startswith('#1/'):
            n=int(label[3:]);require(n<=size,'Bounded extended archive name');label=member[:n].rstrip(b'\0').decode();member=member[n:]
        if not label.startswith('__.SYMDEF') and label not in ('/','//'):
            ordinary_ios(member,1);objects+=1
        at+=size+(size&1)
    require(at==len(raw),'Exact archive member coverage')
mesa_objects=0
for obj in (ROOT/'build/mesa-ir3-ios').rglob('*.o'):
    ordinary_ios(obj.read_bytes(),1);mesa_objects+=1
require(mesa_objects>400,'Complete native NIR/IR3 object coverage')
exe=OUT/'air-native' if args.diagnostic else ROOT/'build/mesa-ir3-ios/src/freedreno/ir3/air_nir'
raw=exe.read_bytes();parsed=ordinary_ios(raw,2)
build=json.loads((ROOT/'build/air-nir-ios/build-report.json').read_text())
if args.diagnostic:
    instrument=json.loads((OUT/'build-report.json').read_text())
    require(sha(raw)==instrument['process_sha256'],'Exact signed instrument identity')
    require(sha((ROOT/'kits/sheng/AirNirDiagnostic.cpp').read_bytes())==instrument['source_sha256'],'Exact diagnostic source')
else:require(sha(raw)==build['executable_sha256'] and build['target']=='arm64-apple-ios17.0','Exact native compiler build identity')
for name,digest in build['kit_sources'].items():require(sha((ROOT/'kits/AirNir'/name).read_bytes())==digest,'Native compiler source identity')
loads=[]
for cmd,at,size in parsed['commands']:
    if cmd in (0xc,0x80000018,0x8000001f,0x80000023,0x20):
        off=struct.unpack_from('<I',raw,at+8)[0];require(24<=off<size,'Bounded load-library command')
        end=raw.index(0,at+off,at+size);loads.append(raw[at+off:end].decode())
require(loads==['/usr/lib/libSystem.B.dylib','/usr/lib/libc++.1.dylib'],'No host library or unexpected target framework dependency')
sym=next(at for cmd,at,_ in parsed['commands'] if cmd==2)
symoff,count,stroff,strsize=struct.unpack_from('<4I',raw,sym+8);strings=raw[stroff:stroff+strsize]
require(symoff+count*16<=len(raw) and len(strings)==strsize,'Native symbol table extent')
imports=[]
for i in range(count):
    name,kind,section,desc,value=struct.unpack_from('<IBBHQ',raw,symoff+i*16)
    if kind&0xe0 or (kind&0xe)!=0 or not kind&1:continue
    require(name<len(strings),'Native imported symbol name extent');end=strings.index(0,name)
    text=strings[name:end].decode();ordinal=desc>>8
    require(1<=ordinal<=len(loads) and not value and not section,'Require actual two-level library imports')
    imports.append((text,loads[ordinal-1],bool(desc&0x40)))
require(len(imports)==(371 if args.diagnostic else 367),'Review native compiler runtime import inventory changes: '+str(len(imports)))
# Original ARM64 subtype-0 cache, directly mapped/read-only; no extraction or
# rebasing of target components and no global fallback to unrelated exports.
cache=ROOT/'.cache/firmware/target-volumes/systemos-mount/System/Library/Caches/com.apple.dyld'
handles=[];mappings=[]
for path in sorted(cache.glob('dyld_shared_cache_arm64*')):
    if path.suffix=='.symbols':continue
    stream=path.open('rb');data=mmap.mmap(stream.fileno(),0,access=mmap.ACCESS_READ);handles.append((stream,data))
    require(data[:16].rstrip(b'\0')==b'dyld_v1   arm64','Unchanged target ordinary ARM64 cache profile')
    at,n=struct.unpack_from('<2I',data,16)
    for i in range(n):
        va,size,offset,_,_=struct.unpack_from('<3Q2I',data,at+i*32);mappings.append((va,va+size,offset,data))
main=handles[0][1]
require(main[88:104].hex()=='590d839a82d230f1a17d0cc053420f73' and main[400:416].hex()=='a1a859a2479c31998a34f4afbd808cf5','Exact supplied target cache identities')
def read(va,size):
    matches=[(off+va-start,d) for start,end,off,d in mappings if start<=va and va+size<=end]
    require(len(matches)==1,'Original cache extent must be uniquely backed')
    off,d=matches[0];require(off+size<=len(d),'Original cache file-backed extent');return d[off:off+size]
images={};at,n=struct.unpack_from('<2Q',main,136);require(n<10000,'Bounded target image list')
for i in range(n):
    uuid,va,_,name=struct.unpack_from('<16sQ2I',main,at+i*32);end=main.find(b'\0',name)
    require(name<end<name+4096,'Target image path bound');images[main[name:end].decode()]=(va,uuid)
def uleb(data,at):
    result=shift=0
    while at<len(data) and shift<64:
        b=data[at];at+=1;result|=(b&127)<<shift
        if b<128:return result,at
        shift+=7
    raise SystemExit('Invalid export trie integer')
entries={}
def image(name):
    if name in entries:return entries[name]
    require(name in images,'Original dependency missing: '+name)
    va,uuid=images[name];header=read(va,32);size=struct.unpack_from('<I',header,20)[0]
    require(size<65536,'Bounded original Mach-O commands');blob=header+read(va+32,size);m=macho(blob)
    require(next(blob[p+8:p+24] for c,p,_ in m['commands'] if c==0x1b)==uuid,'Original dependency UUID mismatch')
    link=next(s for s in m['segments'] if s['name']=='__LINKEDIT');export=None;deps=[];reexports=[]
    for cmd,p,length in m['commands']:
        if cmd in (0xc,0x80000018,0x8000001f,0x80000023,0x20):
            off=struct.unpack_from('<I',blob,p+8)[0];end=blob.index(0,p+off,p+length);dep=blob[p+off:end].decode();deps.append(dep)
            if cmd==0x8000001f:reexports.append(dep)
        elif cmd==0x80000033:export=struct.unpack_from('<2I',blob,p+8)
        elif cmd==0x80000022:
            value=struct.unpack_from('<2I',blob,p+40)
            if value[1]:export=value
    require(export is not None,'Original dependency export trie missing')
    require(link['fileoff']<=export[0] and export[0]+export[1]<=link['fileoff']+link['filesize'] and export[1]<16*1024*1024,'Bounded original export extent')
    trie=read(link['va']+export[0]-link['fileoff'],export[1]);exports={};pending=[(0,'')];seen=set()
    while pending:
        p,prefix=pending.pop();require(p not in seen and len(seen)<200000,'Bounded acyclic export trie');seen.add(p)
        length,start=uleb(trie,p);require(start+length<len(trie),'Export terminal bound')
        if length:
            flags,cursor=uleb(trie,start)
            if flags&8:
                ordinal,cursor=uleb(trie,cursor);require(1<=ordinal<=len(deps),'Export reexport ordinal')
                end=trie.index(0,cursor,start+length);alias=trie[cursor:end].decode() or prefix
                exports[prefix]=(deps[ordinal-1],alias)
            else:exports[prefix]=None
        cursor=start+length;children=trie[cursor];cursor+=1
        for _ in range(children):
            end=trie.index(0,cursor);require(end-cursor<4096,'Export edge bound');label=trie[cursor:end].decode();child,cursor=uleb(trie,end+1);pending.append((child,prefix+label))
    entries[name]=dict(uuid=uuid.hex(),exports=exports,reexports=reexports)
    return entries[name]
def resolve(name,symbol,path=()):
    require(len(path)<64,'Bounded library reexport chain')
    if (name,symbol) in path:return None
    entry=image(name)
    if symbol in entry['exports']:
        target=entry['exports'][symbol]
        return name if target is None else resolve(*target,path+((name,symbol),))
    for target in entry['reexports']:
        found=resolve(target,symbol,path+((name,symbol),))
        if found:return found
    return None
found=[];missing=[]
for symbol,owner,weak in imports:
    defining=resolve(owner,symbol)
    (found if defining else missing).append(dict(symbol=symbol,library=owner,defining_library=defining,weak=weak))
result=dict(executable_sha256=sha(raw),ordinary_arm64_subtype=0,platform=2,minimum_ios='17.0',llvm_native_objects=objects,mesa_native_objects=mesa_objects,
    imported_libraries=loads,imports=len(imports),resolved=found,missing=missing,
    target_dependency_uuids={name:value['uuid'] for name,value in entries.items()},
    target_main_cache_uuid=main[88:104].hex(),source_inputs_modified=False,target_runtime_executed=False,
    all_runtime_semantics_qualified=False,metal_or_gpu_execution=False)
(OUT/'target-import-audit.json').write_text(json.dumps(result,indent=2)+'\n')
for stream,data in handles:data.close();stream.close()
if missing:
    print(json.dumps(missing,indent=2));raise SystemExit('Native compiler imports are not all available in supplied target')
print(f'PASS: {objects} actual native LLVM objects and compiler ordinary ARM64/iOS17; all {len(imports)} imports resolve through original target dependency/reexport chains')
print('Target runtime, AIR semantics completeness, GPU execution and Metal/UI remain unqualified')
