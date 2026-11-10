"""Validate retained stock firmware before skipping its archive download."""
import hashlib
import json
from pathlib import Path


def retained_inputs(directory: Path, image: dict) -> bool:
    try:
        record = json.loads((directory / 'porting-inputs.json').read_text())
        if not isinstance(record, dict):
            return False
        if (record.get('source_url') != image['url'] or
                record.get('archive_sha256') != image['sha256'] or
                not record.get('gzip_integrity_verified') or not record.get('selected')):
            return False
        names = set()
        for entry in record['selected']:
            name = entry['file']
            if Path(name).name != name or name in ('.', '..') or name in names:
                return False
            names.add(name)
            path = directory / 'porting-inputs' / name
            if path.stat().st_size != entry['bytes']:
                return False
            digest = hashlib.sha256()
            with path.open('rb') as stream:
                while blob := stream.read(1024 * 1024):
                    digest.update(blob)
            if digest.hexdigest() != entry['sha256']:
                return False
        return 'uefi.elf' in names and any(n.startswith('xbl') for n in names)
    except (OSError, ValueError, KeyError, TypeError):
        return False
