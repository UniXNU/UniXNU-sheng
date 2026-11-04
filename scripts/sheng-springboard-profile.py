#!/usr/bin/env python3
"""Decode sheng's opt-in SpringBoard Mach calls and EL0 PMU samples."""
import argparse
import collections
import json
from pathlib import Path
import struct

HEADER = struct.Struct('<4sIII')
ROW = struct.Struct('<QQII6Q')
WAIT_KIND = ('receive', 'send and receive on reply port', 'explicit synchronous receive',
             'combined call on a different receive port')
PMU_STATUS = {1: 'ready', 2: 'PMU not advertised', 3: 'register access trapped',
              4: 'counters in use', 5: 'required events unavailable', 6: 'configuration lost'}


def profile(path):
    data = path.read_bytes()
    magic, total, count, dropped = HEADER.unpack_from(data)
    if magic != b'LAT1' or len(data) != HEADER.size + count * ROW.size:
        raise ValueError(f'invalid LAT1 export in {path}')
    names = {0: 'kernel'}
    calls, samples, overhead, targets = [], [], [], []
    for us, tid, kind, pid, a, b, c, d, e, f in ROW.iter_unpack(memoryview(data)[HEADER.size:]):
        if kind == 14:
            name = struct.pack('<4Q', c, d, e, f).split(b'\0', 1)[0].decode(errors='replace')
            if name:
                names[b] = name
        elif kind in (37, 40):
            peer = d & 0xffffffff
            mode = d >> 32
            calls.append(dict(pid=pid, tid=tid, begin_us=a, receive_us=b or None, end_us=c,
                              elapsed_us=c-a, receive_elapsed_us=c-b if b else None,
                              kind=WAIT_KIND[mode], destination_pid=peer if peer < 0x80000000 else None,
                              message_id=e, result=f if kind == 37 else None, pending=kind == 40))
        elif kind == 38:
            samples.append((tid, a, b, c, d, e, f))
        elif kind == 39:
            targets.append(dict(time_us=us, tid=a, minimum_wait_us=b, xnu_cpu_counters=c))
        elif kind == 41:
            overhead.append((a, b, c, d, e, f))

    for call in calls:
        call['destination'] = names.get(call['destination_pid'])
    previous = {}
    spans = []
    for tid, us, cpu, switches, status, ins, cycles in sorted(samples):
        old = previous.get(tid)
        previous[tid] = (us, cpu, switches, status, ins, cycles)
        if old is None or status != 1 or old[1:4] != (cpu, switches, 1):
            continue
        elapsed = us - old[0]
        dc, di = (cycles-old[5]) & 0xffffffff, (ins-old[4]) & 0xffffffff
        # Two EL0 observations of the same uninterrupted thread. The 20 ms
        # bound rules out multiple wraps and sampling gaps across idle epochs.
        if 0 < elapsed <= 20000 and 0 < dc <= 4000 * elapsed:
            spans.append(dict(tid=tid, cpu=cpu, begin_us=old[0], end_us=us,
                              instructions=di, cycles=dc))
    cores = {}
    for cpu in sorted({s['cpu'] for s in spans}):
        rows = [s for s in spans if s['cpu'] == cpu]
        inst = sum(s['instructions'] for s in rows)
        cycles = sum(s['cycles'] for s in rows)
        cores[cpu] = dict(intervals=len(rows), instructions=inst, cycles=cycles, ipc=round(inst/cycles, 4))

    probe_us = sum(b-a for a,b,*_ in overhead)
    elapsed_ns = sum(r[4] for r in overhead)
    return dict(latency_records=count, total=total, dropped=dropped, targets=targets, calls=calls,
                pmu_status=collections.Counter(PMU_STATUS.get(r[4], str(r[4])) for r in samples),
                pmu_by_core=cores, pmu_spans=spans,
                probe_cost=dict(windows=len(overhead), attempts=sum(r[2] for r in overhead),
                                exact=sum(r[3] for r in overhead), elapsed_ns=elapsed_ns,
                                max_ns=max((r[5] for r in overhead), default=0),
                                sampler_wall_share=elapsed_ns/(probe_us*1000) if probe_us else None))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('latency', type=Path, help='extracted kernel.latency.bin')
    print(json.dumps(profile(parser.parse_args().latency), indent=1))
