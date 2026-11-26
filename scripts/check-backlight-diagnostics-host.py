#!/usr/bin/env python3
"""Exercise production backlight methods and GENI core against host MMIO/IOKit fakes."""
import ast
import hashlib
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PACKAGE = ROOT / 'kits/sheng' if (ROOT / 'kits/sheng').is_dir() else ROOT / 'package'
source = (PACKAGE / 'AppleARMBacklight.cpp').read_text()


def method(signature):
    start = source.index('    ' + signature)
    opening = source.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


signatures = [
    'static uint64_t now(', 'static uint64_t after(', 'static bool ktzNumber(', 'void publishKtz(',
    'void cancelSnapshots(', 'bool snapshotInterrupted(', 'static void diagnosticFired(',
    'void kick(', 'void noteFailure(', 'void finished(', 'void advance(', 'void armTick(', 'void service(',
    'static void requestFired(', 'static void lidFired(', 'void setLevel(', 'static void fadeStep(',
    'void beginFade(', 'void publish(uint32_t', 'void publishFirst(', 'static uint32_t milliNitsForLevel(',
    'static uint32_t levelForNits(', 'IOReturn setProperties(',
]
state = source[source.index('    enum Step :'):source.index('    static uint64_t now(')]
constants = source[source.index('constexpr uint32_t kMinNits'):source.index('\nstruct Window')]
production = '\n'.join(method(signature) for signature in signatures)
start = method('bool startBacklight(')
setup = start[start.index('        diagnostic = '):start.index('        if (!registerSleepWakeInterest')]
production += '\n    void hostDiagnosticSetup() {\n#if defined(XNU_TARGET_OS_OSX) && XNU_TARGET_OS_OSX\n' + setup + '#endif\n    }\n'
program = (PACKAGE / 'tests/backlight_diagnostics_host.cpp').read_text()
program = program.replace('/* PRODUCTION_CONSTANTS */', constants)
program = program.replace('/* PRODUCTION_STATE */', state).replace('/* PRODUCTION_METHODS */', production)

# Run the actual source-list expression and hash function, without importing/executing the builder.
builder = (ROOT / 'scripts/build-gop-legacy-display-27.py').read_text()
tree = ast.parse(builder)
header = PACKAGE / 'Ktz8866Diagnostics.h'
namespace = {'KITS': PACKAGE, 'Path': Path, 'hashlib': hashlib, 'sources': []}
for node in tree.body:
    if isinstance(node, ast.FunctionDef) and node.name == 'sha':
        exec(compile(ast.Module(body=[node], type_ignores=[]), '<builder-sha>', 'exec'), namespace)
    if isinstance(node, ast.AugAssign) and isinstance(node.target, ast.Name) and node.target.id == 'sources':
        if 'Ktz8866Diagnostics.h' in ast.unparse(node):
            exec(compile(ast.Module(body=[node], type_ignores=[]), '<builder-inputs>', 'exec'), namespace)
assert header in namespace['sources'] and PACKAGE / 'AppleARMBacklight.cpp' in namespace['sources']
assert namespace['sha'](header) == hashlib.sha256(header.read_bytes()).hexdigest()
print('PASS: display archive source_sha256 includes production backlight and diagnostic header', flush=True)

# iPadOS creates no diagnostic timer. Validate the actual preprocessed startBacklight body too.
bootlog = (PACKAGE / 'BootLog.c').read_text()
block = bootlog[bootlog.index('  // KTZ chip response,'):bootlog.index('  if (&boot_progress_ext_wide)')]
boot_program = '''#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "Ktz8866Diagnostics.h"
extern volatile int32_t native_backlight_ktz[2][KtzDiagCount] __attribute__((weak_import));
static size_t put(char *out, size_t at, size_t cap, const char *row, size_t bound) {
    size_t n = strnlen(row, bound);
    if (at + n + 1 >= cap) return at;
    memcpy(out + at, row, n); out[at + n] = '\\n'; out[at + n + 1] = 0;
    return at + n + 1;
}
size_t host_bootlog_ktz(char *out, size_t cap) {
    size_t at = 0;
''' + block + '\nreturn at;\n}\n'
flags = ['-Wall', '-Wextra', '-Werror', '-O1', '-g', '-fsanitize=address,undefined',
         '-fno-sanitize-recover=all', '-fno-omit-frame-pointer']
with tempfile.TemporaryDirectory(prefix='backlight-diagnostics-') as tmp:
    temp = Path(tmp)
    test = temp / 'backlight-diagnostics.cpp'
    test.write_text(program)
    boot = temp / 'bootlog-ktz.c'
    boot.write_text(boot_program)
    core = temp / 'geni.o'
    boot_obj = temp / 'bootlog.o'
    for src, obj in [(PACKAGE / 'GeniI2c.c', core), (boot, boot_obj)]:
        subprocess.run(['cc', '-std=c11', *flags, '-I', str(PACKAGE), '-c', str(src), '-o', str(obj)], check=True)
    for name, defines in [('macos', ['-DXNU_TARGET_OS_OSX=1']), ('ipados', ['-DXNU_TARGET_OS_OSX=0']), ('unset', [])]:
        preprocessed = subprocess.check_output(['c++', '-E', '-P', '-x', 'c++', *defines, '-'], input=start, text=True)
        assert ('diagnostic = IOTimerEventSource::timerEventSource' in preprocessed) == (name == 'macos')
        exe = temp / ('backlight-diagnostics-' + name)
        subprocess.run(['c++', '-std=gnu++2b', *flags, *defines, '-I', str(PACKAGE), str(test),
                        str(core), str(boot_obj), '-pthread', '-o', str(exe)], check=True)
        print('Running ' + name + ': production methods + real GeniI2c.c + production BootLog block; ASan/UBSan, warnings as errors', flush=True)
        result = subprocess.run([str(exe)], text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        sys.stdout.write(result.stdout)
        sys.stderr.write(result.stderr)
        if result.returncode or not result.stdout.startswith('PASS:'):
            raise SystemExit('FAIL: backlight diagnostics (' + name + ')')
print('PASS: backlight diagnostics host checks')
