#!/usr/bin/env python3
"""Write the reviewed AppleUSBXDCI ABI fixture for iPadOS 27 (24A446): UniXNU-sheng/scripts/usb-xdci-24A446.json.

The fixture records the class the native SM8550 controller derives from: its 280 vtable slots (target and diversity), instance
size and the original constructor, destructor, metaclass and vtable. Slots 168 and up are first declared by IOUSBDeviceController
or AppleUSBXDCI, whose headers are not public, so each gets a generated method name with that slot's pointer-authentication
discriminator. Slots the native class overrides or calls also carry their signature, which is part of the mangled name.
"""
import hashlib
import json
import struct
import sys
from pathlib import Path
SERIES = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(SERIES / 'UniXNU/kits/XnuPrelink'))
from kc_image import Image
raw = open(str(SERIES / 'UniXNU/out/ipsw/24A446/kernelcache.decompressed'), 'rb').read()
fix = Image(raw).fixups()
VT = 0xfffffe00083f24c0
COUNT = 280
BASE = 168


def discriminator(name):
    mask = (1 << 64) - 1
    rotl = lambda x, b: ((x << b) | (x >> (64 - b))) & mask  # noqa: E731
    key = bytes([0xb5, 0xd4, 0xc9, 0xeb, 0x79, 0x10, 0x4a, 0x79, 0x6f, 0xec, 0x8b, 0x1b, 0x42, 0x87, 0x81, 0xd4])
    k0, k1 = struct.unpack('<2Q', key)
    v = [0x736f6d6570736575 ^ k0, 0x646f72616e646f6d ^ k1, 0x6c7967656e657261 ^ k0, 0x7465646279746573 ^ k1]

    def rounds(n):
        for _ in range(n):
            v[0] = (v[0] + v[1]) & mask; v[1] = rotl(v[1], 13) ^ v[0]; v[0] = rotl(v[0], 32)
            v[2] = (v[2] + v[3]) & mask; v[3] = rotl(v[3], 16) ^ v[2]
            v[0] = (v[0] + v[3]) & mask; v[3] = rotl(v[3], 21) ^ v[0]
            v[2] = (v[2] + v[1]) & mask; v[1] = rotl(v[1], 17) ^ v[2]; v[2] = rotl(v[2], 32)
    data = name.encode()
    full = len(data) & ~7
    for i in range(0, full, 8):
        m = struct.unpack_from('<Q', data, i)[0]
        v[3] ^= m; rounds(2); v[0] ^= m
    b = len(data) << 56 & mask
    for i, c in enumerate(data[full:]):
        b |= c << (8 * i)
    v[3] ^= b; rounds(2); v[0] ^= b; v[2] ^= 0xff; rounds(4)
    return (v[0] ^ v[1] ^ v[2] ^ v[3]) % 0xffff + 1


slots = []
for i in range(COUNT):
    target, kind, _, diversity = fix[VT + 8 * i][:4]
    assert kind == 1, i
    slots.append([hex(target), hex(diversity)])
assert not (fix.get(VT + 8 * COUNT) and fix[VT + 8 * COUNT][1] == 1)
# Signatures read from the callers in AppleUSBXDCI (bl sites in goOnBus, activateEndpoint) and AppleUSBXDCIARM's implementations.
SIGNATURES = {179: ('bool', '', 'v'), 227: ('uint32_t', '', 'v'), 267: ('bool', '', 'v'), 268: ('IOReturn', '', 'v'), 269: ('IOReturn', '', 'v'), 270: ('void', '', 'v'),
              275: ('IOReturn', '', 'v'), 276: ('uint32_t', '', 'v'), 277: ('uint32_t', '', 'v'), 278: ('void', 'uint32_t', 'j')}
methods = {}
for i in range(BASE, COUNT):
    ret, params, mangled = SIGNATURES.get(i, ('void', '', 'v'))
    want = int(slots[i][1], 16)
    for n in range(4000000):
        name = f'opaque{i}_{n}'
        if discriminator(f'_ZN12AppleUSBXDCI{len(name)}{name}E{mangled}') == want:
            methods[i] = {'name': name, 'returns': ret, 'parameters': params}
            break
    else:
        raise SystemExit(f'No name for slot {i}')
abi = {'kernelcache_sha256': hashlib.sha256(raw).hexdigest(), 'collection_slide': hex(0x1f000000000), 'vtable': hex(VT),
       'instance_size': hex(0x2650), 'slots': slots, 'methods': {str(k): v for k, v in methods.items()},
       # C2 stores this vptr after the IOUSBDeviceController constructor; D2 is the tail the subclass destructors branch to.
       'bindings': {'__ZN12AppleUSBXDCIC2EPK11OSMetaClass': '0xfffffe000a68d85c', '__ZN12AppleUSBXDCID2Ev': '0xfffffe000a68d894',
                    '__ZN12AppleUSBXDCI10gMetaClassE': '0xfffffe000bb68bd0', '__ZTV12AppleUSBXDCI': hex(VT - 16)}}
out = SERIES / 'UniXNU-sheng/scripts/usb-xdci-24A446.json'
out.write_text(json.dumps(abi, indent=1) + '\n')
print('wrote', out, len(methods), 'names')
