#!/usr/bin/env python3
"""Qualify an offline host-GPU route using only unchanged original AIR slices."""
import hashlib
import argparse
import json
import math
from pathlib import Path
import struct
import subprocess
parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--bgra',action='store_true');args=parser.parse_args()
ROOT=Path(__file__).resolve().parents[1];OUT=ROOT/('build/host-metal-bgra' if args.bgra else 'build/host-metal');OUT.mkdir(parents=True,exist_ok=True)
def run(cmd,**kw):return subprocess.run([str(x) for x in cmd],check=True,timeout=60,**kw)
def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def require(v,why):
    if not v:raise SystemExit(why)
base=ROOT/'.cache/firmware/target-volumes/os-mount/System/Library'
paths=[base/'Frameworks/QuartzCore.framework/default.metallib',base/'PrivateFrameworks/RenderBox.framework/default.metallib']
expected=['606f913adf520420f9b4bb666efb782a68c830f082f1e559e17b20d0d13adbc9','d76883f626b1527c36fb0940cf27dcade503a55cf2fa05c8f7a247a40ccebec4']
require([sha(p) for p in paths]==expected,'Both exact original OS libraries')
source=ROOT/'kits/sheng/HostMetalDiagnostic.mm';kit=ROOT/'kits/AirNir'
obj=OUT/'Library.o';exe=OUT/'host-metal-originals'
run(['xcrun','clang','-std=c11','-O2','-Wall','-Wextra','-Werror','-c',kit/'Library.c','-o',obj])
run(['xcrun','clang++','-std=c++17','-O2','-Wall','-Wextra','-Werror','-Wno-deprecated-declarations',
     '-fobjc-arc','-I',kit,'-I',ROOT/'drivers/AdrenoMetal',source,ROOT/'drivers/AdrenoMetal/HostMetalCommandServer.mm',obj,'-framework','Foundation','-framework','Metal','-o',exe])
report_path=OUT/'original-air-gpu-report.json'
with report_path.open('w') as output:run([exe,*paths,*(['--bgra'] if args.bgra else [])],stdout=output)
d=json.loads(report_path.read_text())
require(d['host_device'] and not d['guest_device_created'] and not d['adreno_execution']
        and not d['original_desktop'] and not d['cpu_pixel_renderer'],'Host-only real Metal feasibility scope')
for i,(row,path,count) in enumerate(zip(d['original_libraries'],paths,(167,73))):
    raw=path.read_bytes();at=row['air_slice_offset'];size=row['air_slice_bytes']
    require(row['input_sha256']==expected[i] and row['input_bytes']==len(raw)
            and row['function_count']==count and row['air_slice_only']
            and row['all_names_matched'] and row['all_module_hashes_checked']
            and hashlib.sha256(raw[at:at+size]).hexdigest()==row['air_slice_sha256'],
            'Actual original AIR-bearing slice and all function identities/hashes')
require(d['original_libraries'][0]['air_slice_offset']==48 and d['original_libraries'][0]['air_slice_bytes']==1255616
        and d['original_libraries'][0]['air_slice_sha256']=='c43c90ff007a7d525707ff48c94bc3621f0f2df79db475ce98b8209cd6cfe890',
        'Original QuartzCore native-GPU fat slice excluded')
require(d['unchanged_original_functions']==['read_surf_vert','attachment_clear_frag_lpf' if args.bgra else 'path_clear_frag_lpf'] and d['color_attachment']==(0 if args.bgra else 2) and d['bgra8']==args.bgra,
        'Actual original vertex/fragment pair with sparse color target')
for label,draw,bits in [('clear_only_control',False,0xff336699 if args.bgra else 0x3f800000),('original_draw',True,0)]:
    row=d[label];start=row['gpu_start'];end=row['gpu_end']
    require(row['original_shader_draw']==draw and row['verified_pixels']==4096
            and row['expected_pixel_bits']==bits and row['command_status']==4
            and math.isfinite(start) and math.isfinite(end) and 0<start<end
            and row['readback_sha256']==hashlib.sha256(struct.pack('<I',bits)*4096).hexdigest(),
            'Real completed GPU commands, timestamps and independently checked pixel readback')
require([sha(p) for p in paths]==expected,'Original inputs remain unchanged after compilation/execution')
(OUT/'qualification.json').write_text(json.dumps(dict(source_sha256=sha(source),
    backend_sources={str(p.relative_to(ROOT)):sha(p) for p in (ROOT/'drivers/AdrenoMetal/HostMetalCommandServer.mm',ROOT/'drivers/AdrenoMetal/HostMetalCommandServer.h',ROOT/'drivers/AdrenoMetal/OfflineMetalWire.h',ROOT/'drivers/AdrenoMetal/OfflineMetalFunctionWire.h',ROOT/'drivers/AdrenoMetal/OfflineMetalQueueWire.h',ROOT/'drivers/AdrenoMetal/OfflineMetalTextureWire.h',ROOT/'drivers/AdrenoMetal/OfflineMetalDepthStencilWire.h',ROOT/'drivers/AdrenoMetal/OfflineMetalVertexWire.h',ROOT/'drivers/AdrenoMetal/OfflineMetalFenceWire.h',ROOT/'drivers/AdrenoMetal/OfflineMetalCommandWire.h',ROOT/'drivers/AdrenoMetal/OfflineMetalEventWire.h',ROOT/'drivers/AdrenoMetal/OfflineMetalSharedEventWire.h',ROOT/'drivers/AdrenoMetal/OfflineMetalSessionWire.h')},
    reader_revision=subprocess.check_output(['git','-C',str(kit),'rev-parse','HEAD'],text=True).strip(),
    reader_sha256={p.name:sha(p) for p in (kit/'Library.h',kit/'Library.c')},
    process_sha256=sha(exe),report_sha256=sha(report_path),
    actual_host_gpu_original_shader_pair_executed=True,air_slice_only=True,
    original_library_functions_loaded=240,all_functions_pipeline_compiled=False,
    qemu_connected=False,guest_metal_device=False,adreno_execution=False,
    active_native_gpu_4k_qualified=False,original_desktop=False),indent=2)+'\n')
print('PASS: unchanged original AIR-only libraries load all 240 names; original shader pair draws on real host Metal GPU')
print('Actual clear-only control and 4096-pixel readback match; QEMU/guest Metal/native Adreno remain unconnected')
