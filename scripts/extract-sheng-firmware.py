#!/usr/bin/env python3
"""Stream the verified fastboot archive, retaining only porting inputs."""
import argparse
import gzip
import hashlib
import json
from pathlib import Path
import re
import shutil
import tarfile
import time

ROOT = Path(__file__).resolve().parents[1]
directory = ROOT / '.cache/firmware/sheng'
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--delete-archive', action='store_true')
args = parser.parse_args()
record_path = directory / 'porting-inputs.json'
if record_path.is_file():
    import json as _json
    record = _json.loads(record_path.read_text())
    if (record.get('archive_deleted') and record.get('source_url') == image['url']
            and record.get('archive_sha256') == image['sha256']):
        print('firmware inputs already extracted; nothing to do')
        raise SystemExit(0)
states = list(directory.glob('*.tgz.ranges.json'))
if len(states) != 1:
    raise SystemExit('Expected one firmware download manifest')
download = json.loads(states[0].read_text())
import os
import tomllib
board = Path(os.environ.get('UNIXNU_DEVICE') or ROOT) / 'board.toml'
image = tomllib.load(open(board, 'rb'))['firmware']['image']
if (not download.get('verified') or download.get('url') != image['url'] or
        download.get('md5') != image['md5'] or download.get('sha256') != image['sha256']):
    raise SystemExit('Download must finish its verification against board.toml [firmware.image] first')
archive = directory / download['url'].rsplit('/', 1)[1]
if archive.stat().st_size != image['bytes']:
    raise SystemExit('Archive size changed after verification')
output = directory / 'porting-inputs'
output.mkdir(exist_ok=True)
selected = []
inventory = []
started = time.monotonic()
last = started
total_kept = 0
firmware = re.compile(r'^(xbl[^/]*|uefi[^/]*|abl|aop[^/]*|devcfg|hyp|tz|dtbo|boot|vendor_boot|init_boot)\.(elf|img|bin|mbn)$')
with gzip.open(archive, 'rb') as inflated:
    with tarfile.open(fileobj=inflated, mode='r|', bufsize=1024 * 1024) as stream:
        for member in stream:
            if not member.isfile():
                continue
            name = Path(member.name).name
            inventory.append({'path': member.name, 'bytes': member.size})
            retain = firmware.fullmatch(name.lower()) or (
                member.size < 1024 * 1024 and
                (name.startswith('flash_all') or name.lower() in ('md5sum.xml', 'partition.xml', 'android-info.txt')))
            if not retain:
                if time.monotonic() - last >= 15:
                    print(f'Scanning {member.name}: {member.size / 1024**2:.0f} MiB (not extracted)', flush=True)
                    last = time.monotonic()
                continue
            if member.size > 512 * 1024**2 or total_kept + member.size > 2 * 1024**3:
                raise SystemExit(f'Unexpectedly large selected input: {member.name}')
            destination = output / name
            if any(entry['file'] == name for entry in selected):
                raise SystemExit(f'Duplicate selected basename: {name}')
            digest = hashlib.sha256()
            length = 0
            partial = destination.with_suffix(destination.suffix + '.part')
            with stream.extractfile(member) as source, partial.open('wb') as target:
                while blob := source.read(1024 * 1024):
                    target.write(blob)
                    digest.update(blob)
                    length += len(blob)
            if length != member.size:
                raise SystemExit(f'Truncated tar member: {member.name}')
            partial.replace(destination)
            selected.append({'file': name, 'tar_path': member.name,
                             'bytes': length, 'sha256': digest.hexdigest()})
            total_kept += length
            print(f'Kept {name}: {length / 1024**2:.1f} MiB', flush=True)
    # Tar EOF can precede gzip EOF; consume the trailer to verify gzip CRC/size.
    while inflated.read(1024 * 1024):
        pass
if not any(entry['file'].lower().startswith('xbl') for entry in selected):
    raise SystemExit('No XBL input was found; preserve the archive for inspection')
manifest = {'source_url': download['url'], 'archive_bytes': download['bytes'],
            'archive_md5': download['md5'], 'archive_sha256': download['sha256'],
            'gzip_integrity_verified': True, 'selected': selected, 'inventory': inventory,
            'retained_bytes': total_kept, 'seconds': round(time.monotonic() - started, 2),
            'archive_deleted': False}
manifest_path = directory / 'porting-inputs.json'
manifest_path.write_text(json.dumps(manifest, indent=2) + '\n')
if args.delete_archive:
    archive.unlink()
    manifest['archive_deleted'] = True
    manifest_path.write_text(json.dumps(manifest, indent=2) + '\n')
print(f'Kept {len(selected)} inputs, {total_kept / 1024**2:.1f} MiB; archive deleted={manifest["archive_deleted"]}', flush=True)
