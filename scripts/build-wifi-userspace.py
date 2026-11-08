#!/usr/bin/env python3
"""Stage the native supplicant and its launchd job for the existing Data addon."""
import argparse
import hashlib
import json
from pathlib import Path
import plistlib
import re
import shutil
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("output", type=Path)
parser.add_argument("target", choices=("ios27",))
args = parser.parse_args()
out = args.output.resolve()
out.mkdir(parents=True, exist_ok=True)
(out / "report.json").unlink(missing_ok=True)
component = root / "drivers/Ath12kWifi"
firmware_prefix = re.search(r'^#define SHENG_WIFI_FIRMWARE_PREFIX "([^"]+)"$',
    (root / "package/WifiFirmware.h").read_text(), re.M).group(1)
firmware_source = root / "firmware/wcn7850"
firmware_manifest = json.loads((firmware_source / "manifest.json").read_text())["files"]
if not {'amss.bin', 'm3.bin', 'board-2.bin'} <= firmware_manifest.keys():
    raise SystemExit("missing required WCN7850 firmware inputs")
shutil.rmtree(out / 'firmware', ignore_errors=True)
(out / 'firmware').mkdir()
for name, expected in firmware_manifest.items():
    if name not in ('amss.bin', 'amss2.bin', 'm3.bin', 'board-2.bin'):
        raise SystemExit("unsupported WCN7850 firmware name")
    data = (firmware_source / name).read_bytes()
    if len(data) != expected['bytes'] or hashlib.sha256(data).hexdigest() != expected['sha256']:
        raise SystemExit("WCN7850 firmware differs from manifest: " + name)
    (out / 'firmware' / name).write_bytes(data)
subprocess.run([sys.executable, component / "tools/build-supplicant.py", "--target", "ios"], check=True)
for name in ("bin", "etc/wifi", "daemons"):
    (out / name).mkdir(parents=True, exist_ok=True)
binary = out / "bin/unixnu-wpa_supplicant"
shutil.copy2(component / "out/wpa-ios/bin/wpa_supplicant", binary)
binary.chmod(0o755)
entitlements = out / "entitlements.plist"
entitlements.write_bytes(plistlib.dumps({"com.apple.private.security.no-container": True}))
subprocess.run(["codesign", "--force", "--sign", "-", "--pagesize", "4096", "--identifier",
    "org.unixnu.wifi.supplicant", "--entitlements", entitlements, binary], check=True)
subprocess.run(["codesign", "--verify", "--strict", binary], check=True)
detail = subprocess.run(["codesign", "--display", "--verbose=4", binary], check=True,
                        capture_output=True, text=True).stderr
match = re.search(r"^CDHash=([0-9a-f]{40})$", detail, re.M)
if not match: raise SystemExit("signed supplicant has no CDHash")
(out / "etc/wifi/wpa_supplicant.conf").write_text("# Networks arrive from the native Wi-Fi control channel.\nap_scan=1\n")
job = {"Label": "org.unixnu.wifi.supplicant", "ProgramArguments": [
    "/usr/libexec/unixnu-wpa_supplicant", "-Dsheng", "-ien0",
    "-c/private/var/ngpu/etc/wifi/wpa_supplicant.conf", "-q", "-s"],
    "RunAtLoad": True, "KeepAlive": True, "ThrottleInterval": 3, "UserName": "root",
    "Umask": 0o77, "ProcessType": "Interactive"}
# -s: warnings and errors go to syslog, so logd keeps them in its bounded store; no log file grows on Data.
(out / "daemons/org.unixnu.wifi.supplicant.plist").write_bytes(plistlib.dumps(job))
files = [path for name in ("bin", "etc/wifi", "daemons", "firmware") for path in sorted((out / name).rglob("*")) if path.is_file()]
(out / "report.json").write_text(json.dumps({"built": True, "target": args.target,
    "cdhash": match[1], "hardware_verified": False, "firmware_prefix": firmware_prefix,
    "files": {str(path.relative_to(out)): hashlib.sha256(path.read_bytes()).hexdigest() for path in files},
    "supplicant": json.loads((component / "out/wpa-ios/report.json").read_text())}, indent=2) + "\n")
print(f"staged signed native Wi-Fi service: {out}; CDHash {match[1]}")
