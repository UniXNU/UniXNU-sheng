#!/usr/bin/env python3
"""Qualify real IR3 semantic linkage and A740 VPC commands without GPU submission."""
import ctypes as C
import hashlib
import json
import os
from pathlib import Path
import shlex
import struct
import subprocess
import sys
import xml.etree.ElementTree as E
ROOT=Path(__file__).resolve().parents[1];OUT=ROOT/'build/adreno-vpc';OUT.mkdir(parents=True,exist_ok=True)
KIT=ROOT/'kits/AdrenoPM4';MESA=ROOT/'.cache/mesa-work';HEAD='85fda3b36405bcaea8bffc64aa4f2651dc90770d';REFERENCE=ROOT/'build/adreno-shader-bindings'
sys.path.insert(0,str(ROOT/'kits/XnuPrelink'));from native_prelink import macho
def run(cmd,**kw):return subprocess.check_output([str(v) for v in cmd],timeout=60,**kw)
def require(value,why):
 if not value:raise RuntimeError(why)
def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
pins=json.loads((ROOT/'scripts/data/adreno-vpc-inputs.json').read_text());require(run(['git','-C',MESA,'rev-parse','HEAD'],text=True).strip()==HEAD,'Pinned Mesa revision')
for name,digest in pins['files'].items():
 require(hashlib.sha256(run(['git','-C',MESA,'show',HEAD+':'+name])).hexdigest()==digest,'Pinned primary shader/link reference')
 if (MESA/name).exists():require(sha(MESA/name)==digest,'Working primary source differs')
# Generate deterministic headers from the exact pinned primary XML inputs.
for name in ('adreno_common','adreno_pm4','a6xx_enums','a7xx_enums','a8xx_enums','a6xx'):
 generated=run([ROOT/'.cache/mesa-venv/bin/python',MESA/'src/freedreno/registers/gen_header.py','--rnn',MESA/'src/freedreno/registers','--xml',MESA/'src/freedreno/registers/adreno'/(name+'.xml'),'c-defines'],env=dict(os.environ,PYTHONHASHSEED='0'))
 (OUT/(name+'.xml.h')).write_bytes(generated)
# Compile the real primary ir3_link_shaders/ir3_link_add helpers with the same
# host Mesa feature definitions, rather than transcribing their algorithms.
build=ROOT/'build/mesa-ir3';commands=run(['ninja','-C',build,'-t','commands','src/freedreno/ir3/air_nir'],text=True).splitlines()
compile=next(shlex.split(x) for x in commands if shlex.split(x)[-1]==str(ROOT/'kits/AirNir/MesaBackend.c'))
for flag in ('-MQ','-MF'):
 i=compile.index(flag);del compile[i:i+2]
compile=[x for x in compile if x not in ('-c','-MD')];compile[compile.index('-o')+1]=str(OUT/'test-vpc');compile[-1]=str(KIT/'test_vpc.c')
compile[1:1]=['-O2','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-DMESA_REFERENCE','-DFD_NO_DEPRECATED_PACK','-fsanitize=address,undefined','-I'+str(KIT),'-I'+str(OUT)]
compile += [str(KIT/'AdrenoVPC.c'),str(KIT/'AdrenoPM4.c')];run(compile,cwd=build)
unit=run([OUT/'test-vpc'],text=True).strip();require(unit.startswith('PASS: 16597 VPC source-reference profiles, 36 transactional refusals,'),'Exact source-reference unit inventory');print(unit)
run(['xcrun','clang','-std=c11','-O2','-Wall','-Wextra','-Werror','-dynamiclib',KIT/'AdrenoPM4.c',KIT/'AdrenoVPC.c','-o',OUT/'VPC.dylib'])
class Output(C.Structure):_fields_=[('identity',C.c_void_p),('bytes',C.c_size_t)]+[(v,C.c_uint32) for v in ('reg','components','bits','half','view','alias')]
class Input(C.Structure):_fields_=[('identity',C.c_void_p),('bytes',C.c_size_t)]+[(v,C.c_uint32) for v in ('components','bits','mask','loc','interp')]
class State(C.Structure):_fields_=[('chip',C.c_uint64),('maxreg',C.c_int32),('reserved',C.c_uint32),('outputs',C.POINTER(Output)),('outcount',C.c_size_t),('inputs',C.POINTER(Input)),('incount',C.c_size_t)]
class Result(C.Structure):_fields_=[(v,C.c_uint32) for v in ('status','words','registers','varyings','stride','position')]
lib=C.CDLL(str(OUT/'VPC.dylib'));encode=lib.AdrenoA740VaryingMap;encode.argtypes=[C.POINTER(State),C.POINTER(C.c_uint32),C.c_size_t];encode.restype=Result
xml=E.parse(MESA/'src/freedreno/registers/adreno/a6xx.xml');ns={'r':xml.getroot().tag.split('}')[0].lstrip('{')};domain=xml.find("r:domain[@name='A6XX']",ns)
def reg(name):
 candidates=[e for e in domain if e.get('name')==name and e.get('offset') and ('A6XX-A7XX' in e.get('variants','') or not e.get('variants'))]
 require(len(candidates)==1,'Unique A740 XML register '+name);return int(candidates[0].get('offset'),0)
def load(prefix):return json.loads(prefix.with_suffix('.interface.json').read_text()),json.loads(prefix.with_suffix('.ir3.json').read_text())
def state_from(vs,fs,chip):
 vi,va=load(vs);fi,fa=load(fs);require(vi['stage']==va['stage']=='vertex' and fi['stage']==fa['stage']=='fragment','Actual stage metadata')
 names=[];outs=[];ins=[]
 for entry in vi['outputs']:
  slot=0 if entry['identity']=='air.position' else 32+entry['slot']-1
  matches=[o for o in va['outputs'] if o['slot']==slot];require(len(matches)==1,'Actual IR3 VS export')
  actual=matches[0];name=entry['identity'].encode();buf=C.create_string_buffer(name);names.append(buf)
  outs.append(Output(C.addressof(buf),len(name),actual['register_id'],entry['components'],32,actual['half'],actual['view'],actual['aliased_components']))
 actual_inputs={i['slot']:i for i in fa['inputs'] if not i['system_value']}
 for entry in fi['inputs']:
  if entry['kind']!='air.fragment_input' or 32+entry['index'] not in actual_inputs:continue
  actual=actual_inputs.pop(32+entry['index']);require(entry['interpolation']=='perspective' and entry['sampling']=='center' and not actual['half'],'Supported actual FS interpolation')
  name=entry['identity'].encode();buf=C.create_string_buffer(name);names.append(buf)
  ins.append(Input(C.addressof(buf),len(name),entry['components'],32,actual['component_mask'],actual['inloc'],0))
 require(not actual_inputs,'Every active IR3 varying has its actual source identity')
 oa=(Output*len(outs))(*outs);ia=(Input*len(ins))(*ins)
 return State(chip,va['max_full_register'],0,oa,len(outs),ia,len(ins)),names,oa,ia
# Decode packets independently, using the source XML map for every address.
def decode(words):
 result={};at=0
 while at<len(words):
  header=words[at];require(header>>28==4,'Only actual type4 linkage registers');count=header&127;address=(header>>8)&0x3ffff
  require(count and at+1+count<=len(words),'Bounded actual VPC packet')
  for i in range(count):require(address+i not in result,'Unique state register');result[address+i]=words[at+1+i]
  at+=1+count
 return result
def encode_pair(vs,fs,expected=0):
 states=[]
 for chip in (0x43050a01,0xffff43050a01):
  state,names,outputs,inputs=state_from(vs,fs,chip);words=(C.c_uint32*128)(*([0x6b6b6b6b]*128));r=encode(C.byref(state),words,128)
  require(r.status==expected,'Actual semantic linkage result')
  if expected:require(list(words)==[0x6b6b6b6b]*128 and not any((r.words,r.registers,r.varyings,r.stride,r.position)),'Whole output preserved on semantic refusal');continue
  states.append(list(words)[:r.words]);require(list(words)[r.words:]==[0x6b6b6b6b]*(128-r.words),'Untouched output tail')
  registers=decode(states[-1]);require(registers[reg('VPC_VS_CNTL')]==r.stride|(r.position<<8)|0xff0000 and registers[reg('SP_VS_OUTPUT_CNTL')]==state.incount+1,'XML actual count/stride/position')
  for i in range(16):require(registers[reg('VPC_VARYING_INTERP_MODE')+i]==0,'Clear all stale smooth/replacement state')
  require(len(registers)==r.registers,'Actual register count')
 if not expected:require(states[0]==states[1],'Both complete FD740 identities have identical linkage words')
 return r,states[-1] if states else []
reference=ROOT/'build/air-nir/observed-identity';qualified=json.loads((reference/'report.json').read_text())['original_shaders'];vertices=[x for x in qualified if x['stage']=='vertex'];constants=[x for x in qualified if x['stage']=='fragment' and x['index'] in (69,73,157)]
original_pairs=[]
for vertex in vertices:
 for fragment in constants:
  vn=f"{vertex['library']}-{vertex['index']}";fn=f"{fragment['library']}-{fragment['index']}";r,words=encode_pair(reference/vn,reference/fn)
  require(r.varyings==0 and r.stride==4 and r.position==0,'Constant fragment linkage still exports actual position')
  original_pairs.append(dict(vertex=vn,fragment=fn,words_sha256=hashlib.sha256(struct.pack('<'+'I'*len(words),*words)).hexdigest()))
llvm=Path(run(['brew','--prefix','llvm'],text=True).strip())/'bin';source=ROOT/'kits/sheng/AdrenoVPCFragment.ll';bc=OUT/'witness.bc';witness=OUT/'witness';run([llvm/'llvm-as',source,'-o',bc]);env=dict(os.environ,IR3_SHADER_DEBUG='asmroundtrip')
meta=json.loads(run([ROOT/'build/mesa-ir3/src/freedreno/ir3/air_nir',bc,'0xffff43050a01',witness,'--compile'],env=env,text=True));require(meta['asm_roundtrip'],'Actual authored FS ISA roundtrip')
r,words=encode_pair(reference/'QuartzCore-7',witness);registers=decode(words)
vi,va=load(reference/'QuartzCore-7');fi,fa=load(witness)
require(r.varyings==2 and r.position==8 and r.stride==12 and fa['binding_state']['input_words']==8,'Actual sparse active FS mask and reordered semantic footprint')
# Independent declaration/IR3 lookup proves coordinate export is first even
# though it occurs after color in the original vertex return metadata.
outputs={v['slot']:v['register_id'] for v in va['outputs']};packed=registers[0xa803]
require(packed&255==outputs[33] and (packed>>16)&255==outputs[32] and (packed>>8)&15==1 and (packed>>24)&15==15,'Actual identities override VS/FS declaration order, preserving subset component mask')
missing=[]
for index in (65,67,75,153,155):
 encode_pair(reference/'QuartzCore-7',reference/f'QuartzCore-{index}',4);missing.append(index)
# Native ordinary ARM64 C/C++ and a real complete helper link witness.
sdk=run(['xcrun','--sdk','iphoneos','--show-sdk-path'],text=True).strip();objects=[];imports={}
for name in ('AdrenoPM4','AdrenoConstants','AdrenoShader','AdrenoVPC'):
 obj=OUT/(name+'.ios.o');run(['xcrun','clang','-target','arm64-apple-ios17.0','-isysroot',sdk,'-std=c11','-O2','-Wall','-Wextra','-Werror','-ffreestanding','-fstack-protector-strong','-c',KIT/(name+'.c'),'-o',obj]);parsed=macho(obj.read_bytes());version=next(at for cmd,at,_ in parsed['commands'] if cmd==0x32);require(parsed['kind']==1 and struct.unpack_from('<II',obj.read_bytes(),version+8)==(2,0x110000),'Actual ordinary ARM64/iOS17 object');objects.append(obj);imports[name]=run(['xcrun','nm','-u',obj],text=True).split()
 require(set(imports[name])<=set(['___stack_chk_fail','___stack_chk_guard','_AdrenoPM4Type4','_AdrenoPM4Type7']),'Bounded native helper imports')
archive=OUT/'libAdrenoPipeline.ios.a';archive.unlink(missing_ok=True);run(['xcrun','ar','rcs',archive,*objects],env=dict(os.environ,ZERO_AR_DATE='1'))
cpp=OUT/'client.cpp';cpp.write_text('#include "AdrenoVPC.h"\n#include "AdrenoShader.h"\nstatic_assert(sizeof(AdrenoVPCOutput)==40 && sizeof(AdrenoVPCInput)==40 && sizeof(AdrenoVPCState)==48 && sizeof(AdrenoVPCResult)==24);\nextern "C" unsigned client(const AdrenoVPCState *v,const AdrenoShaderState *s,const AdrenoShaderMapping *m,const AdrenoConstantConfig *c,const AdrenoConstantSpan *p,const unsigned char *b,unsigned *o){auto a=AdrenoA740VaryingMap(v,o,52);auto d=AdrenoA740ShaderBind(s,m,o+52,24);auto e=AdrenoA740Constants(c,p,1,b,16,o+76,8);return a.status|d.status|e.status;}\n')
obj=OUT/'client.ios.o';run(['xcrun','clang++','-target','arm64-apple-ios17.0','-isysroot',sdk,'-std=c++17','-O2','-Wall','-Wextra','-Werror','-fno-exceptions','-fno-rtti','-I',KIT,'-c',cpp,'-o',obj]);native=OUT/'AdrenoPipeline.ios.dylib';run(['xcrun','clang','-target','arm64-apple-ios17.0','-isysroot',sdk,'-dynamiclib',obj,archive,'-Wl,-undefined,error','-o',native]);parsed_native=macho(native.read_bytes());version=next(at for cmd,at,_ in parsed_native['commands'] if cmd==0x32);require(parsed_native['kind']==6 and struct.unpack_from('<II',native.read_bytes(),version+8)==(2,0x110000),'Actual complete target-native dylib platform')
native_imports=run(['xcrun','nm','-u',native],text=True).split();require(set(native_imports)<=set(['___stack_chk_fail','___stack_chk_guard','dyld_stub_binder']),'Actual complete helper link has no missing symbols')
(OUT/'report.json').write_text(json.dumps(dict(mesa_revision=HEAD,kit_revision=run(['git','-C',KIT,'rev-parse','HEAD'],text=True).strip(),unit=unit,original_pairs=original_pairs,authored_fragment_source_sha256=sha(source),authored_link_words=words,
    reordered_identity_linkage=True,missing_original_vertex_varyings_refused=missing,native_object_imports=imports,native_link_imports=native_imports,native_dylib_sha256=sha(native),
    gpu_execution=False,complete_pipeline_state=False,active_gpu_smmu_qualified=False),indent=2)+'\n')
print('PASS: 42 actual original stage pairs, reordered original-VS/authored-FS identities, missing-varying refusals and native C/C++ full helper link')
