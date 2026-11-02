#!/usr/bin/env python3
"""Host checks for the trace ABI bounds, the latency store and the decoders."""
import importlib.util
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
spec = importlib.util.spec_from_file_location('trace', Path(__file__).with_name('decode-gpu-trace.py'))
trace = importlib.util.module_from_spec(spec); spec.loader.exec_module(trace)

class TraceTests(unittest.TestCase):
    def test_kernel_store_and_abi_copies(self):
        root = Path(__file__).resolve().parents[1]
        if not (root / 'drivers/Adreno740').is_dir():
            root = root.parent / 'UniXNU'
        driver = root / 'drivers/Adreno740'
        for other in ('GopFramebuffer', 'AdrenoMetal'):
            self.assertEqual((driver / 'NativeGpuTrace.h').read_bytes(),
                             (root / 'drivers' / other / 'NativeGpuTrace.h').read_bytes())
        with tempfile.TemporaryDirectory() as directory:
            exe = str(Path(directory) / 'store')
            subprocess.run(['xcrun','clang++','-std=c++17','-O1','-g','-fsanitize=address,undefined',
                            '-pthread',str(driver / 'NativeGpuTraceTest.cpp'),'-o',exe],check=True)
            subprocess.run([exe],check=True)

    def test_latency_store_and_decoder(self):
        root=Path(__file__).resolve().parents[1]
        source=root/'package/BootLatencyStoreTests.cpp'
        if not source.exists():source=root/'kits/sheng/BootLatencyStoreTests.cpp'
        with tempfile.TemporaryDirectory() as directory:
            exe=str(Path(directory)/'latency')
            subprocess.run(['xcrun','clang++','-std=c++17','-O1','-g','-Wall','-Wextra','-Werror',
                            '-fsanitize=address,undefined','-pthread',str(source),'-o',exe],check=True)
            subprocess.run([exe],check=True)
        raw=struct.pack('<4I',0x3154414c,1,1,0)+trace.LATENCY_RECORD.pack(99,123,35,61,456,0x1e0200000,0x1e2200000,7,8,9)
        head,rows=trace.latency_records(raw)
        self.assertEqual(head['kept'],1)
        self.assertEqual(rows[0]['fields']['start'],0x1e0200000)
        self.assertEqual(rows[0]['name'],'unnest')
        with self.assertRaises(ValueError):trace.latency_records(raw[:-1])
        with self.assertRaises(ValueError):trace.latency_records(b'')

    def test_fault_unwind(self):
        root=Path(__file__).resolve().parents[1]
        package=root/'package'
        if not package.exists():package=root/'kits/sheng'
        with tempfile.TemporaryDirectory() as directory:
            unwind=str(Path(directory)/'unwind')
            subprocess.run(['xcrun','clang','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
                            str(package/'BootLatencyUnwindTests.c'),'-o',unwind],check=True)
            subprocess.run([unwind],check=True)

    def test_counter_plan_matches_pinned_mesa(self):
        import re,xml.etree.ElementTree as ET
        root=Path(__file__).resolve().parents[1]
        if not (root/'drivers/Adreno740').exists():root=root.parent/'UniXNU'
        candidates=[root/'.cache/mesa-work',*root.glob('out/sheng/*/*/tree/.cache/mesa-work')]
        mesa=next((p for p in candidates if p.is_dir()),None)
        if mesa is None:self.skipTest('pinned Mesa sources are not prepared')
        xml=ET.parse(mesa/'src/freedreno/registers/adreno/a6xx.xml')
        values={e.get('name'):int(e.get('value'),0) for e in ET.parse(mesa/'src/freedreno/registers/adreno/a7xx_perfcntrs.xml').iter() if e.tag.endswith('value') and e.get('name') and e.get('value')}
        def register(name):
            choices=[e for e in xml.iter() if e.get('name')==name and (not e.get('variants') or 'A7XX' in e.get('variants'))]
            self.assertEqual(len(choices),1,name)
            return int(choices[0].get('offset'),0),int(choices[0].get('stride','1'),0)
        groups=[('CP_PERFCTR_CP_SEL','RBBM_PERFCTR_CP',13,['CP_ALWAYS_COUNT']),
                ('UCHE_PERFCTR_UCHE_SEL','RBBM_PERFCTR_UCHE',6,['UCHE_STALL_CYCLES_ARBITER','UCHE_VBIF_LATENCY_CYCLES','UCHE_VBIF_LATENCY_SAMPLES','UCHE_VBIF_READ_BEATS_TP','UCHE_VBIF_READ_BEATS_SP','UCHE_READ_REQUESTS_TP']),
                ('TPL1_PERFCTR_TP_SEL','RBBM_PERFCTR_TP',9,['TP_BUSY_CYCLES','TP_L1_CACHELINE_REQUESTS','TP_L1_CACHELINE_MISSES']),
                ('SP_PERFCTR_SP_SEL','RBBM_PERFCTR_SP',17,['SP_BUSY_CYCLES','SP_ALU_WORKING_CYCLES','SP_STALL_CYCLES_TP','SP_FS_STAGE_FULL_ALU_INSTRUCTIONS','SP_ICL1_REQUESTS','SP_ICL1_MISSES','SP_PIXELS']),
                ('RB_PERFCTR_CMP_SEL','RBBM_PERFCTR_CMP',2,['CMPDECMP_VBIF_READ_DATA','CMPDECMP_VBIF_WRITE_DATA'])]
        expected=[]
        for select,counter,start,names in groups:
            sel,sel_stride=register(select);low,stride=register(counter)
            expected.extend((sel+(start+i)*sel_stride,values['A7XX_PERF_'+name],low+(start+i)*stride) for i,name in enumerate(names))
        header=(root/'drivers/Adreno740/NativeGpuPerfCounters.h').read_text()
        actual=[tuple(int(v,0) for v in triple) for triple in re.findall(r'\{(0x[0-9a-f]+),(\d+),(0x[0-9a-f]+)\}',header)]
        self.assertEqual(actual,expected)

    def test_counter_intervals_reject_preemption(self):
        rows=[]
        def add(kind,fields):rows.append(dict(kind=kind,pid=0,us=len(rows),thread=0,fields=fields))
        for sample,epoch in ((1,0),(2,0),(3,1)):
            for first in range(0,len(trace.GPU_COUNTER_NAMES),4):
                add(17,dict(sample=sample,first=first,**{'v'+str(i):sample*100+first+i for i in range(4)}))
            add(18,dict(sample=sample,began_us=sample*100-1,ended_us=sample*100,busy_ticks=10,flags=1 if sample==1 else 0,epoch=epoch))
            add(19,dict(sample=sample,selector_mismatch=0,unstable=0,reset=0,epoch_before=epoch,epoch_after=epoch))
        values=trace.latency_analysis(rows)['gpu_counters']
        self.assertEqual([r['valid'] for r in values],[False,True,False])
        self.assertEqual(values[1]['delta']['cp_clocks'],100)
        result=trace.latency_analysis([dict(kind=26,pid=5,us=1,thread=7,name='fault_image_uuid',uuid='aabb',
            fields=dict(fault=8,image=9,uuid0=0,uuid1=0,index=0,unused=0)),
            dict(kind=28,pid=5,us=2,thread=7,name='thread_wait',fields=dict(capture=10,state=1,wait=20,pc=30,lr=40,switches=50))])
        self.assertEqual(result['faults'][0]['images'][0]['uuid'],'aabb')
        self.assertEqual(result['waits'][0]['thread_wait']['wait'],20)

    def test_detail_schema_and_counters(self):
        root=Path(__file__).resolve().parents[1]
        if not (root/'drivers/Adreno740').exists():root=root.parent/'UniXNU'
        with tempfile.TemporaryDirectory() as directory:
            for name in ('NativeGpuPerfCountersTest.cpp',):
                exe=str(Path(directory)/'perf')
                subprocess.run(['xcrun','clang++','-std=c++17','-Wall','-Wextra','-Werror',
                                '-fsanitize=address,undefined',str(root/'drivers/Adreno740'/name),'-o',exe],check=True)
                subprocess.run([exe],check=True)
        blob=trace.FRAME_RECORD.pack(17,9,100,22,33,44,3|(2<<16),123,0x87654321,1,0,500,77,0)
        row=trace.frontend_details(blob)[0]
        self.assertEqual((row['operation'],row['kind'],row['phase'],row['function']),(22,3,2,0x187654321))
        with self.assertRaises(ValueError):trace.frontend_details(blob[:-1])
        wide=trace.FRAME_RECORD.pack(22,1,100,99,0,0,42,65536,0,0,0,0,0,0)
        self.assertEqual(trace.frontend_details(wide)[0]['argument'],65536)

    def test_turnip_filters_before_transport(self):
        root=Path(__file__).resolve().parents[1]
        if not (root/'patches/mesa').exists():root=root.parent/'UniXNU'
        patch=(root/'patches/mesa/turnip-frame-diagnostics.patch').read_text().split('diff --git')[1]
        header='\n'.join(line[1:] for line in patch.splitlines() if line.startswith('+') and not line.startswith('+++'))
        with tempfile.TemporaryDirectory() as directory:
            source=Path(directory)/'transport.cpp';exe=Path(directory)/'transport'
            (Path(directory)/'tu_frame_diagnostic.h').write_text(header)
            source.write_text('#include <cassert>\n#include "tu_frame_diagnostic.h"\n'+
                'static int calls; extern "C" bool tu_darwin_frame_diagnostic(uint32_t,const void *,uint32_t){++calls;return true;}\n'+
                'int main(){setenv("SHENG_GPU_TRACE","1",1);for(int i=0;i<100000;i++)tu_frame_event(37);assert(calls==0);'+
                'tu_frame_event(36);tu_frame_event(38);tu_frame_event(43);assert(calls==3);}')
            subprocess.run(['xcrun','clang++','-std=c++17','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(source),'-o',str(exe)],check=True)
            subprocess.run([str(exe)],check=True)

    def test_runtime_idle_stage_filter(self):
        import shutil
        root=Path(__file__).resolve().parents[1]
        if not (root/'drivers/AdrenoMetal').exists():root=root.parent/'UniXNU'
        candidates=list(root.glob('out/sheng/*/*/tree/.cache/gpu-reference/metal2vulkan'))
        if not candidates or not shutil.which('rustc'):self.skipTest('prepared runtime and rustc are required')
        relative=Path('crates/metal2vulkan-runtime/src/stage_trace.rs')
        patch=root/'drivers/AdrenoMetal/bridge/patches/0029-latency-stage-selection.patch'
        with tempfile.TemporaryDirectory() as directory:
            directory=Path(directory);source=directory/relative;source.parent.mkdir(parents=True)
            source.write_bytes((candidates[0]/relative).read_bytes())
            subprocess.run(['git','init','-q',str(directory)],check=True)
            subprocess.run(['git','apply',str(patch)],cwd=directory,check=True)
            text=source.read_text();a=text.index('pub fn now_nanos()');b=text.index('/// Whether the host asked',a)
            text=text[:a]+'pub fn now_nanos() -> u64 {1}\n'+text[b:]
            text+='\n#[cfg(test)] mod focus {\n use super::*;\n #[test] fn idle_cannot_evict_commands() {\n  std::env::set_var("SHENG_DIAG","1");\n  for _ in 0..100000 {mark(0,Stage::SchedulerSleep,0);mark(0,Stage::SchedulerResumed,1);}\n  let mut rows=[StageRecord::default();4];assert_eq!(read(0,&mut rows).0,0);\n  mark(5,Stage::CommitCall,0);mark(5,Stage::CompleteBegin,0);mark(5,Stage::CompleteEnd,0);\n  assert_eq!(read(0,&mut rows).0,3);assert_eq!(rows[0].id,5);\n }\n}\n'
            source.write_text(text);exe=directory/'stage-test'
            subprocess.run(['rustc','--edition=2021','--test',str(source),'-o',str(exe)],check=True)
            subprocess.run([str(exe)],check=True)

    def test_focused_capture_schema(self):
        values=[(35,[4,0x1e0200000,0x1e2200000,1,2,3]),(36,[4,0|(9<<8),11,12,13,14]),(8,[4,0,21,22,23,24]),
                (34,[1000000,100,20,20,0,0])]
        raw=struct.pack('<4I',0x3154414c,1000021,len(values),0)
        raw+=b''.join(trace.LATENCY_RECORD.pack(i,7,k,61,*v) for i,(k,v) in enumerate(values))
        head,rows=trace.latency_records(raw)
        self.assertEqual(head['overwritten'],1000000-491519)
        unnest=trace.latency_analysis(rows)['unnests'][0]
        self.assertEqual((unnest['start'],unnest['kernel_depth'],unnest['kernel'][0],unnest['frames'][0]),(0x1e0200000,9,11,21))
        touch=struct.pack('<4I',0x31484354,4,4,0)+struct.pack('<4Q',100,200,2000000,2100000)
        windows=trace.touch_windows(touch)['windows']
        self.assertEqual([(w['begin_us'],w['end_us']) for w in windows],[(100,500200),(2000000,2600000)])
        self.assertTrue(trace.in_windows(500000,windows) and not trace.in_windows(1000000,windows))
        blob=trace.FRAME_RECORD.pack(1,17,100,110,120,130,2,3,4000,0,5,20,24,6)
        frame=trace.frontend_details(blob)[0]
        self.assertEqual((frame['created_us'],frame['committed_us'],frame['completed_us']),(100,110,130))
        blob=trace.FRAME_RECORD.pack(7,0,500,3,1000,1010,3048|(2032<<16),2,44,6193,0,0,0,0)
        frame=trace.frontend_details(blob)[0]
        self.assertEqual((frame['width'],frame['height'],frame['gpu_elapsed_us']),(3048,2032,10))

if __name__=='__main__':unittest.main()
