#!/usr/bin/env python3
"""Bounded offline QEMU UI compiler/cache run and actual mutable-Data export.

This never starts or contacts a physical device. Original images, signatures
and trust modules are preserved. The own software peer executes no shader.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/quartzcore-ios-compiler-peer'
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', type=Path, required=True, help='Fresh QEMU observation directory')
parser.add_argument('--stage-dir', type=Path, default=OUT, help='Own staged input directory; default is the producer fixture')
args = parser.parse_args()
OUT = args.stage_dir.resolve()
fixture = args.output.resolve()
if fixture.exists():
    raise SystemExit('Preserve the previous observation; choose a fresh directory')
if subprocess.run(['pgrep', '-f', 'qemu-system-aarch64'], capture_output=True).returncode == 0:
    raise SystemExit('Another QEMU is running')
if shutil.disk_usage(ROOT).free < 5 * 1024**3 + 512 * 1024**2:
    raise SystemExit('Preserve 5 GiB and the bounded offline allowance')


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


stage = json.loads((OUT / 'stage-report.json').read_text())
if sha(ROOT / stage.get('parent_source_relative', 'drivers/AdrenoMetal/bridge/quartzcore_cache_ios.cpp')) != stage['parent_source_sha256']:
    raise SystemExit('Exact compiled own parent source required')
if sha(ROOT / stage.get('stage_script_relative', 'scripts/stage-quartzcore-ios-compiler-peer.py')) != stage['stage_script_sha256']:
    raise SystemExit('Exact own staging implementation required')
for name, digest in stage.get('parent_source_dependencies', {}).items():
    if sha(ROOT / name) != digest:
        raise SystemExit('Exact shared parent source: ' + name)
for name, digest in stage['qualified_host_sources'].items():
    if sha(ROOT / name) != digest:
        raise SystemExit('Exact cross-linked compiler source: ' + name)
for name, info in stage['outputs'].items():
    if sha(OUT / name) != info['sha256']:
        raise SystemExit('Exact signed own executable input: ' + name)
if sha(OUT / 'ui-compiler-data-64.cdr') != stage['data_sha256'] or sha(OUT / 'offline-trust.bundle') != stage['trust_bundle_sha256']:
    raise SystemExit('Exact staged own Data and combined trust bundle required')
base = ROOT / 'build/xnu-prelink/native-owner-turnip-compiler-qemu-candidate'
plan = json.loads(base.with_suffix('.json').read_text())
if sha(base.with_suffix('.macho')) != plan['candidate_sha256'] or plan['original_sha256'] != 'f18b4df865405f2a287b84977d7aca745e056d656d9fc9df1662970ce2969de7':
    raise SystemExit('Exact preserved QEMU candidate and supplied original kernel required')
command = ['python3', ROOT / 'scripts/probe-darwinboot-xnu.py', '--kernel', base.with_suffix('.macho'),
    '--prelink-report', base.with_suffix('.json'), '--output', fixture,
    '--virtio-udif-image', ROOT / '.cache/firmware/target-volumes/044-76040-038.dmg',
    '--virtio-extra-block-image', ROOT / '.cache/firmware/target-volumes/044-76034-038.dmg',
    '--virtio-mmio-fdt', ROOT / 'build/virtio-block-fixture/qemu-virt.dtb',
    '--root-match-volume-uuid', '04769D26-2CA0-4825-B443-E2E5F88300F2', '--trust-cache', OUT / 'offline-trust.bundle',
    '--armvirt', '--armcpu', '--qemu-vmm-present', '--qemu-accel', 'hvf', '--owned-memory-size', '0x20000000',
    '--nvram-proxy', '--crypto-hash-method', 'sha2-384', '--amfi-platform-only', '--load-apfs', '--inspect-owner-root',
    '--inspect-dyld-flags', '--check-commpage', '--skip-initializer-traces', '--timeout', '90',
    '--data-ramdisk', OUT / 'ui-compiler-data-64.cdr', '--capture-scratch-data', '--qemu-ramfb', '--inspect-boot-video',
    '--native-air-init', '--break-symbol', '_exception_triage', '--entry-process-name', 'air-native',
    '--entry-register-equals', '0:6', '--inspect-current-process', '--inspect-entry-user-state',
    '--read-saved-user-register', 'd:180', '--read-saved-user-register', 'e:1000', '--read-saved-user-register', 'f:1000']
(OUT / (fixture.name + '.command.json')).write_text(json.dumps([str(x) for x in command], indent=2) + '\n')
with (OUT / (fixture.name + '.probe.log')).open('w') as log:
    run = subprocess.run([str(x) for x in command], cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, timeout=240)
(OUT / (fixture.name + '.attempt.json')).write_text(json.dumps(dict(exit_code=run.returncode,
    fixture=str(fixture.relative_to(ROOT)), candidate_sha256=plan['candidate_sha256'], stage_report_sha256=sha(OUT / 'stage-report.json'),
    data_sha256=stage['data_sha256'], original_os_modified=False, physical_device_tested=False,
    compiler_cache_qualified=False, complete_deployment_pack=False), indent=2) + '\n')
print('Bounded QEMU UI compiler/export probe exit', run.returncode, '; inspect actual state before qualification')
raise SystemExit(run.returncode)
