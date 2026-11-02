#!/usr/bin/env python3
"""Acceptance figures of a sheng latency capture, inside touch windows only.

Reads RUN/diag (written by read-sheng-log.py) and prints one JSON object:
frame intervals while a finger moves, large GPU allocations, full-screen
pass times, slow completions, encode thread scheduling and client crashes.
"""
import argparse
import bisect
import collections
import json
from pathlib import Path
import struct


def pct(values, q):
    return values[min(len(values) - 1, int(q * len(values)))] if values else None


def report(diag):
    files = diag / 'files'
    touch_blob = (files / 'kernel.touch.bin').read_bytes()
    _, _, kept, _ = struct.unpack_from('<4I', touch_blob)
    touches = sorted(t for (t,) in struct.iter_unpack('<Q', touch_blob[16:16 + kept * 8]))
    windows = json.loads((diag / 'latency.touch-windows.json').read_text())['windows']

    def inside(us):
        return any(w['begin_us'] <= us <= w['end_us'] for w in windows)

    out = {'touch_windows': len(windows), 'touch_seconds': round(sum(w['end_us'] - w['begin_us'] for w in windows) / 1e6, 1)}

    # Frame intervals: display flips (swap ring event 4) with a touch report between them.
    flips = sorted(int(l.split()[1]) for l in open(files / 'kernel.rings.txt') if l.startswith('S ') and l.split()[2] == '4')
    intervals, gaps = [], 0
    for a, b in zip(flips, flips[1:]):
        i, j = bisect.bisect_left(touches, a), bisect.bisect_left(touches, b)
        if j > i:
            intervals.append((b - a) / 1000)
        if b - a >= 100000 and j - i >= 3:
            gaps += 1
    intervals.sort()
    out['frame_ms'] = {'n': len(intervals), 'p50': pct(intervals, .5), 'p90': pct(intervals, .9), 'p99': pct(intervals, .99),
                       'max': intervals[-1] if intervals else None}
    out['touch_gaps_100ms'] = gaps

    # Large GPU allocations: page wiring per 64 MiB from kernel.boalloc.bin, runtime allocation times from the frame trace.
    blob = (files / 'kernel.boalloc.bin').read_bytes()
    _, _, count, _ = struct.unpack_from('<4I', blob)
    wires = []
    for i in range(count):
        entered, kib, _, limit_us, create_us, wire_us = struct.unpack_from('<QIIIII', blob, 16 + i * 256)
        if kib >= 32768 and inside(entered):
            wires.append(round(wire_us / 1000 * 65536 / kib, 1))
    wires.sort()
    out['wire_ms_per_64mib'] = {'n': len(wires), 'p50': pct(wires, .5), 'max': wires[-1] if wires else None}
    frontend = [json.loads(l) for l in open(diag / 'latency.frontend.jsonl')]
    memory, completion = {}, {}
    for row in frontend:
        if row['name'] == 'runtime_stage_wide':
            if 40 <= row['stage'] <= 47:
                memory.setdefault(row['runtime'], {})[row['stage']] = (row['us'], row['argument'])
            elif 53 <= row['stage'] <= 57:
                completion.setdefault(row['runtime'], {})[row['stage']] = row['us']
    allocations = sorted(((s[44][0] - s[40][0]) / 1000 for s in memory.values()
                          if 40 in s and 44 in s and s[44][1] == 1 and s.get(42, (0, 0))[1] >= 32768 and inside(s[40][0])), reverse=True)
    out['allocations_over_50ms'] = sum(a > 50 for a in allocations)
    out['allocation_max_ms'] = allocations[0] if allocations else 0
    out['completions_over_20ms'] = sum(1 for s in completion.values() if 53 in s and 57 in s and s[57] - s[53] > 20000 and inside(s[53]))

    passes = sorted(r['gpu_elapsed_us'] / 1000 for r in frontend
                    if r['name'] == 'gpu_pass' and r.get('gpu_elapsed_us') and r['width'] >= 3000 and inside(r['us']))
    out['fullscreen_pass_ms'] = {'n': len(passes), 'p95': pct(passes, .95), 'max': passes[-1] if passes else None,
                                 'over_8_33': sum(p > 8.333 for p in passes)}
    encode = sorted((r['elapsed_us'] / 1000 for r in frontend if r['name'] == 'cpu_detail' and r.get('phase') == 2
                     and r.get('detail_name') == 'encoding' and inside(r['us'])), reverse=True)
    out['encode_calls_over_30ms'] = sum(e > 30 for e in encode)
    out['encode_call_max_ms'] = encode[0] if encode else 0

    # Encode thread (backboardd role 2) priority and run-queue share from the client sampler.
    blob = (files / 'kernel.client.bin').read_bytes()
    _, _, count, _ = struct.unpack_from('<4I', blob)
    sample = struct.Struct('<QiHhIiQQ11Q')
    pids = {}
    for line in open(files / 'kernel.rings.txt'):
        if line.startswith('C '):
            parts = line.split()
            pids.setdefault(parts[4], int(parts[1]))
    server = pids.get('backboardd')
    priorities, queued, total = collections.Counter(), 0, 0
    for i in range(count):
        r = sample.unpack_from(blob, 16 + i * 128)
        if r[1] == server and (r[2] >> 12) & 3 == 2 and not r[2] & 0x4000 and inside(r[0]):
            total += 1
            priorities[r[3]] += 1
            queued += bool(r[2] & 0x8000)
    out['encode_thread'] = {'samples': total, 'priorities': priorities.most_common(3), 'run_queue_share': round(queued / total, 3) if total else None}

    # Client crashes: signal exits by name.
    names, crashes = {}, collections.Counter()
    rows = [json.loads(l) for l in open(diag / 'latency.jsonl')]
    for row in rows:
        if row['kind'] == 14:
            names[row['fields']['task_pid']] = row.get('text')
    for row in rows:
        if row['kind'] == 22 and row['fields']['status'] & 127 in (4, 5, 6, 9, 10, 11):
            crashes[names.get(row['fields']['subject_pid'], str(row['fields']['subject_pid']))] += 1
    out['crashes'] = dict(crashes)
    problems = diag / 'gpu' / 'events.jsonl'
    if problems.exists():
        out['refusals'] = sorted({(r['pid'], r['text']) for r in map(json.loads, open(problems)) if r['event'] == 65})
    return out


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('diag', type=Path, help='RUN/diag of read-sheng-log.py')
    print(json.dumps(report(parser.parse_args().diag), indent=1))
