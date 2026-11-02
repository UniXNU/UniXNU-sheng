#!/usr/bin/env python3
"""Host checks for ABI bounds, command/fence joins and unconverted pixel extraction."""
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

    def test_links_and_reuse(self):
        events = [(30,[1,100,1]), (45,[100,200,0]), (42,[300,200,0,1]), (38,[300,7]),
                  (50,[200]), (42,[300,200,0,1]), (38,[300,8])]
        raw = b''.join(trace.RECORD.pack(i,i,e,5,*(v+[0]*(12-len(v)))) for i,(e,v) in enumerate(events,1))
        head = trace.HEADER.pack(0x31545047,1,120,len(events),len(events),0)
        _, rows = trace.records(head+raw)
        links = trace.links(rows)
        self.assertEqual(links[0]['command'],1)
        self.assertEqual(links[0]['fence'],7)
        self.assertIsNone(links[1]['command'])
        self.assertEqual(len(links),2)
        with self.assertRaises(ValueError):trace.records(head+raw[:-1])
        with self.assertRaises(ValueError):trace.records(b'')

    def test_latency_store_and_decoder(self):
        root=Path(__file__).resolve().parents[1]
        source=root/'package/BootLatencyStoreTests.cpp'
        if not source.exists():source=root/'kits/sheng/BootLatencyStoreTests.cpp'
        with tempfile.TemporaryDirectory() as directory:
            exe=str(Path(directory)/'latency')
            subprocess.run(['xcrun','clang++','-std=c++17','-O1','-g','-Wall','-Wextra','-Werror',
                            '-fsanitize=address,undefined','-pthread',str(source),'-o',exe],check=True)
            subprocess.run([exe],check=True)
        raw=struct.pack('<4I',0x3154414c,1,1,0)+trace.LATENCY_RECORD.pack(99,123,2,61,456,789,1,61,0,0)
        head,rows=trace.latency_records(raw)
        self.assertEqual(head['kept'],1)
        self.assertEqual(rows[0]['fields']['cookie'],456)
        self.assertEqual(rows[0]['name'],'ipc_receive')
        with self.assertRaises(ValueError):trace.latency_records(raw[:-1])
        with self.assertRaises(ValueError):trace.latency_records(b'')

    def test_fault_unwind_and_dispatch_hooks(self):
        root=Path(__file__).resolve().parents[1]
        package=root/'package'
        if not package.exists():package=root/'kits/sheng'
        code=root if (root/'drivers/AdrenoMetal').exists() else root.parent/'UniXNU'
        with tempfile.TemporaryDirectory() as directory:
            unwind=str(Path(directory)/'unwind')
            subprocess.run(['xcrun','clang','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
                            str(package/'BootLatencyUnwindTests.c'),'-o',unwind],check=True)
            subprocess.run([unwind],check=True)
            dispatch=str(Path(directory)/'dispatch')
            subprocess.run(['xcrun','clang++','-std=c++17','-fobjc-arc','-Wall','-Wextra','-Werror',
                            '-fsanitize=address,undefined',str(code/'drivers/AdrenoMetal/bridge/dispatch_trace_host.mm'),
                            '-framework','Foundation','-o',dispatch],check=True)
            subprocess.run([dispatch],check=True);subprocess.run([dispatch,'unsupported'],check=True)

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
        raw=struct.pack('<4I',0x3154414c,1,1,0)+trace.LATENCY_RECORD.pack(99,123,20,61,456,2|(3<<8),9,8,7,6)
        _,rows=trace.latency_records(raw)
        self.assertEqual((rows[0]['phase'],rows[0]['flags']),(2,3))

    def test_pixels(self):
        self.assertEqual(trace.PIXEL.size,112)
        h=dict.fromkeys(trace.PIXEL_FIELDS,0)
        h.update(magic=0x31585047,version=1,pid=5,stage=1,capture=9,width=2,height=1,
                 format=80,grid_width=2,grid_height=1,row_bytes=8,bytes_per_pixel=4,
                 flags=9,bytes=8,source_stride=128)
        raw=bytes([255,0,0,255,0,0,255,255])
        collection=struct.pack('<4I',0x31535047,1,1,0)+trace.PIXEL.pack(*(h[k] for k in trace.PIXEL_FIELDS))+raw
        with tempfile.TemporaryDirectory() as name:
            out=Path(name);result=trace.pixels(collection,out)
            self.assertEqual((out/'pixel-5-1-9.raw').read_bytes(),raw)
            self.assertEqual(result['captures'][0]['source_stride'],128)
            self.assertTrue((out/'pixel-5-1-9.png').read_bytes().startswith(b'\x89PNG'))
            with self.assertRaises(ValueError):trace.pixels(collection[:-1],out)
            h['row_bytes']=7
            bad=collection[:16]+trace.PIXEL.pack(*(h[k] for k in trace.PIXEL_FIELDS))+raw
            with self.assertRaises(ValueError):trace.pixels(bad,out)

if __name__=='__main__':unittest.main()
