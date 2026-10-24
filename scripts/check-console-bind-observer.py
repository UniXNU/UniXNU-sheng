#!/usr/bin/env python3
"""Qualify a console dispatch route and reproduce observer rebinding on host.

No VM, device operation, privileged instruction or framebuffer write. The host
executes the production binding function with drawing replaced by address sinks
and single-threaded stubs for the recorder lock and clock dependencies.
This demonstrates a mechanism; it cannot identify a past physical invocation.
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
sys.path.insert(0, str(ROOT / 'kits/XnuPrelink'))
from native_prelink import macho, backed_offset, exports


def require(condition, message):
    if not condition:
        raise SystemExit(message)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kernel', type=Path, required=True)
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    raw = args.kernel.read_bytes()
    image = macho(raw)
    symbols = {name: va for va, names in exports(raw, image).items() for name in names}

    def read(address, size):
        at = backed_offset(image['segments'], address, size, len(raw))
        return raw[at:at + size]

    def branch(address):
        word, = struct.unpack('<I', read(address, 4))
        require(word & 0x7c000000 == 0x14000000, 'Expected immediate B/BL')
        imm = word & 0x3ffffff
        return address + 4 * (imm - (1 << 26) if imm & (1 << 25) else imm)

    wrapper = symbols['__ZN16IOPlatformExpert14setConsoleInfoEP8PE_Videoj']
    require(read(wrapper, 8).hex() == 'e00301aae10302aa', 'Console wrapper argument moves changed')
    require(branch(wrapper + 8) == symbols['_PE_initialize_console'], 'Console tail target changed')
    vtable = symbols['__ZTV16IOPlatformExpert']
    require(struct.unpack('<Q', read(vtable + 16 + 188 * 8, 8))[0] == wrapper,
            'Platform expert slot 188 no longer has the qualified console target')

    # Qualify the actual original Legacy body, rather than identifying arbitrary
    # virtual calls solely by their coincidentally equal slot numbers.
    checks = [
        (0xfffffff00578c8c8, 'a3580094080040f908f142f9e1a302912200805200013fd6'),
        (0xfffffff00578c8e8, '9b580094e89f41390a010012a9008052880080524a0100720211891a080040f908f142f9010080d200013fd6'),
        # One branch calls a method on IOSurface's +0x28 object and adds the
        # surface offset. That object's dynamic type is not qualified here.
        (0xfffffff00578c62c, 'e02b40f9080040f9085540f900013fd6e01700f9e02f40f9b6590094e80300aae01740f90840288be85700f9'),
        # The alternative descriptor mapping path also installs a virtual base.
        (0xfffffff00578c744, 'e02740f9080040f9083d40f900013fd6e05700f9'),
        # Do not omit the entry guard: +0x1782 must be nonzero. Its checked
        # initializer explicitly clears it; a later physical value is unknown.
        (0xfffffff00578c484, '49f082d20801098b08014039c801003601000014'),
        (0xfffffff005787ca8, '4af082d2e90308aa29010a8b3f010039'),
        # Both IOSurface choices being null exits before constructing PE_Video.
        (0xfffffff00578c594, 'e82f40f9080100f1e8079f1a6800003701000014ef000014'),
    ]
    for address, expected in checks:
        require(read(address, len(expected) // 2).hex() == expected,
                f'Original console-route bytes changed at {address:#x}')
    getter_stub = branch(0xfffffff00578c8c8)
    require(getter_stub == 0xfffffff0057a2b54, 'Platform getter stub changed')
    require(read(getter_stub, 12).hex() == '903c00b010ee45f900021fd6',
            'Platform getter stub instructions changed')
    require(struct.unpack('<Q', read(0xfffffff005f33bd8, 8))[0] ==
            symbols['__ZN9IOService11getPlatformEv'], 'Receiver is not the qualified platform getter')

    source = (ROOT / 'kits/sheng/BootProgress.c').read_text()
    begin = source.index('void boot_progress_bind(')
    end = source.index('\nvoid boot_progress_putc(', begin)
    binding = source[begin:end]
    # Address sinks replace pixel-producing dependencies; recorder-lock and
    # clock stubs model an uncontended invocation, not actual synchronization.
    # Keep production address validation, state transitions and ownership gates.
    harness = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
'''
    harness += '#include "BootProgressCore.h"\n'
    display_header = (ROOT / 'kits/sheng/BootProgressDisplay.h').read_text()
    harness += display_header[display_header.index('struct bp_display {'):
                              display_header.index('static void bp_clear')]
    harness += '#include "BootProgressPulse.h"\nstatic struct bp_pulse pulse;\nstatic struct bp_panic_info boot_progress_panic_vinfo;\n'
    harness += r'''
static struct bp_display display;
static unsigned enabled=2, boot_progress_display_owned;
static unsigned bind_count,bind_result,bind_depth;
static uintptr_t bind_base;
static uintptr_t cleared, drawn;
static unsigned draws;
static int take(void) { return 1; }
static void release(void) {}
static unsigned seconds(void) { return 74; }
static void bp_clear(struct bp_display *d) { if(d->bound) cleared=d->frame; }
static void draw(unsigned now) { (void)now; if(display.bound){drawn=display.frame;++draws;} }
'''
    harness += binding
    harness += r'''
int main(void) {
  const uintptr_t scanout=UINT64_C(0xfffffff100000000);
  const uintptr_t surface=UINT64_C(0xfffffff200000000);
  boot_progress_bind(scanout,3048,2032,12192,32);
  assert(display.bound && cleared==scanout && drawn==scanout && draws==1);
  assert(boot_progress_panic_vinfo.frame==scanout);
  pulse.ready=1; // A retained anchor is separately qualified by check-boot-pulse.
  boot_progress_bind(surface,3048,2032,12192,32);
  assert(display.bound && cleared==surface && drawn==surface && draws==2);
  assert(boot_progress_panic_vinfo.frame==scanout);
  draw(75);
  assert(drawn==surface && draws==3 && !boot_progress_display_owned);
  boot_progress_bind(surface,3048,2032,12192,30);
  assert(!display.bound && !boot_progress_display_owned);
  draw(76); assert(draws==3);
  boot_progress_bind(scanout,3048,2032,12192,32);
  boot_progress_display_owned=1;
  boot_progress_bind(surface,3048,2032,12192,32);
  assert(display.frame==scanout); // A completed explicit handoff blocks rebinding.
  puts("PASS: actual bind redirects writes to a new valid base; depth 30 disables drawing; neither needs handoff");
}
'''
    with tempfile.TemporaryDirectory(prefix='console-bind-') as tmp:
        c = Path(tmp) / 'binding.c'
        exe = Path(tmp) / 'binding'
        c.write_text(harness)
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-Wno-unused-function', '-I', str(ROOT / 'kits/sheng'), str(c), '-o', str(exe)],
                       check=True, timeout=30)
        result = subprocess.run([str(exe)], check=True, text=True, capture_output=True, timeout=10)
    report = {
        'kernel_sha256': hashlib.sha256(raw).hexdigest(),
        'binding_source_sha256': hashlib.sha256(binding.encode()).hexdigest(),
        'qualified_original_function': hex(0xfffffff00578c44c),
        'platform_expert_console_slot': 188,
        'non_null_video_operation': 1,
        'null_video_operations': [4, 5],
        'entry_guard_field_offset': '0x1782',
        'entry_guard_initialized_to': 0,
        'entry_guard_required_for_route': 'nonzero',
        'physical_entry_guard_value': None,
        'surface_plus_0x28_dynamic_class_qualified': False,
        'host_result': result.stdout.strip(),
        'pixel_writes_executed': False,
        'physical_invocation_observed': False,
        'physical_root_cause_proven': False,
        'vm_executed': False,
        'device_operation': False,
    }
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2) + '\n')
    print(result.stdout.strip())
    print('Physical trigger remains unobserved; this result does not prove the physical root cause.')


if __name__ == '__main__':
    main()
