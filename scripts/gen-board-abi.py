#!/usr/bin/env python3
"""Write the class ABI fixtures the board drivers bind to, from a kernel collection that keeps its symbols.

    gen-board-abi.py --kernelcache KC --oracle ORACLE_KC --build BUILD [--reference-build 24A446]

The fixtures have the layout of the reviewed iPadOS ones (legacy-display-, hid-device-, usb-xdci-, usb-xhci-<build>.json),
so build-gop-legacy-display-27.py reads either. Everything comes from the collection: vtables from its chained fixups,
instance sizes from the classes' OSMetaClass constructors, bindings from its symbol tables. A slot keeps the diversity of
its vtable entry, except that a pure slot IOMobileFramebufferLegacy inherits from IOMobileFramebuffer takes the diversity a
concrete subclass signs it with (ORACLE_KC: AppleParavirtDisplay of the virtual-machine collection of the same release,
whose IOMobileGraphicsFamily is identical); the family's own entry for such a slot is never called.

The roles the drivers give slots (overridden methods, macros) carry over from the reference build's fixtures: a slot
takes the role of the reference slot that has the same diversity, the same declaration. Opaque slot names are found as
the reference ones were: the smallest opaque<slot>_<n> whose mangling hashes to the slot's diversity.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
UNIXNU = Path(__import__('os').environ.get('UNIXNU_ROOT', ROOT.parent / 'UniXNU'))
HERE = Path(__file__).resolve().parent
NATIVE_LINK_ADDRESS = 0xfffffff007004000
IOAV_TIMING = '__ZN22IOAVVideoTimingElement8withDataEPK19IOAVVideoTimingData16IOAVStandardType23IOAVVideoTimingStandard'
IOAV_TIMING_COLORS = '__ZN22IOAVVideoTimingElement16setColorElementsEP7OSArray'
IOAV_COLOR = ('__ZN21IOAVVideoColorElement16withCapabilitiesE16IOAVStandardTypej22IOAVVideoPixelEncoding'
              '26IOAVVideoColorDynamicRange18IOAVVideoColorEOTF20IOAVVideoColorimetrybPK12OSDictionaryS7_S7_PK5OSSet')

# The owned display backend's methods with their prototypes (build-gop-legacy-display-27.py declares them). The labels
# of the reference build; a macOS-only label is a pure slot of the macOS family that the backend implements.
LEGACY_PROTOTYPES = {
    'isMainDisplay': 'bool isMainDisplay()', 'getDotPitch': 'IOReturn getDotPitch(uint32_t *)',
    'supportedFrameInfo': 'IOReturn supportedFrameInfo(uint32_t,uint32_t *,char *,uint32_t *)=0',
    'getDisplayDimensions': 'void getDisplayDimensions(uint32_t *,uint32_t *)', 'startHardware': 'bool startHardware()',
    'kickSwapQueue': 'void kickSwapQueue()', 'completeCurrentSwap': 'void completeCurrentSwap()',
    'validateSwap': 'bool validateSwap(const OriginalLegacySwapRequest *)',
    'getRefreshAttributes': 'bool getRefreshAttributes(uint64_t *,bool)',
    'collectCompletionReports': 'IOReturn collectCompletionReports(uint32_t,void *,uint32_t *,uint32_t)=0',
    'applyStateRequest': 'int applyStateRequest(OriginalLegacyStateRequest *,bool)',
    'cancelSwapsAfterTimeout': 'bool cancelSwapsAfterTimeout()',
    'create_default_fb_surface': 'void create_default_fb_surface()',
}
# The slots the macOS family leaves pure that the reference build's fixtures do not name, with the oracle's method name: the
# backend implements them as the virtual-machine display does. Every other family slot keeps the family's own code.
LEGACY_MACOS = {
    'setNotifyTagMaskGated': ('set_notify_tag_mask_gated', 'IOReturn setNotifyTagMaskGated(uint32_t,void *,uint32_t)=0'),
    'factoryPortal': ('factory_portal', 'IOReturn factoryPortal(void *,task_t)=0'),
    'executeFactoryConfigurations': ('executeFactoryConfigurations_gated', 'IOReturn executeFactoryConfigurations()=0'),
    'setFrameInfoNotificationState': ('set_frame_info_notification_state_gated', 'IOReturn setFrameInfoNotificationState(void *,bool)=0'),
    'doSwapInfoNotify': ('do_swap_info_notify_gated', 'IOReturn doSwapInfoNotify(void *,const void *)=0'),
    'notifyCrcReady': ('notify_crc_ready_gated', 'IOReturn notifyCrcReady(uint32_t)=0'),
}

# Family slots whose IOMobileFramebufferLegacy code refuses the call (kIOReturnUnsupported) and that macOS's display clients make,
# with the family method name: the backend implements them as the virtual-machine display does. Found by the family's own
# symbol for the slot, so no oracle is needed. Pointer and enumeration parameters are declared as void * and uint32_t.
LEGACY_MACOS_FAMILY = {
    'setDisplayDevice': ('set_display_device', 'IOReturn setDisplayDevice(uint32_t)'),
    'setDigitalOutMode': ('set_digital_out_mode', 'IOReturn setDigitalOutMode(uint32_t,uint32_t)'),
    'setGammaTable': ('set_gamma_table', 'IOReturn setGammaTable(void *)'),
    'setColorRemapMode': ('set_color_remap_mode', 'IOReturn setColorRemapMode(uint32_t)'),
    'getColorRemapMode': ('get_color_remap_mode', 'IOReturn getColorRemapMode(uint32_t *)'),
    'swapSetColorMatrix': ('swap_set_color_matrix', 'IOReturn swapSetColorMatrix(void *,uint32_t,uint32_t)'),
}


def family_overrides(kc, slots, cls='IOMobileFramebufferLegacy'):
    """The slot of each LEGACY_MACOS_FAMILY method: the one whose target the collection names cls::method."""
    found = {}
    for label, (method, _) in LEGACY_MACOS_FAMILY.items():
        marker = f'{len(cls)}{cls}{len(method)}{method}E'
        hits = [slot for slot, (target, _) in enumerate(slots) if any(marker in n for n in kc.names.get(target, []))]
        if len(hits) != 1:
            raise SystemExit(f'{label}: the collection names {len(hits)} slots {cls}::{method}')
        found[label] = hits[0]
    return found


def discriminator(name):
    """clang ptrauth_string_discriminator: SipHash-2-4 with the fixed key, modulo 0xffff, plus one."""
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


SEARCH_C = r"""
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define ROTL(x, b) (uint64_t)(((x) << (b)) | ((x) >> (64 - (b))))
#define ROUND do { v0 += v1; v1 = ROTL(v1, 13); v1 ^= v0; v0 = ROTL(v0, 32); v2 += v3; v3 = ROTL(v3, 16); v3 ^= v2; \
  v0 += v3; v3 = ROTL(v3, 21); v3 ^= v0; v2 += v1; v1 = ROTL(v1, 17); v1 ^= v2; v2 = ROTL(v2, 32); } while (0)
static const uint8_t K[16] = {0xb5, 0xd4, 0xc9, 0xeb, 0x79, 0x10, 0x4a, 0x79, 0x6f, 0xec, 0x8b, 0x1b, 0x42, 0x87, 0x81, 0xd4};
static uint64_t le64(const uint8_t *p) { uint64_t v = 0; for (int i = 7; i >= 0; i--) v = v << 8 | p[i]; return v; }
static unsigned disc(const char *s) {
  size_t n = strlen(s); const uint8_t *in = (const uint8_t *)s;
  uint64_t k0 = le64(K), k1 = le64(K + 8);
  uint64_t v0 = 0x736f6d6570736575ULL ^ k0, v1 = 0x646f72616e646f6dULL ^ k1, v2 = 0x6c7967656e657261ULL ^ k0, v3 = 0x7465646279746573ULL ^ k1;
  size_t full = n & ~(size_t)7;
  for (size_t i = 0; i < full; i += 8) { uint64_t m = le64(in + i); v3 ^= m; ROUND; ROUND; v0 ^= m; }
  uint64_t b = (uint64_t)n << 56;
  for (size_t i = 0; i < (n & 7); i++) b |= (uint64_t)in[full + i] << (8 * i);
  v3 ^= b; ROUND; ROUND; v0 ^= b; v2 ^= 0xff; ROUND; ROUND; ROUND; ROUND;
  return (unsigned)((v0 ^ v1 ^ v2 ^ v3) % 0xffff + 1);
}
int main(void) {
  char prefix[256], ident[256], suffix[256], want_text[32], name[300], mangled[700];
  while (scanf("%255s %255s %255s %31s", prefix, ident, suffix, want_text) == 4) {
    unsigned want = (unsigned)strtoul(want_text, 0, 0), n;
    for (n = 100000; n < 1000000; n++) {
      snprintf(name, sizeof name, "%s_%u", ident, n);
      snprintf(mangled, sizeof mangled, "%s%zu%s%s", prefix, strlen(name), name, suffix);
      if (disc(mangled) == want) break;
    }
    if (n == 1000000) return 1;
    printf("%s\n", name);
  }
  return 0;
}
"""


def opaque_names(requests):
    """For each (prefix, slot, suffix, want): the smallest opaque<slot>_<n> (six digits) whose mangled name
    prefix<len><name>suffix hashes to want. A compiled search; discriminator() checks every answer."""
    with tempfile.TemporaryDirectory() as work:
        source, tool = Path(work) / 'search.c', Path(work) / 'search'
        source.write_text(SEARCH_C)
        subprocess.run(['xcrun', 'clang', '-O2', '-o', tool, source], check=True)
        lines = ''.join(f'{prefix} opaque{slot} {suffix} {want:#x}\n' for prefix, slot, suffix, want in requests)
        names = subprocess.run([tool], input=lines, capture_output=True, text=True, check=True).stdout.split()
    if len(names) != len(requests):
        raise SystemExit('opaque name search failed')
    for (prefix, slot, suffix, want), name in zip(requests, names):
        if discriminator(f'{prefix}{len(name)}{name}{suffix}') != want:
            raise SystemExit(f'opaque name {name} does not hash to {want:#x}')
    return names


def mangled_suffixes(cls, prototypes):
    """The parameter part of each prototype's mangled name (after the method name), as clang mangles it."""
    lines = ['#include <stdint.h>', 'typedef int IOReturn; typedef struct task *task_t;',
             'struct OriginalLegacySwapRequest; struct OriginalLegacyStateRequest;', f'struct {cls} {{']
    for label, prototype in prototypes.items():
        declaration = prototype.split('=')[0].strip().replace(f'{label}(', f'P{label}X(')
        lines.append(f' {declaration};')
    lines.append('};')
    for label, prototype in prototypes.items():
        declaration = prototype.split('=')[0].strip()
        returns, rest = declaration.split(f'{label}(', 1)
        params = rest.rsplit(')', 1)[0]
        named = ','.join(f'{p} a{i}' if p.strip() else '' for i, p in enumerate(params.split(','))) if params.strip() else ''
        lines.append(f'{returns}{cls}::P{label}X({named}){{ {"" if returns.strip() == "void" else "return {};"} }}')
    with tempfile.TemporaryDirectory() as work:
        source, obj = Path(work) / 'probe.cpp', Path(work) / 'probe.o'
        source.write_text('\n'.join(lines) + '\n')
        subprocess.run(['xcrun', '--sdk', 'iphoneos', 'clang++', '-target', 'arm64e-apple-ios27.0', '-std=c++17', '-c', source, '-o', obj], check=True)
        symbols = subprocess.check_output(['xcrun', 'nm', '-gj', obj], text=True).split()
    result = {}
    for label in prototypes:
        marker = f'{len("P" + label + "X")}P{label}X'
        found = [s for s in symbols if marker in s]
        if len(found) != 1:
            raise SystemExit(f'cannot mangle {label}: {found}')
        result[label] = found[0][found[0].index(marker) + len(marker):]
    return result


class Collection:
    """A decompressed kernel collection: its fixups, symbols by entry, code."""

    def __init__(self, path):
        sys.path.insert(0, str(UNIXNU / 'kits/XnuPrelink'))
        from kc_image import Image
        self.Image = Image
        self.raw = path.read_bytes()
        self.sha256 = hashlib.sha256(self.raw).hexdigest()
        self.cache = Image(self.raw)
        self.fixups = self.cache.fixups()
        self.names, self.by_entry = {}, {}
        for entry, (_, at) in self.cache.entries.items():
            table = {}
            for address, names in Image(self.raw, at).symbols().items():
                self.names.setdefault(address, []).extend(names)
                for name in names:
                    table[name] = address
            self.by_entry[entry] = table
        self.symbol_sites = set(self.names)

    def symbol(self, entry, name):
        return self.by_entry[entry][name]

    def kernel_text(self):
        image = self.Image(self.raw, self.cache.entries['com.apple.kernel'][1])
        segment = next(s for s in image.segments if s['name'] == '__TEXT_EXEC')
        return segment['va'], segment['va'] + segment['size']

    def code(self, address, length):
        for _, (_, at) in self.cache.entries.items():
            try:
                return self.Image(self.raw, at).read(address, length)
            except ValueError:
                pass
        raise ValueError(hex(address))

    def vtable(self, entry, cls):
        """First entry address and [(target, diversity)] of the class's vtable."""
        start = self.symbol(entry, f'__ZTV{len(cls)}{cls}') + 16
        slots = []
        while True:
            site = start + 8 * len(slots)
            pointer = self.fixups.get(site)
            if not pointer or not pointer[1] or (slots and site in self.symbol_sites):
                break
            slots.append((pointer[0], pointer[3]))
        return start, slots

    def instance_size(self, entry, cls):
        """The size the class's OSMetaClass constructor passes (movz w3, #imm before OSMetaClass::OSMetaClass)."""
        start = self.symbol(entry, f'__ZN{len(cls)}{cls}9MetaClassC2Ev')
        code = self.code(start, 128)
        for i in range(0, len(code), 4):
            word = struct.unpack_from('<I', code, i)[0]
            if word & 0xffe0001f == 0x52800003:
                return (word >> 5) & 0xffff
        raise SystemExit(f'{cls}: no instance size')

    def bindings(self, entry, cls):
        n = f'{len(cls)}{cls}'
        return {name: hex(self.symbol(entry, name)) for name in
                (f'__ZN{n}C2EPK11OSMetaClass', f'__ZN{n}D2Ev', f'__ZN{n}10gMetaClassE', f'__ZTV{n}')}

    def pure(self):
        return self.symbol('com.apple.kernel', '___cxa_pure_virtual')

    def service_slots(self):
        return len(self.vtable('com.apple.kernel', 'IOService')[1])


def reference_roles(reference, diversities, extra=None):
    """Reference slot -> this build's slot, through the declaration's diversity (one slot each)."""
    by_diversity = {}
    for slot, diversity in enumerate(diversities):
        by_diversity.setdefault(diversity, []).append(slot)
    mapping = {}
    for slot, (_, diversity) in enumerate(reference['slots']):
        want = int((extra or {}).get(str(slot), {}).get('diversity', diversity), 16)
        found = by_diversity.get(want, [])
        if len(found) == 1:
            mapping[slot] = found[0]
    return mapping


def legacy_fixture(kc, oracle, reference, build):
    entry, cls = 'com.apple.iokit.IOMobileGraphicsFamily', 'IOMobileFramebufferLegacy'
    start, slots = kc.vtable(entry, cls)
    pure = kc.pure()
    _, apd = oracle.vtable('com.apple.driver.AppleParavirtGPUIOGPUFamily', 'AppleParavirtDisplay')
    _, oracle_legacy = oracle.vtable(entry, cls)
    if [d for _, d in oracle_legacy] != [d for _, d in slots]:
        raise SystemExit('the oracle collection has another IOMobileFramebufferLegacy')
    required = []
    for slot, (target, diversity) in enumerate(slots):
        if target == pure:
            diversity = apd[slot][1]   # the concrete subclass's entry: the declaration's own discriminator
        required.append(diversity)
    base = kc.service_slots()
    roles = reference_roles(reference, required, reference.get('caller_diversity'))
    methods = {label: roles[slot] for label, slot in reference['methods'].items()}
    if 'cancelSwapsAfterTimeout' not in methods:
        methods['cancelSwapsAfterTimeout'] = roles[366]
    oracle_names = {}
    for slot, (target, _) in enumerate(apd[:len(slots)]):
        for name in oracle.names.get(target, []):
            oracle_names.setdefault(slot, name)
    prototypes = dict(LEGACY_PROTOTYPES)
    for label, (method, prototype) in LEGACY_MACOS.items():
        found = [slot for slot, name in oracle_names.items() if f'{len(method)}{method}E' in name]
        if len(found) != 1:
            raise SystemExit(f'{label}: the oracle names {len(found)} slots {method}')
        methods[label] = found[0]
        prototypes[label] = prototype
    for label, slot in family_overrides(kc, slots).items():
        methods[label] = slot
        prototypes[label] = LEGACY_MACOS_FAMILY[label][1]
    if len(set(methods.values())) != len(methods):
        raise SystemExit('two methods on one slot')
    unimplemented = [slot for slot, (target, _) in enumerate(slots) if target == pure and slot not in methods.values()]
    if unimplemented:
        raise SystemExit(f'pure slots without a backend method: {unimplemented}')
    suffixes = mangled_suffixes(cls, prototypes)
    prefix = f'_ZN{len(cls)}{cls}'
    requests, labels = [], []
    for slot in range(base, len(slots)):
        label = next((k for k, v in methods.items() if v == slot), None)
        requests.append((prefix, slot, suffixes[label] if label else 'Ev', required[slot]))
        labels.append(label or f'opaque{slot}')
    names = {label: [name, request[2]] for label, name, request in zip(labels, opaque_names(requests), requests)}
    getters = {'_native_legacy_surface_descriptor': ('com.apple.iokit.IOSurface', '__ZNK9IOSurface19getMemoryDescriptorEv'),
               '_native_legacy_surface_width': ('com.apple.iokit.IOSurface', '__ZNK9IOSurface8getWidthEv'),
               '_native_legacy_surface_height': ('com.apple.iokit.IOSurface', '__ZNK9IOSurface9getHeightEv'),
               '_native_legacy_surface_stride': ('com.apple.iokit.IOSurface', '__ZNK9IOSurface14getBytesPerRowEv'),
               '_native_legacy_surface_format': ('com.apple.iokit.IOSurface', '__ZNK9IOSurface14getPixelFormatEv'),
               '_native_legacy_surface_planes': ('com.apple.iokit.IOSurface', '__ZNK9IOSurface13getPlaneCountEv'),
               '_native_legacy_surface_create': ('com.apple.iokit.IOSurface', '__ZN13IOSurfaceRoot13createSurfaceEP4taskP12OSDictionary'),
               '_native_legacy_surface_allocate': ('com.apple.iokit.IOSurface', '__ZN9IOSurface8allocateEv'),
               '_native_legacy_vsync_notify': (entry, '__ZN25IOMobileFramebufferLegacy26swap_notify_dispatch_gatedEyyy'),
               '_native_legacy_set_refresh_properties': (entry, '__ZN25IOMobileFramebufferLegacy27setDisplayRefreshPropertiesEv'),
               '_native_legacy_swap_info_dispatch': (entry, '__ZN25IOMobileFramebufferLegacy31swap_info_notify_dispatch_gatedEPNS_12SwapInfoBlobE'),
               # The display's timing and color elements (macOS reads its modes from them): IOAVFamily's constructors.
               '_native_legacy_ioav_timing': ('com.apple.iokit.IOAVFamily', IOAV_TIMING),
               '_native_legacy_ioav_timing_colors': ('com.apple.iokit.IOAVFamily', IOAV_TIMING_COLORS),
               '_native_legacy_ioav_color': ('com.apple.iokit.IOAVFamily', IOAV_COLOR),
               # macOS: the panel reported plugged in (IOMobileFramebufferGetDigitalOutState); unplugged, WindowServer keeps the
               # display disabled and composites a virtual main display (macOS-DroidVM, AppleParavirtDisplay's start_hardware).
               '_native_legacy_hotplug_notify': (entry, '__ZN25IOMobileFramebufferLegacy14hotPlug_notifyEy')}
    bindings = kc.bindings(entry, cls)
    for name, (owner, symbol) in getters.items():
        bindings[name] = hex(kc.symbol(owner, symbol))
    # The accessors are plain loads; their code is checked, so a moved field cannot hide behind the name.
    checks = [[bindings[name], kc.code(int(bindings[name], 16), 12).hex()] for name in
              ('_native_legacy_surface_descriptor', '_native_legacy_surface_width', '_native_legacy_surface_height',
               '_native_legacy_surface_stride', '_native_legacy_surface_format', '_native_legacy_surface_planes')]
    # Field offsets, each read from the code that defines it (see the checks of build-gop-legacy-display-27.py):
    # display_width/height, the swap queue of clear_up_swap_queue_gated, the vsync clients of swap_notify_dispatch_gated,
    # the surface root and default surface of create_default_fb_surface; the request's layer mask, layer surfaces and queue
    # links from the virtual-machine display's program_hardware_gated and swap_begin_gated.
    display = {'current': 0x2000, 'head': 0x2008, 'tail': 0x2010, 'vsync_clients': 0x2078, 'width': 0x21b0, 'height': 0x21b4,
               'default_surface': 0x2118, 'surface_root': 0x2100, 'swap_info_time': 0x108, 'swap_info_held': 0x239a}
    request = {'rect0': 0x11, 'rect1': 0x51, 'masks': 0x478, 'options': 0x484, 'dims': 0x490, 'surfaces': 0xd08,
               'next': 0xfc8, 'prev': 0xfd0, 'drop_reasons': 0x48c, 'swap_info_a': 0x430, 'swap_info_b': 0x470}
    notify = {'async_reference': 0x18, 'mask': 0x78, 'last_time': 0x80}
    # The current swap's source and destination rectangles of layer 0, and its layer mask, as getCurrentSrcRect_gated and
    # getCurrentDstRect_gated read them: ldr w10, [x9, #masks] (the layer's bit), then add x9, x9, #rect.
    for key, symbol in (('rect0', '__ZN25IOMobileFramebufferLegacy23getCurrentSrcRect_gatedEj'),
                        ('rect1', '__ZN25IOMobileFramebufferLegacy23getCurrentDstRect_gatedEj')):
        words = struct.unpack('<16I', kc.code(kc.symbol(entry, symbol), 64))
        if 0xb9400000 | (request['masks'] // 4) << 10 | 9 << 5 | 10 not in words:
            raise SystemExit(f'{symbol} does not test the layer mask at {request["masks"]:#x}')
        if 0x91000000 | request[key] << 10 | 9 << 5 | 9 not in words:
            raise SystemExit(f'{symbol} does not take the rectangle at {request[key]:#x}')
    width = kc.code(kc.symbol(entry, '__ZNK25IOMobileFramebufferLegacy13display_widthEv'), 12)
    if struct.unpack_from('<I', width, 4)[0] != 0xb9400000 | (display['width'] // 4) << 10:
        raise SystemExit('display_width does not load the width field')
    return {
        'kernelcache_sha256': kc.sha256, 'collection_slide': hex(NATIVE_LINK_ADDRESS - kc.cache.base),
        'vtable': hex(start), 'instance_size': hex(kc.instance_size(entry, cls)),
        'slots': [[hex(t), hex(d)] for (t, _), d in zip(slots, required)],
        'base_slots': base, 'pure_target': hex(pure), 'family_text': [hex(a) for a in text_range(kc, entry)],
        'methods': methods, 'leaves': [], 'prototypes': prototypes, 'bindings': bindings, 'checks': checks,
        'display': display, 'request': request, 'notify': notify, 'names': names, 'caller_diversity': {},
        'abi': 'macos', 'build': build,
    }


def text_range(kc, entry):
    image = kc.Image(kc.raw, kc.cache.entries[entry][1])
    segment = next(s for s in image.segments if s['name'] == '__TEXT_EXEC')
    return segment['va'], segment['va'] + segment['size']


def hid_fixture(kc):
    entry, cls = 'com.apple.iokit.IOHIDFamily', 'IOHIDDevice'
    start, slots = kc.vtable(entry, cls)
    return {'kernelcache_sha256': kc.sha256, 'collection_slide': hex(NATIVE_LINK_ADDRESS - kc.cache.base),
            'vtable': hex(start), 'instance_size': hex(kc.instance_size(entry, cls)),
            'slots': [[hex(t), hex(d)] for t, d in slots], 'base_slots': kc.service_slots(), 'pure_target': hex(kc.pure()),
            'bindings': kc.bindings(entry, cls)}


def ether_fixture(kc):
    """macOS: IONetworkingFamily's IOEthernetController, for the board's Ethernet controller (ShengEthernet.cpp, compiled with the
    SDK's public IONetworkingFamily headers). The vtable is checked slot by slot at build time; the bindings are every exported
    symbol of the family classes the controller uses, so the build binds what the object imports and nothing else."""
    entry, cls = 'com.apple.iokit.IONetworkingFamily', 'IOEthernetController'
    start, slots = kc.vtable(entry, cls)
    prefixes = tuple(f'__ZN{len(c)}{c}' for c in ('IONetworkController', 'IOEthernetController', 'IONetworkMedium',
                                                  'IONetworkInterface', 'IOEthernetInterface', 'IOOutputQueue'))
    prefixes += tuple('__ZNK' + p[4:] for p in prefixes)
    bindings = dict(kc.bindings(entry, cls))
    for name, address in sorted(kc.by_entry[entry].items()):
        if name.startswith(prefixes) and '_os_log' not in name and 'kalloc_type_view' not in name and '.cold' not in name:
            bindings.setdefault(name, hex(address))
    return {'kernelcache_sha256': kc.sha256, 'collection_slide': hex(NATIVE_LINK_ADDRESS - kc.cache.base),
            'vtable': hex(start), 'instance_size': hex(kc.instance_size(entry, cls)),
            'slots': [[hex(t), hex(d)] for t, d in slots], 'base_slots': kc.service_slots(), 'pure_target': hex(kc.pure()),
            'bindings': bindings}


def aes_fixture(kc):
    """macOS: IOCryptoAcceleratorFamily's IOAESAccelerator, for the software AES-CBC provider (UniXNU drivers/SoftwareAES, NativeAES.cpp
    with OriginalAESABI.h). AppleImage4's start waits for an IOAESAccelerator service without a limit (kmod_expert_finalize,
    waitForMatchingService); this board has no AES engine node. The vtable is checked slot by slot at build time; the bindings are the
    exported symbols of the family's two classes."""
    entry, cls = 'com.apple.iokit.IOCryptoAcceleratorFamily', 'IOAESAccelerator'
    start, slots = kc.vtable(entry, cls)
    prefixes = tuple(f'__ZN{len(c)}{c}' for c in ('IOCryptoAccelerator', 'IOAESAccelerator'))
    prefixes += tuple('__ZNK' + p[4:] for p in prefixes)
    bindings = dict(kc.bindings(entry, cls))
    for name, address in sorted(kc.by_entry[entry].items()):
        if name.startswith(prefixes) and '_os_log' not in name and 'kalloc_type_view' not in name and '.cold' not in name:
            bindings.setdefault(name, hex(address))
    return {'kernelcache_sha256': kc.sha256, 'collection_slide': hex(NATIVE_LINK_ADDRESS - kc.cache.base),
            'vtable': hex(start), 'instance_size': hex(kc.instance_size(entry, cls)),
            'slots': [[hex(t), hex(d)] for t, d in slots], 'base_slots': kc.service_slots(), 'pure_target': hex(kc.pure()),
            'bindings': bindings}


def usb_fixture(kc, reference, entry, cls, macros):
    start, slots = kc.vtable(entry, cls)
    base = kc.service_slots()
    roles = reference_roles(reference, [d for _, d in slots])
    methods = {}
    prefix = f'_ZN{len(cls)}{cls}'
    reviewed = {roles[int(k)]: v for k, v in reference['methods'].items() if int(k) in roles}
    prototypes = {}
    for slot in range(base, len(slots)):
        method = reviewed.get(slot, {'returns': 'void', 'parameters': ''})
        prototypes[f'm{slot}'] = f'{method["returns"]} m{slot}({method["parameters"]})'
    suffixes = mangled_suffixes(cls, prototypes)
    requests = [(prefix, slot, suffixes[f'm{slot}'], slots[slot][1]) for slot in range(base, len(slots))]
    for (_, slot, _, _), name in zip(requests, opaque_names(requests)):
        method = reviewed.get(slot, {'returns': 'void', 'parameters': ''})
        methods[str(slot)] = {'name': name, 'returns': method['returns'], 'parameters': method['parameters']}
    named = {}
    for reference_slot, macro in macros.items():
        if reference_slot not in roles:
            raise SystemExit(f'{cls}: {macro} (reference slot {reference_slot}) has no slot here')
        named[str(roles[reference_slot])] = macro
    text = kc.kernel_text()
    return {'kernelcache_sha256': kc.sha256, 'collection_slide': hex(NATIVE_LINK_ADDRESS - kc.cache.base),
            'vtable': hex(start), 'instance_size': hex(kc.instance_size(entry, cls)),
            'slots': [[hex(t), hex(d)] for t, d in slots], 'base_slots': base, 'pure_target': hex(kc.pure()),
            'kernel_text': [hex(text[0]), hex(text[1])], 'methods': methods, 'macros': named,
            'service_overrides': sorted({name for slot, (t, _) in enumerate(slots[:base]) if not text[0] <= t < text[1]
                                         and slot not in (0, 1, 7) for name in kc.names.get(t, [])[:1]}),
            'bindings': kc.bindings(entry, cls)}


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--kernelcache', type=Path, required=True)
    parser.add_argument('--oracle', type=Path, required=True)
    parser.add_argument('--build', required=True)
    parser.add_argument('--reference-build', default='24A446')
    args = parser.parse_args()
    kc, oracle = Collection(args.kernelcache), Collection(args.oracle)
    reference = lambda stem: json.loads((HERE / f'{stem}-{args.reference_build}.json').read_text())  # noqa: E731
    outputs = {
        'legacy-display': legacy_fixture(kc, oracle, reference('legacy-display'), args.build),
        'hid-device': hid_fixture(kc),
        'usb-xdci': usb_fixture(kc, reference('usb-xdci'), 'com.apple.driver.AppleUSBXDCI', 'AppleUSBXDCI',
                                {179: 'XDCI_KEEP_CONFIGURATIONS', 227: 'XDCI_DESIRED_POWER', 267: 'XDCI_TRANSPORT_ACTIVE',
                                 268: 'XDCI_POWER_ON', 269: 'XDCI_POWER_OFF', 270: 'XDCI_PARAMETERS', 275: 'XDCI_CONTROLLER_RESET',
                                 277: 'XDCI_PERIODIC_USAGE', 278: 'XDCI_PERIODIC_SET'}),
        'net-ether': ether_fixture(kc),
        'software-aes': aes_fixture(kc),
        'usb-xhci': usb_fixture(kc, reference('usb-xhci'), 'com.apple.driver.usb.AppleUSBXHCI', 'AppleUSBXHCI',
                                {188: 'XHCI_DESIRED_POWER', 315: 'XHCI_READ64', 317: 'XHCI_WRITE64'}),
    }
    for stem, payload in outputs.items():
        path = HERE / f'{stem}-{args.build}.json'
        path.write_text(json.dumps(payload, indent=1) + '\n')
        print(f'wrote {path.name}: {len(payload["slots"])} slots')


if __name__ == '__main__':
    main()
