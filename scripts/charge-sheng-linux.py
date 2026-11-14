#!/usr/bin/env python3
"""Read sheng charging telemetry, or run its macOS authentication state machine on Linux sysfs."""
import argparse
import ctypes
import errno
import glob
import hashlib
import hmac
import json
import os
from pathlib import Path
import secrets
import struct
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
PACKAGE = ROOT / 'kits/sheng' if (ROOT / 'kits/sheng').is_dir() else ROOT / 'package'
PROPERTIES = {
    0x03: 'authentic', 0xa5: 'slave_authentic', 0x16: 'adapter_id', 0x17: 'adapter_svid',
    0x18: 'pd_verifed', 0x19: 'pdo2', 0x32: 'apdo_max', 0x33: 'power_max',
    0x2b: 'fastchg_mode', 0x31: 'quick_charge_type', 0xe7: 'pps_ptf',
    0x1b: 'bq2597x_chip_ok', 0x1c: 'bq2597x_slave_chip_ok', 0x1d: 'bq2597x_bus_current',
    0x1e: 'bq2597x_slave_bus_current', 0x20: 'bq2597x_bus_voltage', 0x23: 'bq2597x_battery_voltage',
}
VDM_COMMANDS = {0x0d: 1, 0x0e: 2, 0x0f: 3, 0x10: 4, 0x11: 5, 0x12: 6, 0x13: 7, 0x14: 8}
STATES = ('idle', 'gauge-check', 'gauge-challenge', 'gauge-digest', 'gauge-mark', 'adapter-check',
          'host-role', 'vdm-write', 'vdm-state', 'vdm-digest', 'adapter-id', 'verified', 'reverse',
          'compensation', 'pd-mark', 'diagnostics', 'done', 'failed')
WRAPPER = '''#include "ShengChargeAuth.h"
#include <stddef.h>
size_t auth_size(void) {return sizeof(ShengChargeAuth);}
void auth_open(ShengChargeAuth *a, void (*r)(void *, uint32_t),
 void (*h)(const uint8_t *, uint32_t, const uint8_t *, uint32_t, uint8_t *)) {
 ShengChargeCrypto c={r,h}; sheng_charge_auth_init(a,&c);
}
unsigned auth_state(const ShengChargeAuth *a) {return a->state;}
void auth_host(ShengChargeAuth *a, int host) {a->host_ready=host;}
uint64_t auth_due(const ShengChargeAuth *a) {return a->due_ms;}
void auth_status(const ShengChargeAuth *a, int32_t *out) {
 out[0]=a->result; out[1]=a->last_result; out[2]=a->gauge_verified; out[3]=a->adapter_verified;
 out[4]=a->reads; out[5]=a->writes; out[6]=a->errors; out[7]=a->timeouts; out[8]=a->retries;
}
'''


def read(path):
    return Path(path).read_text().strip()


def write(path, value):
    with Path(path).open('w') as stream:
        stream.write(str(value))


def number(path, hexadecimal=False):
    text = read(path)
    return int(text, 16 if hexadecimal else 10)


def telemetry(root):
    result = {}
    for name in ('real_type', *PROPERTIES.values()):
        try:
            result[name] = read(root / name)
        except OSError as error:
            result[name] = {'error': error.errno}
    for supply in ('usb', 'battery'):
        base = Path('/sys/class/power_supply') / supply
        values = {}
        for name in ('online', 'voltage_now', 'current_now', 'current_max', 'input_current_limit', 'temp', 'capacity', 'status'):
            try:
                values[name] = read(base / name)
            except OSError:
                pass
        result[supply] = values
    return result


def swap_data_role():
    paths = sorted(glob.glob('/sys/class/typec/port*/data_role'))
    if len(paths) != 1:
        return -errno.ENODEV
    path = Path(paths[0])
    try:
        if '[host]' not in read(path).split():
            write(path, 'host')
        return int('[host]' in read(path).split())
    except OSError as error:
        return -error.errno


def exchange(root, request):
    owner, kind, opcode = struct.unpack_from('<III', request)
    prop = struct.unpack_from('<I', request, 16 if len(request) == 24 else 12)[0]
    header = struct.pack('<IIII', owner, kind, opcode, prop)
    try:
        if len(request) == 52:
            write(root / 'verify_slave_flag', request[48])
            if opcode == 0x51:
                write(root / 'verify_digest', request[16:48].hex())
            else:
                return header + bytes.fromhex(read(root / 'verify_digest')[:64])
        elif len(request) == 32:
            if opcode == 0x51:
                words = struct.unpack_from('<4I', request, 16)
                write(root / 'request_vdm_cmd', f'{VDM_COMMANDS[prop]},' + ''.join(f'{word:08x}' for word in words))
            else:
                state, _, digest = read(root / 'request_vdm_cmd').partition(',')
                if int(state) != 5:
                    raise OSError(errno.EAGAIN, 'VDM authentication response is pending')
                return header + struct.pack('<4I', *(int(digest[i:i+8], 16) for i in range(0, 32, 8)))
        elif opcode == 0x51:
            value = struct.unpack_from('<I', request, 20)[0]
            if prop in VDM_COMMANDS:
                payload = struct.pack('<I', value).hex() if prop in (0x12, 0x13) else 'null'
                write(root / 'request_vdm_cmd', f'{VDM_COMMANDS[prop]},{payload}')
            else:
                write(root / PROPERTIES[prop], value)
        elif prop == 0x1a:
            return header + struct.pack('<II', int(read(root / 'request_vdm_cmd').split(',')[0]), 0)
        else:
            name = PROPERTIES.get(prop)
            if name is None:
                raise OSError(errno.EOPNOTSUPP, 'Property has no Linux sysfs reader')
            value = number(root / name, prop in (0x16, 0x19))
            return header + struct.pack('<II', value, 0)
    except (OSError, ValueError) as error:
        return header + struct.pack('<II', 0, getattr(error, 'errno', None) or errno.EINVAL)
    return header + bytes(8)


def authenticate(root):
    if os.geteuid() != 0:
        raise SystemExit('Run --authenticate as root to access the charger sysfs interface')
    with tempfile.TemporaryDirectory(prefix='sheng-charge-') as directory:
        directory = Path(directory)
        wrapper = directory / 'host.c'
        wrapper.write_text(WRAPPER)
        library = directory / 'auth.so'
        subprocess.run(['cc', '-std=c11', '-O2', '-fPIC', '-shared', '-I', str(PACKAGE),
                        str(wrapper), str(PACKAGE / 'ShengChargeAuth.c'), '-o', str(library)], check=True)
        lib = ctypes.CDLL(str(library))
        pointer = ctypes.c_void_p
        random_type = ctypes.CFUNCTYPE(None, pointer, ctypes.c_uint32)
        hmac_type = ctypes.CFUNCTYPE(None, pointer, ctypes.c_uint32, pointer, ctypes.c_uint32, pointer)

        @random_type
        def random_bytes(out, count):
            ctypes.memmove(out, secrets.token_bytes(count), count)

        @hmac_type
        def digest(key, key_bytes, message, message_bytes, out):
            value = hmac.new(ctypes.string_at(key, key_bytes), ctypes.string_at(message, message_bytes), hashlib.sha256).digest()
            ctypes.memmove(out, value, len(value))

        lib.auth_size.restype = ctypes.c_size_t
        lib.auth_open.argtypes = [pointer, random_type, hmac_type]
        lib.auth_state.argtypes = [pointer]
        lib.auth_host.argtypes = [pointer, ctypes.c_int]
        lib.auth_due.argtypes, lib.auth_due.restype = [pointer], ctypes.c_uint64
        lib.auth_status.argtypes = [pointer, pointer]
        lib.sheng_charge_auth_observe.argtypes = [pointer, ctypes.c_int, ctypes.c_int, ctypes.c_uint64]
        lib.sheng_charge_auth_next.argtypes = [pointer, ctypes.c_uint64, pointer]
        lib.sheng_charge_auth_receive.argtypes = [pointer, pointer, ctypes.c_uint32, ctypes.c_uint64]
        auth = ctypes.create_string_buffer(lib.auth_size())
        lib.auth_open(auth, random_bytes, digest)
        now_ms = lambda: time.monotonic_ns() // 1000000
        lib.sheng_charge_auth_observe(auth, 1, 8, now_ms())
        end = time.monotonic() + 90
        requests = 0
        while lib.auth_state(auth) not in (16, 17):
            if time.monotonic() >= end or requests >= 500:
                raise TimeoutError('Authentication exceeded its 90-second limit')
            if read('/sys/class/power_supply/usb/online') != '1':
                lib.sheng_charge_auth_observe(auth, 0, 0, now_ms())
                return {'state': 'source-disconnected'}
            if lib.auth_state(auth) == 6:
                lib.auth_host(auth, swap_data_role())
            due = lib.auth_due(auth)
            if due > now_ms():
                time.sleep(min((due - now_ms()) / 1000, 0.1))
                continue
            request = ctypes.create_string_buffer(52)
            count = lib.sheng_charge_auth_next(auth, now_ms(), request)
            if not count:
                continue
            reply = exchange(root, request.raw[:count])
            requests += 1
            lib.sheng_charge_auth_receive(auth, ctypes.c_char_p(reply), len(reply), now_ms())
        values = (ctypes.c_int32 * 9)()
        lib.auth_status(auth, values)
        return {'state': STATES[lib.auth_state(auth)], **dict(zip(
            ('result', 'last_result', 'gauges', 'adapter', 'reads', 'writes', 'errors', 'timeouts', 'retries'), values))}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sysfs', type=Path, default=Path('/sys/class/qcom-battery'))
    parser.add_argument('--authenticate', action='store_true', help='authenticate both gauges and a compatible Xiaomi PD charger')
    parser.add_argument('--out', type=Path, help='save telemetry and the authentication result as JSON')
    args = parser.parse_args()
    result = {'before': telemetry(args.sysfs)}
    if args.authenticate:
        source = result['before'].get('real_type')
        online = result['before'].get('usb', {}).get('online')
        if online != '1' or source not in ('PD', 'PD_DRP', 'PD_PPS'):
            result['authentication'] = {'state': 'source-not-pd'}
        else:
            result['authentication'] = authenticate(args.sysfs)
            result['after'] = telemetry(args.sysfs)
    output = json.dumps(result, indent=2) + '\n'
    if args.out:
        args.out.write_text(output)
    print(output, end='')


if __name__ == '__main__':
    main()
