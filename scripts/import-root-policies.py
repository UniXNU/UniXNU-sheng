#!/usr/bin/env python3
"""Import signed root headers and their public anchor from a verified boot image."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import sys
import tempfile

DEVICE = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image', type=Path)
    parser.add_argument('--policies', type=Path, required=True, help='selected profile directory with verification.json files')
    args = parser.parse_args()
    spec = importlib.util.spec_from_file_location('lab', DEVICE / 'scripts/run-sheng-ci-gpu-lab.py')
    lab = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(lab)
    sys.path.insert(0, str(lab.CORE / 'kits/XnuPrelink'))
    from kc_image import Image
    with tempfile.TemporaryDirectory(prefix='root-policy-import-') as directory:
        work = Path(directory)
        kernel, policy = work / 'kernel.macho', work / 'root-policies.bin'
        lab.extract_kernel(args.image, kernel, work, policy)
        data = kernel.read_bytes()
        image = Image(data)
        candidates = [image] + [Image(data, offset) for _, offset in image.entries.values()]
        key = None
        for candidate in candidates:
            for address, names in candidate.symbols().items():
                if '_verified_root_key' in names:
                    key = candidate.read(address, 395)
                    break
            if key is not None:
                break
        if key is None or key[:8] != b'VROOTKEY':
            raise SystemExit('kernel has no public root trust anchor')
        bundle = policy.read_bytes()
        magic, version, count, length = struct.unpack_from('<8sQQQ', bundle)
        if magic != b'VROOTB01' or version != 1 or count != 3 or length != len(bundle):
            raise SystemExit('require the selected three-window root policy bundle')
        digest = hashlib.sha256(key).digest()
        outputs = []
        for index, name in enumerate(('OS', 'SystemOS', 'App')):
            destination = args.policies / name
            record = json.loads((destination / 'verification.json').read_text())
            offset, size = struct.unpack_from('<QQ', bundle, 32 + 16 * index)
            header = bundle[offset:offset + size]
            if size != 544 or header[56:88].hex() != record['image_sha256'] or header[120:152] != digest:
                raise SystemExit(name + ': header differs from the selected image or kernel anchor')
            # RSA-3072 PKCS#1 v1.5 DigestInfo for the signed 160-byte header.
            encoded = bytes.fromhex('3031300d060960864801650304020105000420') + hashlib.sha256(header[:160]).digest()
            expected = b'\0\1' + b'\xff' * (384 - len(encoded) - 3) + b'\0' + encoded
            signature = pow(int.from_bytes(header[160:], 'big'), int.from_bytes(key[392:], 'big'),
                            int.from_bytes(key[8:392], 'big')).to_bytes(384, 'big')
            if signature != expected:
                raise SystemExit(name + ': root header signature failed')
            record['trusted_key_sha256'] = digest.hex()
            outputs.append((destination, header, record))
        for destination, header, record in outputs:
            (destination / 'manifest.header.bin').write_bytes(header)
            (destination / 'trusted-key.bin').write_bytes(key)
            (destination / 'verification.json').write_text(json.dumps(record, indent=2) + '\n')
        print('Imported and authenticated three root policy headers and their public anchor.')


if __name__ == '__main__':
    main()
