#!/usr/bin/env python3
"""Bounded offline integration of real firmware loading and modern HFI startup."""
import hashlib,importlib.util,json,re,struct,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
disassembler=Path(subprocess.check_output(['brew','--prefix','llvm'],text=True).strip())/'bin/llvm-objdump'
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
out=ROOT/'build/sheng-gmu';out.mkdir(exist_ok=True)
report=out/'startup-report.json';report.unlink(missing_ok=True)
current_pins=json.loads((ROOT/'scripts/data/adreno-inputs.json').read_text())
spec=importlib.util.spec_from_file_location('prepare',ROOT/'scripts/prepare-sheng-gmu.py')
module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
out,provenance=module.prepare();
vspec=importlib.util.spec_from_file_location('votes',ROOT/'scripts/prepare-sheng-gmu-votes.py')
vmodule=importlib.util.module_from_spec(vspec);vspec.loader.exec_module(vmodule);vmodule.prepare()
kit=ROOT/'kits/AdrenoFirmware';board=ROOT/'kits/sheng'
votes_report=ROOT/'build/adreno-hfi-votes/report.json'
try:
 evidence=json.loads(votes_report.read_text())
 valid=evidence['passed'] and evidence['oracle_sha256']==sha(ROOT/'build/adreno-hfi-votes/original-votes-oracle.c')
 valid=valid and all(sha(ROOT/n)==v for n,v in evidence['sources'].items())
 valid=valid and all(sha(ROOT/'.cache/adreno-source'/n)==v==current_pins['linux']['files'][n]['sha256'] for n,v in evidence['pinned_sources'].items())
 valid=valid and evidence['linux_revision']==provenance['linux_revision']
except (OSError,KeyError,ValueError):valid=False
if not valid:subprocess.run([sys.executable,ROOT/'scripts/check-adreno-hfi-votes.py'],check=True,timeout=90)

firmware=ROOT/'.cache/firmware/sheng-gpu/qcom/gmu_gen70200.bin'
if sha(firmware)!=provenance['firmware_policy']['sha256']:raise SystemExit('Firmware hash mismatch')
subprocess.run([sys.executable,ROOT/'scripts/prepare-sheng-command-db-oracle.py'],check=True,timeout=30)
sources=[(ROOT/'kits/Sm8550Platform')/'ShengGmu.c',board/'ShengGmuVotes.c',(ROOT/'kits/Sm8550Platform')/'ShengGmuCommandDb.c',kit/'AdrenoFirmware.c',kit/'AdrenoHfi.c',kit/'AdrenoHfiStartup.c',kit/'AdrenoHfiVotes.c']
includes=['-I',str(ROOT/'kits/Sm8550Platform'),'-I',kit,'-I',out,'-I',board]
test=out/'test-startup'
subprocess.run(['xcrun','clang','-std=gnu11','-O2','-Wall','-Wextra','-Werror','-Wno-deprecated-declarations',
 '-fsanitize=address,undefined','-DVOTES_SOURCE_ORACLE','-Wno-sign-compare',*includes,*sources,board/'ShengGmuHostTests.c',board/'ShengGmuVotesHostTests.c',board/'ShengGmuCommandDbHostTests.c',out/'command-db-oracle.c',ROOT/'build/adreno-hfi-votes/original-votes-oracle.c','-o',test],check=True,timeout=60)
r=subprocess.run([test,firmware],check=True,capture_output=True,text=True,timeout=30)
print(r.stdout,end='');(out/'startup-host.log').write_text(r.stdout)
flags=['-target','arm64-apple-ios17.0','-O2','-Wall','-Wextra','-Werror','-Wno-incompatible-sysroot',
 '-ffreestanding','-mgeneral-regs-only',*includes]
objects=[];defined=set();undefined=set()
for source in sources:
 obj=out/(source.stem+'-startup.o')
 subprocess.run(['xcrun','clang',*flags,'-std=c11','-c',source,'-o',obj],check=True,timeout=30)
 raw=obj.read_bytes()
 if struct.unpack_from('<4I',raw)!=(0xfeedfacf,0x100000c,0,1):raise SystemExit('Not ordinary ARM64 object')
 at=32;ios17=False
 for _ in range(struct.unpack_from('<I',raw,16)[0]):
  cmd,size=struct.unpack_from('<II',raw,at)
  if cmd==0x32:ios17=struct.unpack_from('<II',raw,at+8)==(2,0x110000)
  at+=size
 if not ios17:raise SystemExit('Not iOS17 ABI')
 undefined.update(subprocess.check_output(['xcrun','nm','-u',obj],text=True).split())
 symbols=subprocess.check_output(['xcrun','nm','-gU',obj],text=True)
 defined.update(re.findall(r'^[0-9a-f]+ [A-Z] (\S+)$',symbols,re.M))
 asm=subprocess.check_output([disassembler,'-d',obj],text=True)
 operands='\n'.join(re.findall(r'^[ \t]*[0-9a-f]+:[ \t]+[0-9a-f]{8}[ \t]+\w+[ \t]+([^\n]+)',asm,re.M))
 if re.search(r'\b(?:[qds][0-9]+|v[0-9]+(?:\.[0-9a-z]+)?)\b',operands):raise SystemExit('SIMD in native integration')
 objects.append(obj)
analysis=subprocess.run(['xcrun','clang','--analyze','-std=c11','-Wall','-Wextra','-Werror',*includes,*sources,
 '-Xanalyzer','-analyzer-output=text'],capture_output=True,text=True,timeout=60)
if analysis.returncode or analysis.stderr:raise SystemExit('Static analyzer failed: '+analysis.stderr)
(out/'startup-analyzer.log').write_text(analysis.stdout+analysis.stderr)
imports=undefined-defined
if imports-{'___stack_chk_fail','___stack_chk_guard','_memset','_memcpy','_bzero'}:raise SystemExit('Unexpected native imports '+repr(imports))
boundary=out/'startup-boundary.cpp';boundary.write_text('''#include "ShengGmu.h"
#include "ShengGmuVotes.h"
#include "ShengGmuCommandDb.h"
extern "C" ShengGmuDbStatus db_boundary(const uint8_t *p,size_t n,uint32_t v,ShengGmuVoteInputs *i) {return ShengGmuCommandDbInputs(p,n,v,i);}
extern "C" AdrenoHfiStartupStatus vote_boundary(const ShengGmuVoteInputs *i,AdrenoHfiStartupConfig *c) {return ShengGmuBuildStartup(i,c);}
extern "C" ShengGmuStatus cold_boundary(ShengGmuDevice *d,const ShengGmuConfig *c,const AdrenoHfiStartupConfig *h,const ShengGmuStartupLimits *l,ShengGmuStartupResult *r) {return ShengGmuColdStart(d,c,h,l,r);}
''')
consumer=out/'startup-boundary.o';subprocess.run(['xcrun','clang++',*flags,'-std=c++17','-c',boundary,'-o',consumer],check=True,timeout=30)
if set(subprocess.check_output(['xcrun','nm','-u',consumer],text=True).split())!={'_ShengGmuColdStart','_ShengGmuBuildStartup','_ShengGmuCommandDbInputs'}:raise SystemExit('C++/C boundary mismatch')
archive=out/'libShengGmuStartup.a';archive.unlink(missing_ok=True)
subprocess.run(['xcrun','ar','rcs',archive,*objects],check=True,timeout=30)
inputs=sources+[(ROOT/'kits/Sm8550Platform')/'ShengGmu.h',board/'ShengGmuHostTests.c',kit/'AdrenoFirmware.h',kit/'AdrenoHfi.h',kit/'AdrenoHfiStartup.h',kit/'AdrenoHfiVotes.h',(ROOT/'kits/Sm8550Platform')/'ShengGmuVotes.h',(ROOT/'kits/Sm8550Platform')/'ShengGmuCommandDb.h',board/'ShengGmuCommandDbHostTests.c',ROOT/'scripts/prepare-sheng-command-db-oracle.py',board/'ShengGmuVotesHostTests.c',ROOT/'scripts/prepare-sheng-gmu-votes.py',Path(__file__),ROOT/'scripts/prepare-sheng-gmu.py']
report.write_text(json.dumps(dict(passed=True,linux_revision=provenance['linux_revision'],
 votes_source_report_sha256=sha(out/'votes-source-report.json'),votes_oracle_sha256=sha(ROOT/'build/adreno-hfi-votes/original-votes-oracle.c'),source_report_sha256=sha(out/'source-report.json'),firmware_sha256=sha(firmware),
 sources={str(p.relative_to(ROOT)):sha(p) for p in inputs},objects={str(p.relative_to(ROOT)):sha(p) for p in objects},
 command_db_oracle_sha256=sha(out/'command-db-oracle.c'),archive_sha256=sha(archive),boundary_sha256=sha(consumer),remaining_native_imports=sorted(imports),
 complete_startup_commands=7,per_stage_fault_controls=77,load_io_failure_controls=24,
 board_vote_profiles=8,board_vote_refusals=13,command_db_malformed_controls=18,command_db_mutations=4096,
 command_db_all_short_reservations_refused=True,command_db_original_struct_lookup_matched=True,
 host_asan_ubsan=True,clang_static_analysis_clean=True,ordinary_arm64_ios17=True,actual_instructions_without_simd=True,
 software_register_and_ack_peers_only=True,hardware_touched=False,actual_gmu_execution=False,
 active_native4k_gpu_mappings_qualified=False,kernel_integrated=False,hardware_ready=False),indent=2)+'\n')
print('PASS: combined native archive and C++/C boundary; no device access, kernel integration or hardware readiness claim')
