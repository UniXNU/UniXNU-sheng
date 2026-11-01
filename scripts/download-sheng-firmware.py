#!/usr/bin/env python3
"""Download the requested sheng fastboot archive with resumable HTTP ranges."""
import argparse
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import threading
import time
import urllib.request

import tomllib

ROOT = Path(__file__).resolve().parents[1]
directory = ROOT / '.cache/firmware/sheng'
BOARD = Path(os.environ.get('UNIXNU_DEVICE') or ROOT) / 'board.toml'
image = tomllib.load(open(BOARD, 'rb'))['firmware']['image']
URL, MD5 = image['url'], image['md5']
retained = directory / 'porting-inputs.json'
if retained.is_file():
    import json
    record = json.loads(retained.read_text())
    if record.get('archive_deleted') and record.get('source_url') == URL and record.get('archive_sha256') == image['sha256']:
        print(f'firmware inputs already extracted from {URL.rsplit("/", 1)[1]}')
        raise SystemExit(0)
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--connections', type=int, default=32)
args = parser.parse_args()
if not 1 <= args.connections <= 64:
    parser.error('Use between 1 and 64 connections')
directory.mkdir(parents=True, exist_ok=True)
target = directory / URL.rsplit('/', 1)[1]
partial = target.with_suffix('.tgz.part')
state_path = target.with_suffix('.tgz.ranges.json')
request = urllib.request.Request(URL, headers={'Range': 'bytes=0-0'})
with urllib.request.urlopen(request, timeout=30) as response:
    match = re.fullmatch(r'bytes 0-0/(\d+)', response.headers.get('Content-Range', ''))
    if response.status != 206 or not match or response.read() != b'\x1f':
        raise SystemExit('Expected a ranged gzip response; refusing a whole-body fallback')
    size = int(match[1])
    etag = response.headers.get('ETag')
chunk_size = 8 * 1024 * 1024
count = (size + chunk_size - 1) // chunk_size
state = {'url': URL, 'bytes': size, 'etag': etag, 'chunk_bytes': chunk_size, 'completed': {}}
if state_path.exists():
    previous = json.loads(state_path.read_text())
    if any(previous.get(key) != state[key] for key in ('url', 'bytes', 'etag', 'chunk_bytes')):
        raise SystemExit('Resume metadata differs; preserve it for inspection before restarting')
    state = previous
if target.exists():
    partial = target
elif state['completed'] and not partial.exists():
    raise SystemExit('Resume state exists without its archive')
allocated = partial.stat().st_blocks * 512 if partial.exists() else 0
descriptor = os.open(partial, os.O_RDWR | os.O_CREAT, 0o600)
os.ftruncate(descriptor, size)
lock = threading.Lock()
stop = threading.Event()


def persist():
    temporary = state_path.with_suffix('.json.tmp')
    temporary.write_text(json.dumps(state, indent=2) + '\n')
    temporary.replace(state_path)


def fetch(index):
    begin = index * chunk_size
    end = min(size, begin + chunk_size)
    for attempt in range(4):
        if stop.is_set():
            raise RuntimeError('Download cancelled after another range failed')
        try:
            request_started = time.monotonic()
            headers = {'Range': f'bytes={begin}-{end - 1}'}
            if etag:
                headers['If-Match'] = etag
            request = urllib.request.Request(URL, headers=headers)
            digest = hashlib.sha256()
            position = begin
            with urllib.request.urlopen(request, timeout=30) as response:
                if response.status != 206 or response.headers.get('Content-Range') != f'bytes {begin}-{end - 1}/{size}':
                    raise RuntimeError('Range response does not match the requested extent')
                while position < end:
                    if time.monotonic() - request_started > 45:
                        raise TimeoutError('Slow range exceeded its 45-second attempt budget')
                    blob = response.read1(min(256 * 1024, end - position))
                    if not blob:
                        raise RuntimeError('Truncated range response')
                    digest.update(blob)
                    written = 0
                    while written < len(blob):
                        written += os.pwrite(descriptor, blob[written:], position + written)
                    position += len(blob)
            with lock:
                state['completed'][str(index)] = digest.hexdigest()
                if len(state['completed']) % 8 == 0 or len(state['completed']) >= count - 32:
                    persist()
            return end - begin
        except Exception:
            if attempt == 3:
                stop.set()
                raise
            time.sleep(0.5 * (attempt + 1))


try:
    pending = []
    for index in range(count):
        previous_hash = state['completed'].get(str(index))
        if previous_hash:
            blob = os.pread(descriptor, min(chunk_size, size - index * chunk_size), index * chunk_size)
            if hashlib.sha256(blob).hexdigest() == previous_hash:
                continue
            del state['completed'][str(index)]
        pending.append(index)
    persist()
    started = time.monotonic()
    last = started
    received = 0
    print(f'{size / 1024**3:.2f} GiB; {args.connections} connections; {len(pending)}/{count} ranges pending', flush=True)
    if partial != target:
        with concurrent.futures.ThreadPoolExecutor(max_workers=args.connections) as executor:
            futures = [executor.submit(fetch, index) for index in pending]
            for future in concurrent.futures.as_completed(futures):
                received += future.result()
                now = time.monotonic()
                if now - last >= 10:
                    print(f'{len(state["completed"])}/{count} ranges; {received / 1024**2 / (now - started):.1f} MiB/s', flush=True)
                    last = now
finally:
    with lock:
        persist()
    os.close(descriptor)
md5 = hashlib.md5()
sha256 = hashlib.sha256()
with partial.open('rb') as archive:
    while blob := archive.read(4 * 1024 * 1024):
        md5.update(blob)
        sha256.update(blob)
if md5.hexdigest() != MD5:
    raise SystemExit(f'Archive MD5 mismatch: {md5.hexdigest()}')
if sha256.hexdigest() != image['sha256']:
    raise SystemExit(f'Archive SHA-256 mismatch: {sha256.hexdigest()}')
state.update(md5=md5.hexdigest(), sha256=sha256.hexdigest(), verified=True)
persist()
if partial != target:
    partial.replace(target)
print(f'Verified archive: {target.name}; SHA-256 {sha256.hexdigest()}', flush=True)
