#!/usr/bin/env python3
"""iPadOS 27 Data seed that skips Setup but still runs data migration (which installs the staged system apps), with the render
server's framebuffer preferences of add-gpu-plugin-data.py --publish (no deep RGhA framebuffer, no compression).
usage: make-nosetup-seed27.py BASE.dbr OUTPUT.dbr [--keep-migration]   (BASE: data-seed-215g-skip)
--keep-migration keeps com.apple.migration (no first-boot data migration: on 27 it kept all cores busy for over 20 minutes, and 17's
seed skipped it too while its staged apps still appeared)."""
import plistlib, subprocess, sys, tempfile
from pathlib import Path
TREE = Path(__file__).resolve().parents[2] / 'UniXNU/out/sheng/tree'
sys.path.insert(0, str(TREE / 'drivers/DarwinBoot/Test'))
import packed_ramdisk
keep = '--keep-migration' in sys.argv[1:]
args = [a for a in sys.argv[1:] if a != '--keep-migration']
base, output = Path(args[0]).resolve(), Path(args[1]).resolve()
if output.exists():
    sys.exit('Refuse to overwrite ' + str(output))
logical, extents = packed_ramdisk.regions(base.read_bytes())
with tempfile.TemporaryDirectory(prefix='seed27-') as td:
    raw = Path(td) / 'seed.raw'
    with raw.open('wb') as f:
        f.truncate(logical)
        for offset, data in extents:
            f.seek(offset); f.write(data)
    mount = Path(td) / 'mount'; mount.mkdir()
    attached = plistlib.loads(subprocess.check_output(['hdiutil', 'attach', '-imagekey', 'diskimage-class=CRawDiskImage', '-nobrowse',
                                                       '-owners', 'off', '-mountpoint', str(mount), '-plist', str(raw)]))
    device = attached['system-entities'][0]['dev-entry']
    try:
        migration = mount / 'mobile/Library/Preferences/com.apple.migration.plist'
        if not migration.exists():
            sys.exit('no com.apple.migration.plist: wrong base seed?')
        if not keep:
            migration.unlink()
        for home, name in [(h, n) for h in ('mobile', 'root') for n in ('backboardd.plist', '.GlobalPreferences.plist')]:
            directory = mount / home / 'Library/Preferences'
            directory.mkdir(parents=True, exist_ok=True)
            file = directory / name
            values = plistlib.loads(file.read_bytes()) if file.exists() else {}
            values.update({'CAEnableDeepFramebuffer': False, 'CAEnableFramebufferCompression': False, 'CAEnableIOSurfaceCompression': False})
            file.write_bytes(plistlib.dumps(values, fmt=plistlib.FMT_BINARY))
            file.chmod(0o644)
            print('prefs', file.relative_to(mount))
    finally:
        subprocess.run(['hdiutil', 'detach', device], check=True)
    with raw.open('rb') as f:
        head = f.read(64)
    if head[32:36] != b'NXSB':
        sys.exit('The edited image is no APFS container any more')
    output.parent.mkdir(parents=True, exist_ok=True)
    print(packed_ramdisk.pack(raw, output))
