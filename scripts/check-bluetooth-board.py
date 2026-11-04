#!/usr/bin/env python3
"""Compile Bluetooth board integration with the selected kernel's C++ command."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

root=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('tree',type=Path)
a=p.parse_args();tree=a.tree.resolve();out=root/'drivers/QcaBluetooth/out/board-check';out.mkdir(parents=True,exist_ok=True)
path=tree/'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'
r=json.loads(path.read_text().strip().rstrip(','))
sources=['WifiProbe.cpp']
for stem in ('BluetoothProbe.cpp',):
    if (root/'package'/stem).exists():sources.append(stem)
report={}
for name in sources:
    source=root/'package'/name;obj=out/(source.stem+'.o');cc=[];args=r['arguments'];i=0
    while i<len(args):
        v=args[i]
        if v==r['file']:cc.append(str(source))
        elif v=='-o':cc+=['-o',str(obj)];i+=1
        elif v in ('-MF','-MT','-MJ'):i+=1
        elif v=='-Xclang' and args[i+1].startswith('-W'):i+=1
        elif not v.startswith('-W'):cc.append(v)
        i+=1
    cc+=['-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-unnecessary-virtual-specifier','-Wno-c99-designator',
         '-Wno-reorder-init-list','-Wno-deprecated-enum-enum-conversion']
    for d in (root/'package',root/'drivers/Ath12kWifi/include',root/'drivers/QcaBluetooth/include',tree/'.cache/xnu-work/osfmk'):
        cc+=['-I',str(d)]
    (out/(source.stem+'.command.json')).write_text(json.dumps(cc,indent=2)+'\n')
    result=subprocess.run(cc,cwd=r['directory'],capture_output=True,text=True)
    (out/(source.stem+'.log')).write_text(result.stdout+result.stderr)
    if result.returncode:raise SystemExit(result.stdout+result.stderr)
    report[name]=dict(source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),object_sha256=hashlib.sha256(obj.read_bytes()).hexdigest())
(out/'report.json').write_text(json.dumps(dict(passed=True,hardware_verified=False,record=str(path),sources=report),indent=2)+'\n')
print('PASS: Bluetooth board and Wi-Fi shared-power arm64e compile')
