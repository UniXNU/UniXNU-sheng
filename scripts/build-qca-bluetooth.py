#!/usr/bin/env python3
"""Build QcaBluetooth and SM8550 transport operations for the selected native kernel."""
import argparse,hashlib,json,os,subprocess,sys
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('component',type=Path);p.add_argument('output',type=Path);p.add_argument('--tree',type=Path,required=True)
a=p.parse_args();component=a.component.resolve();tree=a.tree.resolve();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
subprocess.run([sys.executable,component/'tools/prepare-linux.py'],check=True)
record=tree/'build/sheng-source-storage/BootDiagExport.command.json'
base=json.loads(record.read_text());bsd=tree/'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/bsd/DEVELOPMENT/kern_exec.o.json'
cwd=Path(json.loads('['+bsd.read_text().strip().rstrip(',')+']')[0]['directory'])
soc=tree/'kits/Sm8550Platform'
# The front end the OS's bluetoothd uses: the UART.SK kernel control on iPadOS, the serial device /dev/cu.BLTH on macOS.
front='src/qbt_tty.c' if os.environ.get('UNIXNU_XNU_TARGET')=='macosx' else 'src/qbt_transport.c'
sources=[component/f for f in ('src/qbt_core.c','src/qbt_linux.c','src/qbt_geni.c',front,'out/linux/btqca.c')]
sources += [soc/f for f in ('Sm8550GeniUart.c','Sm8550Gcc.c','Sm8550QupDma.c')]
objects=[]
for source in sources:
    obj=out/(source.stem+'.o');cc=list(base);cc[cc.index('-xc')+1]=str(source);cc[cc.index('-o')+1]=str(obj)
    if '-MF' in cc:cc[cc.index('-MF')+1]=str(obj.with_suffix('.d'))
    cc += ['-I'+str(d) for d in (component/'include',component/'src',component/'shim',component/'out/linux',soc,tree/'kits/ArmSmmu4K')]
    cc += ['-Wno-sign-compare']
    (out/(source.stem+'.command.json')).write_text(json.dumps(cc,indent=2)+'\n')
    result=subprocess.run(cc,cwd=cwd,text=True,capture_output=True)
    (out/(source.stem+'.log')).write_text(result.stdout+result.stderr)
    if result.returncode:raise SystemExit(result.stdout+result.stderr)
    objects.append(obj)
(out/'report.json').write_text(json.dumps(dict(built=True,hardware_verified=False,
    source={str(s):hashlib.sha256(s.read_bytes()).hexdigest() for s in sources},
    objects={o.name:hashlib.sha256(o.read_bytes()).hexdigest() for o in objects}),indent=2)+'\n')
for obj in objects:print('OBJ '+str(obj))
