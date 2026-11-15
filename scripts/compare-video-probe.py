#!/usr/bin/env python3
"""Compare the sampled decoder pixels in video-probe output with FFmpeg."""
import argparse
import json
from pathlib import Path
import re
import subprocess
import struct


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('fixture', type=Path)
    parser.add_argument('log', type=Path)
    parser.add_argument('--sample', type=int, default=0, help='zero-based frame sample in a multi-fixture log')
    args = parser.parse_args()
    text = args.log.read_text(errors='replace')
    frames = list(re.finditer(r'video-probe pixels format=0x([0-9a-f]+) size=\d+x\d+ coded=(\d+)x(\d+) pts=([\d.]+)', text))
    if not 0 <= args.sample < len(frames):
        parser.error('the log has no decoded frame sample')
    frame = frames[args.sample]
    text = text[frame.end():frames[args.sample+1].start() if args.sample+1 < len(frames) else len(text)]
    width, height, pts = int(frame[2]), int(frame[3]), float(frame[4])
    ten = int(frame[1], 16) in (0x78343230, 0x78663230)
    if not ten and int(frame[1], 16) not in (0x34323076, 0x34323066):
        parser.error('the sample must use 8-bit or 10-bit bi-planar YCbCr')
    raw = subprocess.run(['ffmpeg', '-v', 'error', '-i', str(args.fixture), '-ss', str(pts),
                          '-frames:v', '1', '-pix_fmt', 'p010le' if ten else 'nv12', '-f', 'rawvideo', '-'],
                         check=True, stdout=subprocess.PIPE, timeout=30).stdout
    if len(raw) != width * height * 3 // 2 * (2 if ten else 1):
        parser.error('the reference dimensions differ from the decoder sample')
    values = [value[0] >> 6 for value in struct.iter_unpack('<H', raw)] if ten else raw
    errors = [[], []]
    for match in re.finditer(r'video-probe grid plane=(\d+) row=(\d+) step=(\d+) hex=([0-9a-f]+)', text):
        plane, row = int(match[1]), int(match[2])
        if plane > 1 or row >= height // (2 if plane else 1):
            parser.error('the sampled plane or row is outside the frame')
        at = (width * height if plane else 0) + row * width
        x_step = int(match[3])
        expected = [value for x in range(0, width, x_step)
                    for value in values[at+x:at+x+(2 if plane else 1)]]
        step = 4 if ten else 2
        actual = [int(match[4][x:x+step], 16) for x in range(0, len(match[4]), step)]
        if len(actual) != len(expected):
            parser.error('the sampled row does not cover the coded width')
        errors[plane].extend(abs(a-b) for a, b in zip(actual, expected))
    result = {'pts': pts, 'size': [width, height], 'depth': 10 if ten else 8, 'planes': []}
    for name, values in zip(('Y', 'UV'), errors):
        if not values:
            parser.error('both planes must contain sampled pixels')
        result['planes'].append({'plane': name, 'samples': len(values),
                                 'mean_absolute_error': sum(values)/len(values),
                                 'maximum_absolute_error': max(values)})
    result['passed'] = all(max(values) <= 2 for values in errors)
    print(json.dumps(result, indent=2))
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
