#!/usr/bin/env python3
"""Write the reviewed IOHIDDevice ABI fixture for iPadOS 27 (24A446): UniXNU-sheng/scripts/hid-device-24A446.json."""
import hashlib, json, sys
from pathlib import Path
SERIES = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(SERIES / 'UniXNU/kits/XnuPrelink'))
from kc_image import Image
raw = open(str(SERIES / 'UniXNU/out/ipsw/24A446/kernelcache.decompressed'), 'rb').read()
fix = Image(raw).fixups()
VT = 0xfffffe0008349508
slots = []
for i in range(207):
    target, kind, _, diversity = fix[VT + 8 * i][:4]
    assert kind == 1, i
    slots.append([hex(target), hex(diversity)])
assert not (fix.get(VT + 8 * 207) and fix[VT + 8 * 207][1] == 1)
abi = {'kernelcache_sha256': hashlib.sha256(raw).hexdigest(), 'collection_slide': hex(0x1f000000000),
       'vtable': hex(VT), 'instance_size': hex(0xd0), 'slots': slots,
       # C2 calls the IOService C2 stub and stores this vptr; D2 tail-calls; metaclass init passes size 0xd0.
       'bindings': {'__ZN11IOHIDDeviceC2EPK11OSMetaClass': '0xfffffe000a1b5574', '__ZN11IOHIDDeviceD2Ev': '0xfffffe000a1b55ac',
                    '__ZN11IOHIDDevice10gMetaClassE': '0xfffffe000bb40ff8', '__ZTV11IOHIDDevice': hex(VT - 16)}}
out = str(SERIES / 'UniXNU-sheng/scripts/hid-device-24A446.json')
open(out, 'w').write(json.dumps(abi, indent=1) + '\n')
print('wrote', out)
