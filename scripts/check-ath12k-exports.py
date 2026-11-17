#!/usr/bin/env python3
"""Check that every entry point the board takes from the Wi-Fi module keeps its name in the kernel (scripts/build-ath12k-wifi.py).

build-ath12k-wifi.py gives each global name of the Ath12kWifi objects the prefix nwk_ except those in its `keep` set, so that the
module's Linux names (sock_release, kernel_sendmsg, ...) cannot clash in the kernel. A function the board calls through
include/nw_abi.h but missing from that set is renamed too, and the kernel link fails: nw_wifi_scan_frequencies did
(`Undefined symbols: _nw_wifi_scan_frequencies, referenced from WifiNet.late.o`).
"""
import ast
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def declared(header):
    text = re.sub(r'/\*.*?\*/', '', header.read_text(), flags=re.S)
    return sorted(set(re.findall(r'\b(nw_wifi_[a-z0-9_]+)\s*\(', text)))


def kept(script):
    source = script.read_text()
    names = set()
    for match in re.finditer(r'^keep\s*=\s*(\{.*?\})', source, re.S | re.M):
        names |= ast.literal_eval(match.group(1))
    for match in re.finditer(r'^keep\.update\((\{.*?\})\)', source, re.S | re.M):
        names |= ast.literal_eval(match.group(1))
    for match in re.finditer(r"^keep\.add\('([^']+)'\)", source, re.M):
        names.add(match.group(1))
    return names


def main():
    header = ROOT / 'drivers/Ath12kWifi/include/nw_abi.h'
    if not header.is_file():
        raise SystemExit(f'{header}: missing (drivers/Ath12kWifi is not checked out)')
    names = kept(ROOT / 'scripts/build-ath12k-wifi.py')
    if not names:
        raise SystemExit('FAIL: no keep set found in scripts/build-ath12k-wifi.py')
    functions = declared(header)
    missing = [f for f in functions if '_' + f not in names]
    if missing:
        sys.stderr.write('renamed to nwk_ although nw_abi.h declares them: ' + ', '.join(missing) + '\n')
        raise SystemExit('FAIL: Ath12kWifi exports')
    print(f'PASS: Ath12kWifi exports: all {len(functions)} nw_abi.h functions keep their names')


if __name__ == '__main__':
    main()
