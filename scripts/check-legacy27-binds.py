#!/usr/bin/env python3
"""Check, in the emitted iPadOS 27 kernel, that the display backend's references reach the original Legacy code:
the constructor's branch to the original C2 and an inherited vtable entry (slot 180, swap_submit)."""
import json, struct, subprocess, sys
from pathlib import Path
SERIES = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(SERIES / 'UniXNU/kits/XnuPrelink'))
from kc_image import Image
import capstone
K = sys.argv[1]
abi = json.load(open(str(SERIES / 'UniXNU-sheng/scripts/legacy-display-24A446.json')))
cs = int(abi['collection_slide'], 16)
sym = {l.split()[2]: int(l.split()[0], 16) for l in subprocess.run(['nm', K], capture_output=True, text=True).stdout.splitlines()
       if len(l.split()) == 3 and l[:1] in '0123456789abcdef'}
raw = open(K, 'rb').read()
top = Image(raw)
images = [top] + [Image(raw, o) for _, (_, o) in top.entries.items()]
def read(va, n):
    for e in images:
        try:
            return e.read(va, n)
        except Exception:
            pass
    raise ValueError(hex(va))
md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
def through(va):
    """Follow an owned stub (bti c; b target) to the code it reaches."""
    ins = list(md.disasm(read(va, 8), va))
    if len(ins) == 2 and ins[0].mnemonic == 'bti' and ins[1].mnemonic == 'b':
        return int(ins[1].op_str.lstrip('#'), 16)
    return va
alloc = sym['_native_legacy_display_allocate']
c2 = int(abi['bindings']['__ZN25IOMobileFramebufferLegacyC2EPK11OSMetaClass'], 16) + cs
targets = [through(int(i.op_str.lstrip('#'), 16)) for i in md.disasm(read(alloc, 0x60), alloc) if i.mnemonic == 'bl']
ok = c2 in targets
print('constructor branch', [hex(t) for t in targets], 'want', hex(c2), 'OK' if ok else 'BAD')
fix = {}
for e in images:
    try:
        fix.update(e.fixups())
    except Exception:
        pass
vt = sym['__ZTV19NativeLegacyDisplay'] + 16
want = int(abi['slots'][180][0], 16) + cs
got = fix.get(vt + 8 * 180)
slot = through(got[0]) if got else None
print('vtable slot 180', hex(slot) if slot else None, 'signed' if got and got[1] else 'unsigned', 'want', hex(want), 'OK' if slot == want else 'BAD')
# The metaclass constructors load the original gMetaClass from a GOT entry: it must hold the original address.
meta = int(abi['bindings']['__ZN25IOMobileFramebufferLegacy10gMetaClassE'], 16) + cs
ctor = sym['__ZN19NativeLegacyDisplay9MetaClassC2Ev']
ins = list(md.disasm(read(ctor, 0x40), ctor))
got_value = None
for a, b in zip(ins, ins[1:]):
    if a.mnemonic == 'adrp' and b.mnemonic == 'ldr' and b.op_str.startswith('x2,'):
        page = int(a.op_str.split('#')[1], 16)
        off = int(b.op_str.split('#')[1].rstrip(']'), 16)
        got_value = struct.unpack('<Q', read(page + off, 8))[0]
meta_ok = got_value == meta
print('gMetaClass GOT', hex(got_value) if got_value else None, 'want', hex(meta), 'OK' if meta_ok else 'BAD')
surface_ok = True
for name in ('_native_legacy_surface_create', '_native_legacy_surface_allocate'):
    expected = int(abi['bindings'][name], 16) + cs
    actual = through(sym[name]) if name in sym else None
    print(name, hex(actual) if actual else None, 'want', hex(expected), 'OK' if actual == expected else 'BAD')
    surface_ok = surface_ok and actual == expected
sys.exit(0 if ok and slot == want and meta_ok and surface_ok else 1)
