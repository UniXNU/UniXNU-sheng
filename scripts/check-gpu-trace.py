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
