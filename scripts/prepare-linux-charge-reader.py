#!/usr/bin/env python3
"""Prepare a sheng 7.3.4 reader with the static charge tool, ADSP firmware and ordered modules."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
VERSION = '7.3.4'
KERNEL = '7.3.4-sm8550-00104-g87ac0f2347eb'
RELEASE = 'https://github.com/ianchb/sm8550-mainline/releases/download/' + VERSION + '/'
FIRMWARE = 'https://raw.githubusercontent.com/ianchb/sheng-firmware/2c1e2729a085c7f0470c855d236e49455c4601f0/'
FIRMWARE_FILES = {'qcom/sm8550/sheng/adsp.mbn': 38275512, 'qcom/sm8550/sheng/adsp_dtb.mbn': 16184}
MODULE_ROOTS = ('qrtr-smd.ko.zst', 'qcom_pd_mapper.ko.zst', 'qcom_glink_smem.ko.zst',
                'qcom_battmgr.ko.zst', 'qcom_q6v5_pas.ko.zst')
SETUP = '''#!/bin/busybox sh
set -eu
while read file name; do
    [ -d "/sys/module/$name" ] || insmod "/lib/charge-modules/$file"
done < /lib/charge-modules/modules.load
exec /bin/sheng-charge --wait 30 "$@"
'''


def download(url, path, size=None):
    if path.is_file() and (size is None or path.stat().st_size == size):
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + '.part')
    try:
        with urllib.request.urlopen(url, timeout=120) as source, temporary.open('wb') as output:
            shutil.copyfileobj(source, output, 1 << 20)
        if size is not None and temporary.stat().st_size != size:
            raise SystemExit(f'{path.name}: unexpected download size')
        temporary.replace(path)
    finally:
        temporary.unlink(missing_ok=True)


def member(package, names, destination):
    process = subprocess.Popen(['ar', '-p', str(package), 'data.tar.xz'], stdout=subprocess.PIPE)
    found = set()
    with tarfile.open(fileobj=process.stdout, mode='r|xz') as archive:
        for entry in archive:
            name = entry.name.removeprefix('./')
            if name not in names:
                continue
            output = destination / Path(name).name
            with archive.extractfile(entry) as source, output.open('wb') as target:
                shutil.copyfileobj(source, target)
            found.add(name)
    if process.wait() or found != set(names):
        raise SystemExit('Linux package is missing the selected module inputs')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--unixnu', type=Path, default=ROOT.parent / 'UniXNU')
    parser.add_argument('--cache', type=Path, default=ROOT / '.cache/linux-charge-reader')
    parser.add_argument('--busybox', required=True, type=Path, help='static aarch64 BusyBox')
    parser.add_argument('--out', type=Path, default=ROOT / 'build/linux-charge/sheng-linux-charge.img')
    args = parser.parse_args()
    cache = args.cache.resolve()
    cache.mkdir(parents=True, exist_ok=True)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    package = cache / 'linux-xiaomi-sheng.deb'
    boot = cache / 'boot_sheng_singleboot.img'
    download(RELEASE + boot.name, boot, 16904192)
    prefix = 'lib/modules/' + KERNEL
    if not (cache / 'modules.dep').is_file():
        download(RELEASE + package.name, package, 274477340)
        member(package, [prefix + '/modules.dep'], cache)
    dependencies = {}
    for line in (cache / 'modules.dep').read_text().splitlines():
        name, _, dep = line.partition(':')
        dependencies[name] = dep.split()
    by_name = {Path(name).name: name for name in dependencies}
    ordered = []

    def add(name):
        if name in ordered:
            return
        for dependency in dependencies[name]:
            add(dependency)
        ordered.append(name)

    for name in MODULE_ROOTS:
        if name not in by_name:
            parser.error(f'{name} is absent from the kernel package')
        add(by_name[name])
    reboot_mode = by_name.get('nvmem-reboot-mode.ko.zst')
    if not reboot_mode:
        parser.error('the kernel package needs nvmem-reboot-mode')
    selected = [prefix + '/' + name for name in [reboot_mode, *ordered]]
    modules = cache / 'modules'
    modules.mkdir(exist_ok=True)
    if any(not (modules / Path(name).name.removesuffix('.zst')).is_file() for name in selected):
        download(RELEASE + package.name, package, 274477340)
        member(package, selected, modules)
    for packed in modules.glob('*.ko.zst'):
        subprocess.run(['zstd', '-q', '-d', '-f', str(packed)], check=True, stdout=subprocess.DEVNULL)
        packed.unlink()
    package.unlink(missing_ok=True)
    files = []
    for name, size in FIRMWARE_FILES.items():
        target = cache / 'firmware' / name
        download(FIRMWARE + name, target, size)
        files += ['--file', f'lib/firmware/{name}={target}']
    tool = cache / 'sheng-charge'
    subprocess.run([sys.executable, str(ROOT / 'scripts/build-linux-charge.py'), '--unixnu', str(args.unixnu), '--out', str(tool)], check=True)
    reboot = cache / 'rebootbl'
    clang = '/opt/homebrew/opt/llvm/bin/clang'
    if not Path(clang).is_file():
        clang = shutil.which('clang')
    lld = shutil.which('ld.lld') or '/opt/homebrew/opt/lld/bin/ld.lld'
    subprocess.run([clang, '--target=aarch64-linux-gnu', '-nostdlib', '-static', '-fno-exceptions', '-fno-rtti',
                    '-fno-stack-protector', '-fuse-ld=' + lld, '-Wl,-e,start', '-O2',
                    str(ROOT / 'package/linux-log/rebootbl.cpp'), '-o', str(reboot)], check=True)
    setup = cache / 'sheng-charge-setup'
    setup.write_text(SETUP)
    setup.chmod(0o755)
    load = cache / 'modules.load'
    load.write_text(''.join(Path(name).name.removesuffix('.zst') + ' ' + Path(name).name.removesuffix('.ko.zst').replace('-', '_') + '\n'
                            for name in ordered))
    files += ['--file', f'bin/sheng-charge={tool}', '--file', f'bin/sheng-charge-setup={setup}',
              '--file', f'lib/charge-modules/modules.load={load}']
    for name in ordered:
        path = modules / Path(name).name.removesuffix('.zst')
        files += ['--file', f'lib/charge-modules/{path.name}={path}']
    subprocess.run([sys.executable, str(ROOT / 'package/linux-log/build-linux-log-boot.py'), '--boot', str(boot),
                    '--busybox', str(args.busybox), '--rebootbl', str(reboot), '--module',
                    str(modules / 'nvmem-reboot-mode.ko'), '--out', str(args.out), *files], check=True)
    report = {'kernel': KERNEL, 'modules': ordered, 'charge_command': '/bin/sheng-charge-setup --authenticate --wait-pd 120 --out /run/charge.log',
              'firmware_source': FIRMWARE, 'firmware': {name: hashlib.sha256((cache / 'firmware' / name).read_bytes()).hexdigest()
                                                       for name in FIRMWARE_FILES},
              'image': str(args.out.resolve()), 'hardware_tested': False}
    args.out.with_suffix('.json').write_text(json.dumps(report, indent=2) + '\n')
    print(args.out)


if __name__ == '__main__':
    main()
