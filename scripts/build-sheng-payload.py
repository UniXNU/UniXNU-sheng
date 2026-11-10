#!/usr/bin/env python3
"""Build the sheng first-boot userdata payload and validate its boot chain.

The payload is a single FAT32 volume (the exact ESP layout every offline
UniXNU -> DarwinBoot -> XNU run qualified: UniXNU.efi, DarwinBoot.efi,
the unmodified source 4 KiB kernel with its sheng hardware ADT sidecar, and
the auto-boot unixnu.toml). Delivery model per the user's instruction:
the user flashes this image to the device's userdata partition with fastboot
at test time; nothing is written to the device from this host. On hardware
the stock chain UFSDxe -> PartitionDxe (GPT name "userdata") -> FatDxe
mounts it and UniXNU enumerates it as a normal volume - no custom
firmware driver is needed and the FD stays as committed.

Kernel-in-boot.img is physically impossible: the stock ABL kernel window is
~78 MiB before its own ~31 MiB vendor ramdisk + ~2 MiB DTB staging, while
the kernel alone is 92 MiB (73 MiB LZMA). The v4 boot image carries only
BootShim + UEFI FD.

Validation (all offline):
 1. A GPT test disk is synthesized with an EFI System Partition holding only
    UniXNU.efi (mirroring the FD-embedded UniXNU of the hardware boot)
    and a partition NAMED "userdata" holding the payload FAT image. QEMU's
    edk2 firmware boots UniXNU from the ESP; UniXNU must discover the
    config on the userdata-named partition, auto-boot the XNU entry and
    DarwinBoot must reach "exiting boot services" - the same boundary every
    standing green probe observes before kernel entry.
 2. Kernel execution with these exact kernel bytes, arguments and DarwinBoot
    build is separately covered by the standing owner-hvf probes.

Evidence: build/sheng/boot/payload-report.json.
"""
import hashlib
import json
import os
import shutil
import struct
import subprocess
import re
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
UNIXNU_ROOT = Path(os.environ.get('UNIXNU_ROOT', ROOT.parent / 'UniXNU'))
OUT = ROOT / 'build/sheng/boot'
STAGE = OUT / 'payload-fat'
PAYLOADS = STAGE / 'EFI/UniXNU/Payloads'
KERNEL = ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837.unstripped.noctf'
DARWINBOOT = ROOT / 'build/DarwinBoot/DEBUG_CLANGPDB/AARCH64/DarwinBoot.efi'
UNIXNU = UNIXNU_ROOT / 'package/Build/Out/AARCH64-DEBUG/UniXNU.efi'
# SMEM item 402 observed on hardware: the low bank ends at 0x100000000.
# This is board metadata, not an allocation grant; DarwinBoot owns its window
# through UEFI AllocatePages. See docs/native-ufs.md for reserved/high ranges.
DRAM_SIZE = 0x80000000
KERNEL_ARGS = '-v serial=3 debug=0x14e kt_fixed_zones=120'
# --kernel-arg=NAME[=VALUE] appends one boot argument (repeatable), e.g. native-ufs-probe=2.
for _arg in sys.argv[1:]:
    if _arg.startswith('--kernel-arg='):
        _value = _arg.split('=', 1)[1]
        if not re.fullmatch(r'[A-Za-z0-9_.\-]+(=[A-Za-z0-9_.,\-]+)?', _value):
            raise SystemExit('Unsupported kernel argument')
        KERNEL_ARGS += ' ' + _value
FAT_IMAGE = OUT / 'sheng-userdata-payload.img'   # what the user flashes
TEST_DISK = OUT / 'sheng-payload-testdisk.img'   # GPT fidelity disk for QEMU
FAT_SIZE = os.environ.get('SHENG_FAT_SIZE', '192m')   # SHENG_FAT_SIZE=176m for an image that has to fit the boot partition (scripts/build-flash-image.sh)   # int-151: ABL accepted a 257 MiB boot image but stayed in fastboot; 193 MiB images boot
GPT_TEST_DISK_SIZE = 512 * 1024 * 1024           # sparse-friendly container


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def check_native_root_boot_args(kernel, arguments):
    """Refuse the known bare-metal AMFI panic before replacing payload files."""
    values = {}
    for argument in arguments.split():
        key, separator, value = argument.partition('=')
        if separator:
            values.setdefault(key, []).append(value)
    if '1' not in values.get('native-ufs-root', []):
        return
    if values.get('pmgr') != ['2']:
        raise SystemExit('Native UFS root boot requires exactly one --kernel-arg=pmgr=2; '
                         'the unchanged target AMFI otherwise panics with No PMGR')
    symbols = subprocess.check_output(['xcrun', 'nm', '-U', str(kernel)], text=True)
    if not any(line.split()[-1:] == ['_sheng_pmgr_run'] for line in symbols.splitlines()):
        raise SystemExit('Native UFS root candidate lacks the linked sheng PMGR integration')


def make_fat_image(stage_dir, image):
    import plistlib
    if image.exists():
        image.unlink()
    created = image.with_name(image.name + '.dmg')
    if created.exists():
        created.unlink()
    subprocess.run(['hdiutil', 'create', '-size', FAT_SIZE, '-layout', 'NONE',
                    '-fs', 'MS-DOS FAT32', '-volname', 'SHENGESP', '-ov', str(image)],
                   check=True, capture_output=True)
    if created.exists():
        created.rename(image)
    if not image.exists():
        raise SystemExit(f'hdiutil did not produce {image}')
    mount = OUT / (image.stem + '-mount')
    mount.mkdir(exist_ok=True)
    try:
        attached = plistlib.loads(subprocess.check_output(
            ['hdiutil', 'attach', str(image), '-nobrowse', '-owners', 'off',
             '-mountpoint', str(mount), '-plist'], stderr=subprocess.STDOUT))
    except subprocess.CalledProcessError:
        # macOS sometimes refuses to mount a fresh FAT32 image ("no mountable file systems", msdosfs not loadable); the
        # image is valid, so fill it without mounting, using mtools.
        mount.rmdir()
        for entry in sorted(stage_dir.iterdir()):
            subprocess.run(['mcopy', '-i', str(image), '-s', '-Q', '-m', str(entry), '::/'], check=True)
        return
    try:
        for item in stage_dir.rglob('*'):
            target = mount / item.relative_to(stage_dir)
            if item.is_dir():
                target.mkdir(exist_ok=True)
            else:
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(item, target)
    finally:
        subprocess.run(['hdiutil', 'detach', attached['system-entities'][0]['dev-entry']],
                       check=True, capture_output=True)
    mount.rmdir()








def build_gpt_test_disk(esp_dir, payload_image):
    """GPT disk via sgdisk: p1 = ESP with UniXNU only, p2 named userdata."""
    import re
    block = 512
    sgdisk = '/opt/homebrew/opt/gptfdisk/bin/sgdisk'
    if TEST_DISK.exists():
        TEST_DISK.unlink()
    with TEST_DISK.open('wb') as f:
        f.truncate(GPT_TEST_DISK_SIZE)
    subprocess.run([sgdisk, TEST_DISK, '-o',
                    '-n', '1:2048:+16M', '-t', '1:EF00', '-c', '1:esp-test',
                    '-n', '2:0:+160M', '-t', '2:8300', '-c', '2:userdata'],
                   check=True, capture_output=True)
    info = subprocess.check_output([sgdisk, TEST_DISK, '-p'], text=True)
    rows = {}
    for line in info.splitlines():
        m = re.match(r'\s*(\d+)\s+(\d+)\s+(\d+)\s+[\d.]+\s*[KMGTP]?i?B\s+([0-9A-Fa-f]{4})\s+(.*)$', line)
        if m:
            rows[int(m.group(1))] = (int(m.group(2)), int(m.group(3)), m.group(5))
    if set(rows) != {1, 2} or rows[2][2].split()[0] != 'userdata':
        raise SystemExit(f'sgdisk layout unexpected: {rows}')
    esp_first, ud_first = rows[1][0], rows[2][0]
    if (payload_image.stat().st_size + block - 1) // block > rows[2][1] - ud_first + 1:
        raise SystemExit('payload FAT larger than test userdata partition')
    esp_fat = make_plain_fat(esp_dir)
    with TEST_DISK.open('r+b') as f:
        f.seek(esp_first * block)
        f.write(esp_fat)
        f.seek(ud_first * block)
        f.write(payload_image.read_bytes())
    return esp_first, ud_first


_ESP_STAGE = OUT / 'esp-boot-only'


def make_plain_fat(dir_tree):
    image = OUT / 'tmp-esp-only.img'
    make_fat_image(dir_tree, image)
    data = image.read_bytes()
    image.unlink()
    return data


def main():
    firmware_loader = '--firmware-loader' in sys.argv[1:]
    # --xnu-probe patches boot-progress probes into the payload's COPY of the
    # kernel (see patch-xnu-entry-probe.py); the original kernel is untouched.
    xnu_probe = any(a == '--xnu-probe' or a == '--xnu-panic-screen' or a.startswith('--xnu-reset-after=') for a in sys.argv[1:])
    # --xnu-reset-after=[FUNCTION/]SYMBOL[:N] moves the probe's reset to after the
    # Nth call to SYMBOL in FUNCTION (default _arm_init); without it the reset is
    # right after the MMU is on.
    reset_after = next((a.split('=', 1)[1] for a in sys.argv[1:] if a.startswith('--xnu-reset-after=')), None)
    reset_before = '--xnu-reset-before' in sys.argv[1:]
    # With --firmware-loader the XNU loader (DarwinBoot) is part of the firmware
    # volume, because the firmware refuses a driver loaded from a volume. The
    # volume then holds only the config, the kernel and its ADT, and the QEMU
    # chain check (which loads the driver from the volume) is skipped.
    # --kernel=PATH selects another kernel Mach-O, e.g. a native-prelink candidate that
    # carries the original components the native kernel links against.
    kernel = next((Path(a.split('=', 1)[1]) for a in sys.argv[1:] if a.startswith('--kernel=')), KERNEL)
    # --data-ramdisk=PATH: explicit mutable scratch APFS Data seed, loaded as the UniXNU initrd.
    data_ramdisk = next((Path(a.split('=', 1)[1]) for a in sys.argv[1:] if a.startswith('--data-ramdisk=')), None)
    trust_cache = next((Path(a.split('=', 1)[1]) for a in sys.argv[1:]
                        if a.startswith('--original-trust-cache=')), None)
    if trust_cache and digest(trust_cache) != '7416ca4f8377146626bb284e2133c76bee6cb4e3ee0af5d648b186235a7323a7':
        raise SystemExit('Require the exact two unchanged target static/System trust modules')
    # --owner-trust-cache=PATH replaces the bundle with one that carries the two original modules byte for byte plus an extra
    # leading owner module (make-owner-trust-bundle.py); the original modules are still pinned by hash.
    owner_trust = next((Path(a.split('=', 1)[1]) for a in sys.argv[1:] if a.startswith('--owner-trust-cache=')), None)
    if owner_trust:
        if trust_cache:
            raise SystemExit('Give either the original trust cache or the owner trust cache')
        import struct
        original = (ROOT / 'build/original-static-system-trust/original-static-system.bundle').read_bytes()
        if digest(ROOT / 'build/original-static-system-trust/original-static-system.bundle') != '7416ca4f8377146626bb284e2133c76bee6cb4e3ee0af5d648b186235a7323a7':
            raise SystemExit('The original static/System trust inputs differ')
        raw = owner_trust.read_bytes()
        count = struct.unpack_from('<I', raw)[0]
        starts = struct.unpack_from('<%dI' % count, raw, 4)
        modules = [raw[a:b] for a, b in zip(starts, (*starts[1:], len(raw)))]
        o_count = struct.unpack_from('<I', original)[0]
        o_starts = struct.unpack_from('<%dI' % o_count, original, 4)
        o_modules = [original[a:b] for a, b in zip(o_starts, (*o_starts[1:], len(original)))]
        if count != 1 + o_count or modules[1:] != o_modules or struct.unpack_from('<I', modules[0])[0] != 2:
            raise SystemExit('The owner trust cache must be exactly one version-2 owner module followed by both unchanged original modules')
        trust_cache = owner_trust
    if data_ramdisk and not data_ramdisk.exists():
        raise SystemExit(f'Missing Data seed: {data_ramdisk}')
    required = (kernel,) if firmware_loader else (kernel, DARWINBOOT, UNIXNU)
    for path in required:
        if not path.exists():
            raise SystemExit(f'Missing qualified artifact: {path}')
    # DarwinBoot copies the command line into a 1024-byte buffer and refuses a longer one with EFI_BAD_BUFFER_SIZE, after the image was already sent.
    if len(KERNEL_ARGS) > 1016:
        raise SystemExit('Kernel command line is %d bytes; the boot loader accepts at most 1023. Drop arguments that equal their default.' % len(KERNEL_ARGS))
    check_native_root_boot_args(kernel, KERNEL_ARGS)
    # The handoff's rootauth3 candidate predates these source-level probes.
    # Refuse that mismatch before replacing any staging files or payload image.
    for mode, banner in ((13, b'NATIVE_UFS memory snapshot after'),
                         (14, b'NATIVE_UFS early memory snapshot')):
        if f'--kernel-arg=native-ufs-probe={mode}' in sys.argv[1:] and banner not in kernel.read_bytes():
            raise SystemExit(f'Candidate lacks native-ufs-probe={mode}; rebuild the kernel or use panic-screen memory counters')
    if STAGE.exists():
        shutil.rmtree(STAGE)
    PAYLOADS.mkdir(parents=True)
    if not firmware_loader:
        shutil.copyfile(DARWINBOOT, PAYLOADS / 'DarwinBoot.efi')
    shutil.copyfile(kernel, PAYLOADS / 'kernel.macho')
    if xnu_probe:
        probe_args = []
        if '--xnu-panic-screen' in sys.argv[1:]:
            # panic() draws its message on the screen and stops; no reset probe
            probe_args = ['--panic-screen', '--no-reset']
        elif reset_after:
            # [FUNCTION/]SYMBOL[:N]
            where, _, rest = reset_after.rpartition('/')
            symbol, _, occurrence = rest.partition(':')
            probe_args = ['--reset-after', symbol, '--occurrence', occurrence or '1']
            if reset_before: probe_args.append('--reset-before')
            if where:
                probe_args += ['--in', where]
        subprocess.run(['python3', ROOT / 'scripts/patch-xnu-entry-probe.py', PAYLOADS / 'kernel.macho', *probe_args], check=True)
    if trust_cache:
        shutil.copyfile(trust_cache, PAYLOADS / 'kernel.macho.trustcache')
    if data_ramdisk:
        shutil.copyfile(data_ramdisk, PAYLOADS / 'scratch-data.img')
    subprocess.run(['python3', ROOT / 'scripts/make-sheng-adt.py',
                    '--output', PAYLOADS / 'kernel.macho.adt',
                    '--dram-size', hex(DRAM_SIZE),
                    *[v for a in sys.argv[1:] if a.startswith(('--unique-chip-id=', '--chip-id=')) for v in (a.split('=', 1)[0], a.split('=', 1)[1])],
                    *[v for a in sys.argv[1:] if a.startswith('--root-match-volume-uuid=') for v in ('--root-match-volume-uuid', a.split('=', 1)[1])],
                    *(['--packed-data-ramdisk' if '--packed-data-ramdisk' in sys.argv else '--data-ramdisk'] if data_ramdisk else []),
                    *(['--trust-cache'] if trust_cache else []),
                    *(['--ipad-identity'] if '--ipad-identity' in sys.argv else []),
                    *[v for a in sys.argv[1:] if a.startswith(('--cpu-regs=', '--psci-method=')) for v in (a.split('=', 1)[0], a.split('=', 1)[1])],
                    *[v for a in sys.argv[1:] if a.startswith('--owned-memory-size=') for v in ('--owned-memory-size', a.split('=', 1)[1])]], check=True)
    # The menu title names the build: the session tag (build-tag) when given, else the first digits of the kernel hash.
    build_tag = re.search(r'(?:^| )build-tag=([A-Za-z0-9._-]+)', KERNEL_ARGS)
    ENTRY_TITLE = (build_tag.group(1) if build_tag else digest(kernel)[:8])
    INITRD_LINE = 'initrd = "Payloads/scratch-data.img"\n' if data_ramdisk else ''
    # Two entries: "Boot" keeps userdata as it is; "Install and boot" asks first (confirm, before anything is touched), then erases the
    # Data store (data-reset: the seed starts over) and boots, skipping the Hello page (1) or showing it (2). The command line
    # limit is 1016 bytes, so each choice is a single argument.
    # The install boot does not need the unattended return to fastboot, so its line leaves out autoreset (room for the reset argument).
    INSTALL_ARGS = re.sub(r' autoreset=[0-9]+', '', KERNEL_ARGS)
    for value in ('1', '2'):
        if len((INSTALL_ARGS + ' data-reset=' + value).encode()) > 1016:
            raise SystemExit(f'the install entry command line is {len((INSTALL_ARGS + " data-reset=" + value).encode())} bytes, the limit is 1016')
    INSTALL_ENTRY = f'''
[[entry]]
id = "install"
title = "Install and boot ({ENTRY_TITLE})"
confirm = "ERASE all iPadOS user data and install again?"
confirm_titles = ["Erase Data, skip Hello, boot", "Erase Data, show Hello, boot"]
confirm_args = ["data-reset=1", "data-reset=2"]
type = "xnu"
path = "Payloads/kernel.macho"
{INITRD_LINE}args = "{INSTALL_ARGS}"
'''
    (STAGE / 'EFI/UniXNU/unixnu.toml').write_text(f'''version = 1
[boot]
timeout = 3
default = "xnu"
[kits]
autoload = false
{'' if firmware_loader else '[[driver]]' + chr(10) + 'path = "Payloads/DarwinBoot.efi"'}
[[entry]]
id = "xnu"
title = "Boot ({ENTRY_TITLE})"
type = "xnu"
path = "Payloads/kernel.macho"
{INITRD_LINE}args = "{KERNEL_ARGS}"
{INSTALL_ENTRY}''')

    make_fat_image(STAGE, FAT_IMAGE)
    kernel_stage_sha = digest(PAYLOADS / 'kernel.macho')
    adt_stage_sha = digest(PAYLOADS / 'kernel.macho.adt')

    if firmware_loader:
        # The volume image holds everything; the staging copy (a second copy of
        # the kernel) only costs space the repository's reserve needs.
        report = {'kernel_sha256': digest(kernel), 'kernel_bytes': kernel.stat().st_size, 'kernel_path': str(kernel.relative_to(ROOT)) if kernel.is_relative_to(ROOT) else kernel.name,
                  'adt_sha256': adt_stage_sha, 'kernel_args': KERNEL_ARGS,
                  'dram_size': hex(DRAM_SIZE), 'loader': 'firmware volume',
                  'payload_image': str(FAT_IMAGE.relative_to(ROOT)),
                  'payload_image_sha256': digest(FAT_IMAGE), 'payload_image_bytes': FAT_IMAGE.stat().st_size,
                  'qemu_chain_checked': False, 'hardware_tested': False,
                  'xnu_probe': xnu_probe, 'xnu_reset_after': reset_after, 'payload_kernel_sha256': kernel_stage_sha,
                  'original_trust_cache_sha256': digest(trust_cache) if trust_cache else None}
        (OUT / 'payload-report-fwloader.json').write_text(json.dumps(report, indent=2) + '\n')
        shutil.rmtree(STAGE)
        print(json.dumps(report, indent=2))
        return

    if _ESP_STAGE.exists():
        shutil.rmtree(_ESP_STAGE)
    (_ESP_STAGE / 'EFI/BOOT').mkdir(parents=True)
    shutil.copyfile(UNIXNU, _ESP_STAGE / 'EFI/BOOT/BOOTAA64.EFI')

    esp_lba, ud_lba = build_gpt_test_disk(_ESP_STAGE, FAT_IMAGE)

    log = OUT / 'payload-chain.log'
    log.unlink(missing_ok=True)
    subprocess.run(['python3', UNIXNU_ROOT / 'package/Scripts/qemu-run.py', _ESP_STAGE,
                    '--log', log, '--cpus', '1', '--timeout', '180',
                    '--disk', TEST_DISK,
                    '--expect', r'UNIXNU: config: loaded \\EFI\\UniXNU\\unixnu.toml',
                    '--expect', r'DARWINBOOT: loader v2 installed EL1',
                    '--expect', r'UNIXNU: boot: entry \'xnu\'',
                    '--expect', r'DARWINBOOT: inputs validated; exiting boot services',
                    '--reject', r'DARWINBOOT[^\n]*FAIL', '--reject', r'ASSERT'],
                   check=True)

    report = {
        'kernel_sha256': digest(KERNEL), 'kernel_bytes': KERNEL.stat().st_size,
        'darwinboot_sha256': digest(DARWINBOOT), 'unixnu_sha256': digest(UNIXNU),
        'adt_sha256': digest(PAYLOADS / 'kernel.macho.adt'),
        'kernel_args': KERNEL_ARGS, 'dram_size': hex(DRAM_SIZE),
        'userdata_image': str(FAT_IMAGE.relative_to(ROOT)),
        'userdata_image_sha256': digest(FAT_IMAGE), 'userdata_image_bytes': FAT_IMAGE.stat().st_size,
        'delivery': 'user runs: fastboot flash userdata build/sheng/boot/sheng-userdata-payload.img',
        'qemu_gpt_fidelity': {'test_disk': str(TEST_DISK.relative_to(ROOT)),
                              'esp_lba': esp_lba, 'userdata_lba': ud_lba,
                              'config_discovered_on_userdata_partition': True},
        'hardware_tested': False,
    }
    (OUT / 'payload-report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
