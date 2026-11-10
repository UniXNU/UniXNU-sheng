#!/usr/bin/env python3
"""Stage signed Bluetooth services and pinned firmware for the existing Data addon."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

root=Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('output',type=Path)
parser.add_argument('target',choices=('ios27','macos27'))
parser.add_argument('--os-root',type=Path,required=True)
parser.add_argument('--component',type=Path,help='resolved QcaBluetooth checkout')
parser.add_argument('--firmware-source',type=Path,default=root/'.cache/firmware/qca')
args=parser.parse_args()
out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
(out/'report.json').unlink(missing_ok=True)
component=(args.component or Path(os.environ.get('QCA_BLUETOOTH_DIR',root/'drivers/QcaBluetooth'))).resolve()
if not (component/'include/qbt.h').is_file():
    parser.error('QcaBluetooth module directory missing: '+str(component))
userland=out/'userland'
subprocess.run([sys.executable,component/'tools/build-userland.py',userland,
                '--bluetoothd',args.os_root/'usr/sbin/bluetoothd','--os',args.target],check=True)
subprocess.run([sys.executable,root/'scripts/stage-bluetooth-firmware.py',out,
                '--source',args.firmware_source,'--fetch'],check=True)
firmware=json.loads((out/'bluetooth-firmware.json').read_text())
report=json.loads((userland/'report.json').read_text())
# The kernel shows the two directories over /usr/sbin and /usr/lib: the system's own launchd jobs start the daemon and BlueTool
# there, and the daemon loads the library from a place its sandbox lets it map code from.
names=('sbin','lib') if args.target=='macos27' else ('bluetooth-bin','bluetooth-lib')
report['directories']={names[0]:'userland/bin',names[1]:'userland/lib',
                       firmware['firmware_prefix']:firmware['firmware_prefix']}
report['firmware']=firmware
# A unique upper entry lets the kernel verify the executable union mount.
marker=dict(native_input_sha256=report['native_input_sha256'],
            files={name:item['sha256'] for name,item in report['files'].items()})
(userland/'bin/QcaBluetooth.runtime').write_text(json.dumps(marker,sort_keys=True)+'\n')
report['runtime_marker_sha256']=hashlib.sha256((userland/'bin/QcaBluetooth.runtime').read_bytes()).hexdigest()
(out/'report.json').write_text(json.dumps(report,indent=2)+'\n')
print('Staged Bluetooth runtime: '+str(out))
