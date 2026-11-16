#!/usr/bin/env python3
"""Qualify inherited watchdog servicing in an existing source-built kernel."""
import argparse
import hashlib
import json
from pathlib import Path
import runpy
import struct
import tomllib

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('input', type=Path)
parser.add_argument('output', type=Path)
parser.add_argument('--kernel-args', required=True)
args = parser.parse_args()
if 'uefi-watchdog=1' not in args.kernel_args.split():
    parser.error('require the inherited-watchdog boot profile')
if args.output.exists():
    parser.error('choose a new output kernel')
root = Path(__file__).resolve().parents[1]
core = (root / tomllib.loads((root / 'board.toml').read_text())['package']['unixnu']).resolve()
probe = runpy.run_path(str(core / 'scripts/patch-xnu-entry-probe.py'))
symbols = {name: address for address, name in probe['kernel_symbols'](args.input).items()}
start = symbols['_boot_reset_watchdog']
end = symbols['_wdt_thread']
data = bytearray(args.input.read_bytes())
probe['load_segments'](data)
offset = probe['file_offset'](start)
body = bytes(data[offset:offset + end - start])
# The control call's zero arguments, function ID, enable operation and SMC.
pattern = bytes.fromhex('020080d2030080d2a000805200c0b07261008052030000d4')
if body.count(pattern) != 1:
    parser.error('require one matching watchdog-enable sequence')
site = offset + body.index(pattern) + len(pattern) - 4
# Firmware has armed the timer and the preceding PING has succeeded. Its
# inherited path proceeds directly to creation of the servicing thread.
struct.pack_into('<I', data, site, 0x52800000)  # mov w0, #0
args.output.write_bytes(data)
report = dict(input_sha256=hashlib.sha256(args.input.read_bytes()).hexdigest(),
              output_sha256=hashlib.sha256(data).hexdigest(),
              enable_site=hex(start + site - offset),
              required_argument='uefi-watchdog=1', hardware_tested=False)
args.output.with_suffix('.watchdog.json').write_text(json.dumps(report, indent=2) + '\n')
print(f'{args.output}: inherited watchdog PING and service thread retained')
