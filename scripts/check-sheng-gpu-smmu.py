#!/usr/bin/env python3
"""Offline board SMMU lifecycle: real production code, software register peers."""
import hashlib,importlib.util,json,re,struct,subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];kit=ROOT/'kits/ArmSmmu4K';board=ROOT/'kits/sheng'
out=ROOT/'build/sheng-gpu-smmu';out.mkdir(exist_ok=True);(out/'report.json').unlink(missing_ok=True)
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
spec=importlib.util.spec_from_file_location('prepare',ROOT/'scripts/prepare-sheng-gpu-smmu.py')
module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module);_,provenance=module.prepare()
sources=[kit/(n+'.c') for n in ['ArmSmmu4K','ArmSmmu4KContext','ArmSmmu4KProgram','ArmSmmu4KStreams','ArmSmmu4KMmio','ArmSmmu4KAudit','ArmSmmu4KTableMemory','ArmSmmu4KLive']]+[(ROOT/'kits/Sm8550Platform')/'ShengGpuSmmu.c']
includes=['-I',str(ROOT/'kits/Sm8550Platform'),'-I',kit,'-I',board,'-I',out];test=out/'test-lifecycle'
subprocess.run(['xcrun','clang','-std=c11','-O2','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',*includes,*sources,board/'ShengGpuSmmuHostTests.c','-o',test],check=True,timeout=60)
r=subprocess.run([test],capture_output=True,text=True,timeout=30)
print(r.stdout,end='')
if r.returncode:raise SystemExit(r.stderr)
(out/'host.log').write_text(r.stdout)
counts=re.search(r'(\d+) startup and (\d+) stop I/O failures',r.stdout)
physical_counts=re.search(r'(\d+) physical-read failures, (\d+) during-walk',r.stdout)
live_counts=re.search(r'(\d+) live-start and (\d+) live-stop I/O failures, (\d+) reserved-graph mutations',r.stdout)
if not counts or not physical_counts or not live_counts:raise SystemExit('Missing actual failure coverage')
flags=['-target','arm64-apple-ios17.0','-O2','-Wall','-Wextra','-Werror','-Wno-incompatible-sysroot','-ffreestanding','-mgeneral-regs-only',*includes]
objdump=Path(subprocess.check_output(['brew','--prefix','llvm'],text=True).strip())/'bin/llvm-objdump'
objects=[];provided=set();imports=set()
for source in sources:
 obj=out/(source.stem+'.o');subprocess.run(['xcrun','clang',*flags,'-std=c11','-c',source,'-o',obj],check=True,timeout=30)
 raw=obj.read_bytes()
 if struct.unpack_from('<4I',raw)!=(0xfeedfacf,0x100000c,0,1):raise SystemExit('Ordinary ARM64 required')
 at=32;ios17=False
 for _ in range(struct.unpack_from('<I',raw,16)[0]):
  cmd,size=struct.unpack_from('<II',raw,at)
  if cmd==0x32:ios17=struct.unpack_from('<II',raw,at+8)==(2,0x110000)
  at+=size
 if not ios17:raise SystemExit('iOS17 ABI required')
 imports.update(subprocess.check_output(['xcrun','nm','-u',obj],text=True).split())
 provided.update(re.findall(r'^[0-9a-f]+ [A-Z] (\S+)$',subprocess.check_output(['xcrun','nm','-gU',obj],text=True),re.M))
 asm=subprocess.check_output([objdump,'-d',obj],text=True)
 operands='\n'.join(re.findall(r'^[ \t]*[0-9a-f]+:[ \t]+[0-9a-f]{8}[ \t]+\w+[ \t]+([^\n]+)',asm,re.M))
 if re.search(r'\b(?:[qds][0-9]+|v[0-9]+(?:\.[0-9a-z]+)?)\b',operands):raise SystemExit('SIMD found')
 objects.append(obj)
imports-=provided
if imports-{'___stack_chk_fail','___stack_chk_guard','_memset','_memcpy','_bzero'}:raise SystemExit('Unexpected imports '+repr(imports))
boundary=out/'boundary.cpp';boundary.write_text('''#include "ShengGpuSmmu.h"
extern "C" ArmSmmu4KStatus board_smmu_boundary(const ArmSmmu4KIo *io,uint64_t pa,unsigned bank,const unsigned *slots,unsigned count,const ArmSmmu4KContextConfig *c,const ArmSmmu4KAuditConfig *a,ShengGpuSmmuSession *s,ShengGpuSmmuResult *r,unsigned operation) {
 if(operation==1)return ShengGpuSmmuObserve(io,pa,s,r);
 if(operation==2)return ShengGpuSmmuStop(io,pa,1000,s,r);
 return ShengGpuSmmuStart(io,pa,ShengGpuSmmuGmu,bank,slots,count,c,a,1000,s,r);
}
''')
consumer=out/'boundary.o';subprocess.run(['xcrun','clang++',*flags,'-std=c++17','-c',boundary,'-o',consumer],check=True,timeout=30)
if set(subprocess.check_output(['xcrun','nm','-u',consumer],text=True).split())!={'_ShengGpuSmmuStart','_ShengGpuSmmuObserve','_ShengGpuSmmuStop'}:raise SystemExit('C++/C boundary mismatch')
live_boundary=out/'live-boundary.cpp';live_boundary.write_text('''#include "ShengGpuSmmu.h"
extern "C" ArmSmmu4KStatus live_board_boundary(const ArmSmmu4KIo *io,uint64_t pa,unsigned bank,const unsigned *slots,unsigned count,const ArmSmmu4KContextConfig *c,const ArmSmmu4KLiveArena *a,ArmSmmu4KLiveState *state,ShengGpuSmmuSession *s,ShengGpuSmmuResult *r,unsigned operation) {
 if(operation)return ShengGpuSmmuObserveLive(io,pa,s,nullptr,0,r);
 return ShengGpuSmmuStartLive(io,pa,ShengGpuSmmuGmu,bank,slots,count,c,a,state,1000,s,r);
}
''')
live_consumer=out/'live-boundary.o';subprocess.run(['xcrun','clang++',*flags,'-std=c++17','-c',live_boundary,'-o',live_consumer],check=True,timeout=30)
if set(subprocess.check_output(['xcrun','nm','-u',live_consumer],text=True).split())!={'_ShengGpuSmmuStartLive','_ShengGpuSmmuObserveLive'}:raise SystemExit('Live C++/C boundary mismatch')
analysis=subprocess.run(['xcrun','clang','--analyze','-std=c11',*includes,*sources,'-Xanalyzer','-analyzer-output=text'],capture_output=True,text=True,timeout=60)
if analysis.returncode or analysis.stderr:raise SystemExit('Analyzer diagnostics: '+analysis.stderr)
archive=out/'libShengGpuSmmu.a';archive.unlink(missing_ok=True);subprocess.run(['xcrun','ar','rcs',archive,*objects],check=True,timeout=30)
inputs=sources+[s.with_suffix('.h') for s in sources]+[board/'ShengGpuSmmuHostTests.c',Path(__file__),ROOT/'scripts/prepare-sheng-gpu-smmu.py']
(out/'report.json').write_text(json.dumps(dict(passed=True,linux_revision=provenance['linux_revision'],source_report_sha256=sha(out/'source-report.json'),
 sources={str(p.relative_to(ROOT)):sha(p) for p in inputs},objects={p.name:sha(p) for p in objects},archive_sha256=sha(archive),boundary_sha256=sha(consumer),
 native_imports=sorted(imports),startup_io_failure_controls=int(counts[1]),stop_io_failure_controls=int(counts[2]),
 physical_read_failures=int(physical_counts[1]),during_walk_mutations=int(physical_counts[2]),
 live_startup_io_failures=int(live_counts[1]),live_stop_io_failures=int(live_counts[2]),live_graph_mutations=int(live_counts[3]),live_boundary_sha256=sha(live_consumer),
 host_asan_ubsan=True,ordinary_arm64_ios17=True,actual_instructions_without_simd=True,static_analysis_clean=True,
 context_disable_requires_faulted_streams=True,physical_graph_audit_required=True,observed_ttbr_roots=True,software_peer_only=True,kernel_integrated=False,active_hardware_mapping_qualified=False,hardware_ready=False),indent=2)+'\n')
print('PASS: board GPU/GMU lifecycle archive and C++/C boundary; no device operation or active hardware proof')
