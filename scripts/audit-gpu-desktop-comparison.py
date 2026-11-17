#!/usr/bin/env python3
"""Compare completed desktop captures using their frame trace and pane timings."""
import argparse
from collections import defaultdict
from datetime import datetime
import json
from pathlib import Path
import re
import statistics
import struct

RECORD = struct.Struct('<II4Q8I')


def distribution(values):
    if not values:
        return {'count': 0}
    values.sort()
    return {'count': len(values), 'median': round(statistics.median(values), 3),
            'p95': round(values[int(.95 * (len(values) - 1))], 3)}


def wall_us(value):
    return datetime.fromisoformat(value.replace('Z', '+00:00')).timestamp() * 1e6


def capture(directory, skip):
    files = directory / 'diag/files'
    session = files / 'Users/Shared/unixnu-test'
    state = (session / 'graphics-state.log').read_text()
    boot = re.search(r'^graphics state boot=(\S+)', state)[1]
    servers = set(re.findall(r'^\s*(\d+)\s+\d+\s+.+/WindowServer\s*$', state, re.M))
    if len(servers) > 1:
        raise ValueError(f'{directory}: WindowServer restarted during the capture')
    log = (session / 'agent.log').read_text()
    start = log.rfind('comparison start boot=' + boot + '\n')
    if start < 0:
        raise ValueError(f'{directory}: no comparison for boot {boot}')
    log = log[start:]
    panes = re.findall(r'comparison round=(\d+) pane=(\S+) time=(\S+)', log)
    done = re.search(r'comparison complete time=(\S+)', log)
    if not panes or not done:
        raise ValueError(f'{directory}: comparison did not complete')
    blob = (files / 'private/var/tmp/NativeAdrenoMetal.trace/frames.trace').read_bytes()
    header = next(row for row in RECORD.iter_unpack(blob) if row[0] == 0)
    offset = header[3] - header[2]
    begin = wall_us(panes[0][2]) - offset + skip * 1e6
    end = wall_us(done[1]) - offset
    if begin >= end:
        raise ValueError(f'{directory}: skip interval exceeds the completed sequence')
    shaders = {row[3]: row[11] for row in RECORD.iter_unpack(blob) if row[0] == 13}
    metrics = defaultdict(list)
    passes = defaultdict(list)
    for row in RECORD.iter_unpack(blob):
        kind, _, t0, t1, t2, t3, a, b, c, d, *_ = row
        when = t1 if kind == 1 else t0
        if not begin <= when <= end:
            continue
        if kind == 1 and t3 >= t1:
            metrics['command_callback_ms'].append((t3 - t1) / 1000)
        elif kind == 7 and t2 and t3 >= t2:
            key = (a & 65535, a >> 16, c & 65535, c >> 16, d, shaders.get(t1))
            passes[key].append((t3 - t2) / 1000)
        elif kind == 8:
            metrics['kernel_call_ms'].append(c / 1000)
            metrics['unsubmitted_sync_wait_ms'].append(b / 1000)
        elif kind == 14 and a == 3:
            metrics['encode_batch_cpu_ms'].append(d / 1000)
    result = {'capture': str(directory), 'boot': boot,
              'pane_requests': [(round_, pane) for round_, pane, _ in panes],
              'timing': re.search(r'comparison timing (.+)', log)[1],
              'first_pane_after_trace_seconds': round((wall_us(panes[0][2]) - header[3]) / 1e6, 3),
              'window_seconds': round((end - begin) / 1e6, 3),
              'metrics': {key: distribution(values) for key, values in metrics.items()}}
    return result, passes


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('baseline', type=Path, help='capture directory containing diag/files')
    parser.add_argument('candidate', type=Path, help='capture directory containing diag/files')
    parser.add_argument('--skip-seconds', type=float, default=30,
                        help='omit this interval after the first pane request (default: 30)')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    try:
        baseline, first = capture(args.baseline, args.skip_seconds)
        candidate, second = capture(args.candidate, args.skip_seconds)
    except ValueError as error:
        parser.error(str(error))
    if baseline['pane_requests'] != candidate['pane_requests'] or baseline['timing'] != candidate['timing']:
        parser.error('captures use different pane requests or timing')
    matched = []
    for signature in first.keys() & second.keys():
        a, b = distribution(first[signature]), distribution(second[signature])
        matched.append({'width': signature[0], 'height': signature[1], 'draws': signature[2],
                        'vk_format': signature[3], 'kpx': signature[4], 'fragment_hash': signature[5],
                        'baseline_ms': a, 'candidate_ms': b})
    matched.sort(key=lambda item: -min(item['baseline_ms']['count'], item['candidate_ms']['count']))
    report = {'skip_seconds': args.skip_seconds, 'baseline': baseline,
              'candidate': candidate, 'matched_gpu_passes': matched}
    output = json.dumps(report, indent=2) + '\n'
    if args.output:
        args.output.write_text(output)
    else:
        print(output, end='')


if __name__ == '__main__':
    main()
