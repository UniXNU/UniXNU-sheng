#!/usr/bin/env python3
"""Validate the production launchd-cache merge with the selected original OS."""
import argparse
import ctypes
import hashlib
import importlib.util
import json
from pathlib import Path
import plistlib
import subprocess
import tempfile
import tomllib

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--os-root', type=Path, required=True)
args = parser.parse_args()
builder = root.parent / 'UniXNU/scripts/build-boot-runtime.py'
spec = importlib.util.spec_from_file_location('boot_runtime', builder)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
source = args.os_root / 'System/Library/xpc/launchd.plist'
original = plistlib.loads(source.read_bytes())
assert original['VersionNumber'] == 7
key = '/System/Library/LaunchDaemons/org.unixnu.wifi.supplicant.plist'
out = root / 'build/wifi-launchd'
out.mkdir(exist_ok=True)
(out / 'report.json').unlink(missing_ok=True)
with tempfile.TemporaryDirectory() as td:
    work = Path(td)
    job = {'Label': 'org.unixnu.wifi.supplicant', 'ProgramArguments': [
        '/usr/libexec/unixnu-wpa_supplicant', '-Dsheng', '-ien0'],
        'RunAtLoad': True, 'KeepAlive': True}
    daemons = work / 'daemons'
    daemons.mkdir()
    plist = daemons / Path(key).name
    plist.write_bytes(plistlib.dumps(job))
    merged = work / 'merged.plist'
    report = module.merge_launchd_cache(source, daemons, merged)
    data = plistlib.loads(merged.read_bytes())
    assert data['LaunchDaemons'].pop(key) == job and data == original
    # Exercise the host OS consumer too, rather than checking only Python's
    # reader. Target iPadOS private-parser acceptance is recorded on device.
    xpc = ctypes.CDLL('/usr/lib/system/libxpc.dylib')
    xpc.xpc_create_from_plist.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
    xpc.xpc_create_from_plist.restype = ctypes.c_void_p
    xpc.xpc_dictionary_get_value.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
    xpc.xpc_dictionary_get_value.restype = ctypes.c_void_p
    xpc.xpc_dictionary_get_count.argtypes = [ctypes.c_void_p]
    xpc.xpc_dictionary_get_count.restype = ctypes.c_size_t
    xpc.xpc_array_get_string.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
    xpc.xpc_array_get_string.restype = ctypes.c_char_p
    xpc.xpc_release.argtypes = [ctypes.c_void_p]
    raw = merged.read_bytes(); buffer = ctypes.create_string_buffer(raw)
    parsed = xpc.xpc_create_from_plist(buffer, len(raw))
    assert parsed
    daemon_dict = xpc.xpc_dictionary_get_value(parsed, b'LaunchDaemons')
    assert xpc.xpc_dictionary_get_count(daemon_dict) == len(original['LaunchDaemons']) + 1
    owner = xpc.xpc_dictionary_get_value(daemon_dict, key.encode())
    arguments = xpc.xpc_dictionary_get_value(owner, b'ProgramArguments')
    assert xpc.xpc_array_get_string(arguments, 0) == job['ProgramArguments'][0].encode()
    xpc.xpc_release(parsed)
    # Deterministic rebuild from the original cache; never merge a previous
    # overlay in place or silently replace a system-owned job.
    first = merged.read_bytes()
    signature_report = module.sign_launchd_cache(merged)
    signature = merged.with_suffix('.plist.sig')
    assert module.verified_cdhash(merged, signature) == signature_report['cdhash']
    changed = bytearray(first); changed[100] ^= 1; merged.write_bytes(changed)
    rejected = subprocess.run(['codesign', '--verify', '--strict', '--detached',
        str(signature), str(merged)], capture_output=True)
    assert rejected.returncode != 0
    merged.write_bytes(first)
    module.merge_launchd_cache(source, daemons, merged)
    assert first == merged.read_bytes()
    module.sign_launchd_cache(merged)
    assert module.verified_cdhash(merged, signature) == signature_report['cdhash']
    try:
        module.merge_launchd_cache(merged, daemons, work / 'bad.plist')
    except ValueError:
        pass
    else:
        raise AssertionError('existing job accepted')
    # Verify the board's iPadOS 27 per-job settings against the actual OS cache.
    with (root / 'board.toml').open('rb') as stream:
        overrides = tomllib.load(stream)['payload']['runtime']['launchd_job_environment']
    icon_label = 'com.apple.iconservices.iconservicesagent'
    assert overrides == {icon_label: {'FORCE_SOFTWARE_RENDERER': '1', 'CI_ENABLE_METAL_GPU': '0'}}
    profile_cache = work / 'icon.plist'
    module.merge_launchd_cache(source, daemons, profile_cache, overrides)
    profile = plistlib.loads(profile_cache.read_bytes())
    assert profile['LaunchDaemons'].pop(key) == job
    icon_key = '/System/Library/LaunchDaemons/' + icon_label + '.plist'
    for name, value in overrides[icon_label].items():
        assert profile['LaunchDaemons'][icon_key]['EnvironmentVariables'].pop(name) == value
    assert profile == original
    try:
        module.merge_launchd_cache(source, daemons, work / 'bad.plist', {'missing.job': {'MODE': '1'}})
    except ValueError:
        pass
    else:
        raise AssertionError('missing OS job accepted')
    job['Label'] = 'com.apple.wifid'
    plist.write_bytes(plistlib.dumps(job))
    try:
        module.merge_launchd_cache(source, daemons, work / 'bad.plist')
    except ValueError:
        pass
    else:
        raise AssertionError('system job label accepted')
report.update(passed=True, hardware_service_started=False,
    builder_sha256=hashlib.sha256(builder.read_bytes()).hexdigest(),
    detached_signature=signature_report, tampered_page_rejected=True, host_libxpc_parsed=True)
(out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
print(f"launchd cache: {report['original_jobs']} original jobs preserved, one owner job added; detached signature validates, tampered page rejected, deterministic rebuild and collision rejection pass")
