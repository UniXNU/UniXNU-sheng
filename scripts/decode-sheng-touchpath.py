#!/usr/bin/env python3
"""Decode TPR1 contact observations. Coordinates are relative to each contact's first sample."""
import argparse
from collections import Counter
import json
from pathlib import Path
import struct

HEADER = struct.Struct('<4sIII')
ROW = struct.Struct('<QQQiHBB4hH6B')


def decode(source, output=None):
    data = source.read_bytes()
    magic, total, kept, size = HEADER.unpack_from(data)
    if magic != b'TPR1' or size != ROW.size or kept > total or len(data) != HEADER.size + kept * size:
        raise ValueError('inconsistent TPR1 header or record count')
    frames = Counter()
    previous = None
    for row in ROW.iter_unpack(memoryview(data)[HEADER.size:]):
        irq, read, returned, result, counter, contact, flags, rx, ry, x, y, peer, count, edges, miss, merges, begins, edge_begins = row
        if (counter, read) != previous:
            previous = counter, read
            frames['observed'] += 1
            frames['hid_reports'] += bool(flags & 16)
            frames['hid_failures'] += bool(flags & 16 and result)
            frames['hid_begins'] += begins
            frames['hid_edge_begins'] += edge_begins
            frames['max_contacts'] = max(frames['max_contacts'], count)
        if output:
            entry = dict(counter=counter, last_irq_us=irq or None, read_us=read,
                         publish_return_us=returned, hid_result=result if flags & 16 else None,
                         id=contact, begin=bool(flags & 1), end=bool(flags & 2),
                         held=bool(flags & 4), core_visible=bool(flags & 8),
                         raw_offset=None if flags & 2 else [rx, ry], output_offset=None if flags & 2 else [x, y],
                         nearest_peer=None if peer == 65535 else peer, contacts=count,
                         origin_edges=edges, miss_reason=miss, merged_peaks=merges,
                         hid_begins=begins, hid_edge_begins=edge_begins)
            output.write(json.dumps(entry, separators=(',', ':')) + '\n')
    return dict(total=total, retained=kept, overwritten=total-kept, frames=dict(frames))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('input', type=Path)
    parser.add_argument('--jsonl', type=Path, help='write one row per contact observation, distances in 0.1 panel px')
    args = parser.parse_args()
    if args.jsonl:
        with args.jsonl.open('w') as stream:
            summary = decode(args.input, stream)
    else:
        summary = decode(args.input)
    print(json.dumps(summary))
