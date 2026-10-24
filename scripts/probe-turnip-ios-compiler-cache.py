#!/usr/bin/env python3
"""Bounded QEMU-only iOS Turnip compilation/cache observation; never hardware."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/turnip-ios-compiler-peer'
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', type=Path, default=ROOT / 'build/darwinboot-owner-hvf-turnip-compiler-cache')
args = parser.parse_args()
fixture = args.output.resolve()
if fixture.exists():
    raise SystemExit('Preserve the existing QEMU observation; select a fresh output directory')
if subprocess.run(['pgrep', '-f', 'qemu-system-aarch64'], capture_output=True).returncode == 0:
    raise SystemExit('Another QEMU is running')
if shutil.disk_usage(ROOT).free < 5 * 1024**3 + 256 * 1024**2:
    raise SystemExit('Preserve 5 GiB and the bounded QEMU fixture allowance')


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


build = json.loads((OUT / 'build-report.json').read_text())
data = json.loads((OUT / 'data-report.json').read_text())
for name, digest in build['sources'].items():
    if sha(ROOT / name) != digest:
        raise SystemExit('Exact compiled source required: ' + name)
if sha(OUT / 'build-report.json') != data['cross_build_report_sha256'] or sha(OUT / 'compiler-data-32.cdr') != data['sha256']:
    raise SystemExit('Exact staged own compiler fixture required')
if sha(OUT / 'offline-trust.bundle') != data['trust_bundle_sha256']:
    raise SystemExit('Exact combined own/original trust bundle required')
base = ROOT / 'build/xnu-prelink/native-owner-turnip-compiler-qemu-candidate'
plan = json.loads(base.with_suffix('.json').read_text())
if sha(base.with_suffix('.macho')) != plan['candidate_sha256'] or plan['original_sha256'] != 'f18b4df865405f2a287b84977d7aca745e056d656d9fc9df1662970ce2969de7':
    raise SystemExit('Exact supplied ordinary ARM64 source and QEMU candidate required')
command = ['python3', ROOT / 'scripts/probe-darwinboot-xnu.py', '--kernel', base.with_suffix('.macho'),
    '--prelink-report', base.with_suffix('.json'), '--output', fixture,
    '--virtio-udif-image', ROOT / '.cache/firmware/target-volumes/044-76040-038.dmg',
    '--virtio-extra-block-image', ROOT / '.cache/firmware/target-volumes/044-76034-038.dmg',
    '--virtio-mmio-fdt', ROOT / 'build/virtio-block-fixture/qemu-virt.dtb',
    '--root-match-volume-uuid', '04769D26-2CA0-4825-B443-E2E5F88300F2',
    '--trust-cache', OUT / 'offline-trust.bundle', '--armvirt', '--armcpu', '--qemu-vmm-present', '--qemu-accel', 'hvf',
    '--owned-memory-size', '0x20000000', '--nvram-proxy', '--crypto-hash-method', 'sha2-384',
    '--amfi-platform-only', '--load-apfs', '--inspect-owner-root', '--inspect-dyld-flags', '--check-commpage',
    '--skip-initializer-traces', '--timeout', '60', '--data-ramdisk', OUT / 'compiler-data-32.cdr',
    '--qemu-ramfb', '--inspect-boot-video', '--native-air-init', '--break-symbol', '_exception_triage',
    '--entry-process-name', 'air-native', '--entry-register-equals', '0:6', '--inspect-current-process',
    '--inspect-entry-user-state', '--read-saved-user-register', 'd:180', '--read-saved-user-register', 'e:1000',
    '--read-saved-user-register', 'f:1000']
(OUT / 'qemu-command.json').write_text(json.dumps([str(x) for x in command], indent=2) + '\n')
with (OUT / (fixture.name + '.probe.log')).open('w') as log:
    run = subprocess.run([str(x) for x in command], cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, timeout=180)
(OUT / (fixture.name + '.attempt.json')).write_text(json.dumps(dict(exit_code=run.returncode, fixture=str(fixture.relative_to(ROOT)),
    candidate_sha256=plan['candidate_sha256'], data_sha256=data['sha256'],
    original_os_modified=False, physical_device_tested=False, cache_import_qualified=False), indent=2) + '\n')
print('Bounded QEMU probe exit', run.returncode, '; inspect actual saved state; no physical device operation')
raise SystemExit(run.returncode)
