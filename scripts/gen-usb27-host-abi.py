#!/usr/bin/env python3
"""Write the reviewed AppleUSBXHCI ABI fixture for iPadOS 27 (24A446): UniXNU-sheng/scripts/usb-xhci-24A446.json. See gen-usb27-abi.py."""
import hashlib, json, re, struct, sys
from pathlib import Path
SERIES = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(SERIES / 'UniXNU/kits/XnuPrelink'))
from kc_image import Image
source = (Path(__file__).with_name('gen-usb27-abi.py')).read_text()
exec(source[source.index('def discriminator'):source.index('slots = []')])
raw = open(str(SERIES / 'UniXNU/out/ipsw/24A446/kernelcache.decompressed'), 'rb').read()
fix = Image(raw).fixups()
VT, COUNT, BASE = 0xfffffe0008430a10, 323, 168
slots = []
for i in range(COUNT):
    target, kind, _, diversity = fix[VT + 8 * i][:4]
    assert kind == 1, i
    slots.append([hex(target), hex(diversity)])
assert not (fix.get(VT + 8 * COUNT) and fix[VT + 8 * COUNT][1] == 1)
# Slots the native class overrides carry their signature: it is part of the mangled name the discriminator hashes. 315 and 317 are the
# 64-bit register read and write, with the mapped base and the offset.
SIGNATURES = {188: ('uint32_t', '', 'v'), 315: ('uint64_t', 'void *,uint32_t', 'Pvj'), 317: ('void', 'void *,uint32_t,uint64_t', 'Pvjy')}
methods = {}
for i in range(BASE, COUNT):
    want = int(slots[i][1], 16)
    returns, parameters, mangled = SIGNATURES.get(i, ('void', '', 'v'))
    for n in range(4000000):
        name = f'opaque{i}_{n}'
        if discriminator(f'_ZN12AppleUSBXHCI{len(name)}{name}E{mangled}') == want:
            methods[i] = {'name': name, 'returns': returns, 'parameters': parameters}
            break
    else:
        raise SystemExit(f'No name for slot {i}')
abi = {'kernelcache_sha256': hashlib.sha256(raw).hexdigest(), 'collection_slide': hex(0x1f000000000), 'vtable': hex(VT), 'instance_size': hex(0xf88),
       'slots': slots, 'methods': {str(k): v for k, v in methods.items()},
       'bindings': {'__ZN12AppleUSBXHCIC2EPK11OSMetaClass': sys.argv[1], '__ZN12AppleUSBXHCID2Ev': sys.argv[2],
                    '__ZN12AppleUSBXHCI10gMetaClassE': '0xfffffe000bb6b7d0', '__ZTV12AppleUSBXHCI': hex(VT - 16)}}
out = SERIES / 'UniXNU-sheng/scripts/usb-xhci-24A446.json'
out.write_text(json.dumps(abi, indent=1) + '\n')
print('wrote', out, len(methods), 'names')
