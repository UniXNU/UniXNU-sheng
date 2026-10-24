#!/usr/bin/env python3
"""Build the explicit private offline GPU broker against the actual kernel ABI."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--accelerator-abi',action='store_true');parser.add_argument('--provider-probe',action='store_true',help='Include unpublished original IOGPU required-hook instrument');options=parser.parse_args()
if options.provider_probe:options.accelerator_abi=True
ROOT=Path(__file__).resolve().parents[1];OUT=ROOT/'build/native-offline-gpu';OUT.mkdir(exist_ok=True)
def run(cmd,**kwargs):return subprocess.check_output([str(x) for x in cmd],timeout=90,**kwargs)
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
print(run(['python3',ROOT/'scripts/build-arm-driver.py','NativeVirtioConsole'],text=True),end='')
print(run(['python3',ROOT/'scripts/build-arm-driver.py','NativeOfflineGPU'],text=True),end='')
llvm=Path(run(['brew','--prefix','llvm'],text=True).strip())/'bin';obj=OUT/'NativeOfflineGPU.o';late=OUT/'NativeOfflineGPU.late.o'
run([llvm/'llvm-objcopy','--remove-section=__DATA,__mod_init_func','--globalize-symbol=__GLOBAL__sub_I_NativeOfflineGPU.cpp',obj,late])
sys.path.insert(0,str(ROOT/'kits/XnuPrelink'))
from native_prelink import macho
def text_code(path):
    b=path.read_bytes();m=macho(b);s=next(s for g in m['segments'] for s in g['sections'] if s['name']=='__text')
    return b[s['fileoff']:s['fileoff']+s['size']]
if text_code(obj)!=text_code(late):raise SystemExit('Only owned initializer registration may change during staging')
record=json.loads((ROOT/'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json').read_text().strip().rstrip(','))
source=ROOT/'kits/sheng/OfflineGPUBrokerProbe.cpp';command=[];args=record['arguments'];i=0
while i<len(args):
    value=args[i]
    if value==record['file']:command.append(str(source))
    elif value=='-o':command+=['-o',str(OUT/'OfflineGPUBrokerProbe.o')];i+=1
    elif value=='-Xclang' and args[i+1].startswith('-W'):i+=1
    elif not value.startswith('-W'):command.append(value)
    i+=1
command+=['-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-unnecessary-virtual-specifier','-Wno-c99-designator','-Wno-reorder-init-list','-Wno-deprecated-enum-enum-conversion','-I',str(ROOT/'.cache/xnu-work/osfmk')]
command+=['-DNATIVE_ACCELERATOR_ABI_PROBE='+str(int(options.accelerator_abi)),'-DNATIVE_PROVIDER_ABI_PROBE='+str(int(options.provider_probe))]
run(command,cwd=record['directory'])
archive=OUT/'libNativeOfflineGPU.late.a';archive.unlink(missing_ok=True)
if options.provider_probe:print(run(['python3',ROOT/'scripts/build-original-gpu-provider-probe.py'],text=True),end='')
if options.accelerator_abi:
    print(run(['python3',ROOT/'scripts/build-native-accelerator-abi.py'],text=True),end='')
    run(['xcrun','libtool','-static','-o',archive,late,OUT/'OfflineGPUBrokerProbe.o',ROOT/'build/native-offline-accelerator/libNativeOfflineAccelerator.late.a',*([ROOT/'build/native-original-provider/libNativeIOGPUProviderProbe.late.a'] if options.provider_probe else [])])
else:run(['xcrun','ar','rcs',archive,late,OUT/'OfflineGPUBrokerProbe.o'])
sysroot=ROOT/'drivers/AdrenoMetal';sources=[source,*(sysroot/n for n in ('NativeOfflineGPU.cpp','OfflineMetalWire.h','OfflineMetalQueueWire.h','OfflineMetalTextureWire.h','OfflineMetalDepthStencilWire.h','OfflineMetalFenceWire.h','OfflineMetalCommandWire.h','OfflineMetalEventWire.h','OfflineMetalSharedEventWire.h','OfflineMetalSessionWire.h')),
    ROOT/'drivers/VirtioMmioConsole/NativeVirtioConsole.cpp',ROOT/'drivers/VirtioMmioConsole/NativeVirtioConsoleTransport.h']
if options.accelerator_abi:sources.extend([ROOT/'drivers/AdrenoMetal/NativeOfflineAccelerator.cpp',ROOT/'drivers/AdrenoMetal/OriginalAcceleratorABI.h',ROOT/'kits/sheng/AcceleratorABIProbe.cpp',ROOT/'kits/sheng/OriginalGPUClientABIProbe.cpp'])
if options.provider_probe:sources.extend([ROOT/'kits/sheng/OriginalGPUProviderProbe.cpp',ROOT/'drivers/AdrenoMetal/OriginalGPUProviderABI.h',ROOT/'drivers/AdrenoMetal/NativeIOGPUProviderProbe.cpp'])
(OUT/'build-report.json').write_text(json.dumps(dict(source_sha256={str(p.relative_to(ROOT)):sha(p) for p in sources},
    archive_sha256=sha(archive),console_archive_sha256=sha(ROOT/'build/virtio-mmio-console/libVirtioMmioConsole.a'),
    compiled_object_sha256=sha(obj),late_object_sha256=sha(late),accelerator_abi_probe=options.accelerator_abi,provider_probe=options.provider_probe,normal_system_device_discovery=False,runtime_qualified=False),indent=2)+'\n')
print('PASS: actual ARM64/iOS17 private GPU broker and real exclusive byte-transport interface build with -Werror')
