#!/usr/bin/env python3
"""Build an own signed original-iOS17 IOKit/GPU transport instrument."""
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import uuid
ROOT=Path(__file__).resolve().parents[1];OUT=ROOT/'build/offline-gpu-transport'
OUT.mkdir(parents=True,exist_ok=True)
def run(cmd):return subprocess.run([str(x) for x in cmd],check=True,timeout=60)
def sha(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()
sdk=subprocess.check_output(['xcrun','--sdk','iphoneos','--show-sdk-path'],text=True).strip()
source=ROOT/'kits/sheng/OfflineGPUTransportDiagnostic.cpp';exe=OUT/'air-native'
run(['xcrun','clang++','-target','arm64-apple-ios17.0','-isysroot',sdk,'-std=c++17','-O2','-g',
     '-Wall','-Wextra','-Werror','-Wno-deprecated-declarations','-fstack-protector-strong',
     '-fno-exceptions','-fno-rtti',source,'-o',exe])
run(['codesign','--force','--sign','-','--pagesize','4096','--identifier','org.blu-sharky.offline-gpu-transport',exe])
run(['codesign','--verify','--strict',exe])
details=subprocess.run(['codesign','--display','--verbose=4',exe],capture_output=True,text=True,check=True).stderr
cdhash=re.search(r'^CDHash=([a-f0-9]{40})$',details,re.M)[1];assert re.search(r'^Page size=4096$',details,re.M)
module=struct.pack('<I16sI',2,uuid.uuid5(uuid.NAMESPACE_URL,'offline-gpu-transport:'+cdhash).bytes,1)+bytes.fromhex(cdhash)+bytes([2,0,0,0])
original=(ROOT/'build/original-static-system-trust/original-static-system.bundle').read_bytes()
assert struct.unpack_from('<I',original)[0]==2
starts=struct.unpack_from('<2I',original,4);modules=[module,original[starts[0]:starts[1]],original[starts[1]:]]
cursor=16;offsets=[]
for data in modules:offsets.append(cursor);cursor+=len(data)
bundle=struct.pack('<4I',3,*offsets)+b''.join(modules);(OUT/'offline-trust.bundle').write_bytes(bundle)
manifest=OUT/'test-inputs.json'
manifest.write_text(json.dumps(dict(sources={str(p.relative_to(ROOT)):sha(p) for p in
    (source,ROOT/'kits/sheng/OfflineGPUWire.h',ROOT/'drivers/VirtioMmioConsole/NativeVirtioConsole.cpp')},
    guest_metal_device=False,adreno_execution=False,original_desktop=False),indent=2)+'\n')
(OUT/'build-report.json').write_text(json.dumps(dict(process_sha256=sha(exe),process_bytes=exe.stat().st_size,
    source_sha256=sha(source),manifest_sha256=sha(manifest),cdhash=cdhash,code_signature_page_bytes=4096,
    trust_bundle_sha256=hashlib.sha256(bundle).hexdigest(),original_trust_modules_unchanged=True,
    offline_test_only=True,native_execution_qualified=False),indent=2)+'\n')
print('PASS: own iOS17 IOKit transport instrument built and explicitly signed; execution pending')
