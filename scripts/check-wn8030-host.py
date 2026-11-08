#!/usr/bin/env python3
"""Host tests of the WN8030 keyboard controller codec (package/tests/wn8030_host.c). With the extracted controller firmware in
.cache/firmware, its boot ROM commands are also compared with an encoding of the boot ROM structures written here. Model tests only."""
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PACKAGE = ROOT / 'kits/sheng' if (ROOT / 'kits/sheng').is_dir() else ROOT / 'package'
FIRMWARE = ROOT / '.cache/firmware/nanosic/MCU_Upgrade.bin'


def expected(image):
    """The header command and the first and last data commands, each with the register byte 0x5c."""
    h = image[0x7fc0:0x8000]
    code = struct.unpack_from('<I', h, 48)[0]
    body = image[0x8000:0x8000 + code]
    words = sum(struct.unpack_from('<I', body.ljust((len(body) + 3) & ~3, b'\0'), i)[0] for i in range(0, len(body), 4)) & 0xffffffff
    header = bytearray(struct.pack('<B7sHIIIIHHBB17sIIIBBBB', 6, h[:7], 0x8140, 0x2345, 0x40, code,
                                   struct.unpack_from('<I', h, 21)[0], struct.unpack_from('<H', h, 25)[0],
                                   struct.unpack_from('<H', h, 27)[0], h[29], h[30], b'', 0, words, 0, 0, 0, 0, 0))
    assert len(header) == 65
    header[64] = sum(header[1:64]) & 0xff

    def data(offset):
        n = min(503, code - offset)
        return b'\x5c\x07' + struct.pack('>II', offset, n) + body[offset:offset + n].ljust(503, b'\0')
    last = (code - 1) // 503 * 503
    return b'\x5c' + bytes(header), data(0), data(last), -(-code // 503), code


with tempfile.TemporaryDirectory() as tmp:
    exe = Path(tmp) / 'wn8030-host'
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-g', '-fsanitize=address,undefined',
                    str(PACKAGE / 'tests/wn8030_host.c'), str(PACKAGE / 'Wn8030.c'), '-o', str(exe)], check=True)
    result = subprocess.run([str(exe)] + ([str(FIRMWARE)] if FIRMWARE.is_file() else []), capture_output=True, text=True)
sys.stderr.write(result.stderr)
lines = dict(line.split(' ', 1) for line in result.stdout.splitlines() if ' ' in line)
ok = result.returncode == 0 and result.stdout.rstrip().splitlines()[-1].startswith('PASS')
if ok and FIRMWARE.is_file():
    header, first, last, chunks, code = expected(FIRMWARE.read_bytes())
    ok = (bytes.fromhex(lines['HEADER']) == header and bytes.fromhex(lines['FIRST']) == first and
          bytes.fromhex(lines['LAST']) == last and lines['CHUNKS'] == f'{chunks} CODE {code}')
    print(f'{"PASS" if ok else "FAIL"} WN8030 firmware commands: {chunks} chunks, {code} code bytes')
elif ok:
    print('SKIP WN8030 firmware commands: run scripts/extract-files.py --section Keyboard')
print(result.stdout.rstrip().splitlines()[-1] if result.stdout else 'no output')
sys.exit(0 if ok else 1)
