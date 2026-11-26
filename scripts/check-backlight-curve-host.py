#!/usr/bin/env python3
"""Host checks of the production nits/level mapping in AppleARMBacklight.cpp."""
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PACKAGE = ROOT / 'kits/sheng' if (ROOT / 'kits/sheng').is_dir() else ROOT / 'package'
source = (PACKAGE / 'AppleARMBacklight.cpp').read_text()


def function(name):
    start = source.index('    static uint32_t ' + name + '(')
    opening = source.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


constants = '\n'.join('constexpr uint32_t ' + name + ' = ' + value + ';'
                      for name, value in re.findall(r'\b(kMinNits|kMaxNits|kMinLevel|kMaxLevel) = (\d+)', source))
program = r'''
#include <stdint.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
CONSTANTS
FUNCTIONS
static unsigned checks;
static void check(bool ok, const char *message) {
    ++checks;
    if (!ok) { fprintf(stderr, "FAIL: %s\n", message); exit(1); }
}
int main() {
    check(kMinNits == 15 && kMaxNits == 500 && kMinLevel == 892 && kMaxLevel == 2047, "preserved curve endpoints");
#if defined(XNU_TARGET_OS_OSX) && XNU_TARGET_OS_OSX
    check(levelForNits(INT32_MIN) == 892, "negative request keeps visible floor");
    check(levelForNits(0) == 892, "zero request keeps visible floor");
    check(levelForNits(15 << 16) == 892, "minimum endpoint");
    check(levelForNits(500 << 16) == 2047, "maximum endpoint");
    check(levelForNits(INT32_MAX) == 2047, "oversized request clamps");
    check(milliNitsForLevel(0) == 15000, "low readback clamps");
    check(milliNitsForLevel(UINT32_MAX) == 500000, "high readback clamps");
    uint32_t previous = 0;
    for (int32_t fixed = 0; fixed <= (501 << 16); fixed += 256) {
        const uint32_t level = levelForNits(fixed);
        check(level >= 892 && level <= 2047, "bounded output");
        check(level >= previous, "monotonic request mapping");
        previous = level;
        if (fixed > (15 << 16) && fixed < (500 << 16)) {
            const long double nits = fixed / 65536.0L;
            const long double ideal = logl(nits) / logl(1.003040572L);
            check(fabsl((long double)level - ideal) < 1.5L, "integer curve matches independent exponential inverse");
        }
    }
    previous = 0;
    for (uint32_t level = 892; level <= 2047; ++level) {
        const uint32_t milli = milliNitsForLevel(level);
        check(milli >= previous, "monotonic readback");
        previous = milli;
        const uint32_t roundtrip = levelForNits((int32_t)(((uint64_t)milli << 16) / 1000));
        check(abs((int)roundtrip - (int)level) <= 1, "roundtrip within one hardware level");
        const long double idealMilli = 1000.0L * powl(1.003040572L, (long double)level);
        const long double clampedIdeal = idealMilli < 15000.0L ? 15000.0L : idealMilli > 500000.0L ? 500000.0L : idealMilli;
        check(fabsl((long double)milli - clampedIdeal) <= 1.0L,
              "readback matches exponential response within one millinit");
    }
    const uint32_t q0 = levelForNits(15 << 16), q1 = levelForNits((15 << 16) + (485 << 14));
    const uint32_t q2 = levelForNits((15 << 16) + (485 << 15));
    const uint32_t q3 = levelForNits((15 << 16) + 3 * (485 << 14)), q4 = levelForNits(500 << 16);
    check(q1 - q0 > q2 - q1 && q2 - q1 > q3 - q2 && q3 - q2 > q4 - q3,
          "upper quarters increase more gradually than lower quarters");
    check(levelForNits(140 << 16) >= 1625 && levelForNits(140 << 16) <= 1630, "140 nits maps to ~1628 in exponential mode");
    const int nits[] = {15, 20, 39, 50, 100, 140, 265, 500};
    for (int value : nits) printf("%d nits -> level %u\n", value, levelForNits(value << 16));
#else
    check(levelForNits(INT32_MIN) == 0, "iPadOS negative request stays off");
    check(levelForNits(0) == 0, "iPadOS zero request stays off");
    check(levelForNits(INT32_MAX) == 2047, "iPadOS oversized request clamps");
    for (int32_t fixed = 1; fixed <= (501 << 16); fixed += 257) {
        const uint64_t scale = 500ULL << 16;
        const uint64_t scaled = ((uint64_t)fixed * 2047 + scale / 2) / scale;
        const uint32_t original = scaled < 1 ? 1 : scaled > 2047 ? 2047 : (uint32_t)scaled;
        check(levelForNits(fixed) == original, "iPadOS request matches previous proportional mapping");
    }
    for (uint32_t level = 0; level <= 2047; ++level)
        check(milliNitsForLevel(level) == (uint64_t)level * 500000 / 2047,
              "iPadOS readback stays unchanged");
#endif
    printf("PASS: %u backlight curve checks\n", checks);
}
'''.replace('CONSTANTS', constants).replace('FUNCTIONS', function('milliNitsForLevel') + '\n' + function('levelForNits'))

with tempfile.TemporaryDirectory() as tmp:
    test = Path(tmp) / 'backlight-curve-host.cpp'
    test.write_text(program)
    for name, flags in [('macos', ['-DXNU_TARGET_OS_OSX=1']), ('ipados', ['-DXNU_TARGET_OS_OSX=0']), ('unset', [])]:
        exe = Path(tmp) / ('backlight-curve-' + name)
        subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-O1', '-g',
                        '-fsanitize=address,undefined', *flags, str(test), '-o', str(exe)], check=True)
        result = subprocess.run([str(exe)], capture_output=True, text=True)
        sys.stderr.write(result.stderr)
        print(name + ':\n' + result.stdout.strip())
        if result.returncode:
            raise SystemExit('FAIL: backlight curve host tests (' + name + ')')
print('PASS: backlight curve host tests')
