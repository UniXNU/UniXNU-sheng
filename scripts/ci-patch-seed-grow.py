#!/usr/bin/env python3
"""Add a wait-and-retry to the APFS container probe in the framework's seed-data-grow.py.

On the macos-26 runner the container is not yet registered with diskutil the instant
`hdiutil attach` returns, so the author's single immediate `diskutil apfs list` probe
finds nothing and the Data seed fails ("No APFS container found on the attached seed").
The mechanism itself is sound (an isolated job detected and resized the container once
given a moment). This patch only waits for the container to appear; it changes no other
behavior. Idempotent, and it refuses to run if the expected source block is not present.

  ci-patch-seed-grow.py TREE/scripts/seed-data-grow.py
"""
import sys
from pathlib import Path

target = Path(sys.argv[1])
text = target.read_text()

if 'for _attempt in range(30):' in text:
    print('seed-data-grow.py already patched; nothing to do')
    raise SystemExit(0)

old = """            listing = plistlib.loads(subprocess.check_output(['diskutil', 'apfs', 'list', '-plist']))
            container = None
            for entry in listing['Containers']:
                if any(s['DeviceIdentifier'] == store for s in entry.get('PhysicalStores', [])):
                    container = entry['ContainerReference']
            if not container:
"""
new = """            container = None
            for _attempt in range(30):
                listing = plistlib.loads(subprocess.check_output(['diskutil', 'apfs', 'list', '-plist']))
                for entry in listing['Containers']:
                    if any(s['DeviceIdentifier'] == store for s in entry.get('PhysicalStores', [])):
                        container = entry['ContainerReference']
                if container:
                    break
                time.sleep(2)
            if not container:
"""
if old not in text:
    raise SystemExit('Expected container-probe block not found in ' + str(target) + '; refusing to patch blindly')
text = text.replace(old, new, 1)

if '\nimport time\n' not in text:
    if '\nimport tempfile\n' not in text:
        raise SystemExit('import anchor not found; refusing to patch blindly')
    text = text.replace('\nimport tempfile\n', '\nimport tempfile\nimport time\n', 1)

target.write_text(text)
print('patched seed-data-grow.py: container probe now retries up to 30 x 2s')