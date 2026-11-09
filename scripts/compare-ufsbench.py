#!/usr/bin/env python3
"""Compare ufsbench results: Linux reader runs (scripts/linux-ufsbench.py, bench.txt) against XNU runs (sheng-ufs-bench=1: the
'ufsbench' lines of the UFS log's msgbuf.txt). Prints the mean MB/s of each test with its range, the XNU/Linux ratio and the mean p99.

usage: compare-ufsbench.py --linux A/bench.txt B/bench.txt ... --xnu C/msgbuf.txt D/msgbuf.txt ..."""
import argparse, re, statistics
from pathlib import Path

LINE = re.compile(r'(rand|seq)\s+bs=(\d+) qd=(\d+) ops=\d+ iops=(\d+) MBps=([\d.]+) avg_us=([\d.]+) p50_us=\d+ p99_us=(\d+) max_us=(\d+)')


def load(path):
    out = {}
    for line in Path(path).read_text(errors='replace').splitlines():
        if ' spin=' in line:   # XNU's spin budget sweep, not one of the common tests
            continue
        m = LINE.search(line)
        if m:
            kind, bs, qd, _, mb, _, p99, _ = m.groups()
            out[(kind, int(bs), int(qd))] = (float(mb), int(p99))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--linux', nargs='+', required=True)
    ap.add_argument('--xnu', nargs='+', required=True)
    a = ap.parse_args()
    linux, xnu = [load(p) for p in a.linux], [load(p) for p in a.xnu]
    keys = [k for k in linux[0] if all(k in r for r in linux + xnu)]
    print(f'{"test":<16}{"Linux MB/s":>24}{"XNU MB/s":>24}{"ratio":>8}{"p99 us L/X":>14}')
    worst = None
    for k in keys:
        lm, xm = [r[k][0] for r in linux], [r[k][0] for r in xnu]
        ratio = statistics.mean(xm) / statistics.mean(lm)
        worst = ratio if worst is None else min(worst, ratio)
        lp, xp = statistics.mean(r[k][1] for r in linux), statistics.mean(r[k][1] for r in xnu)
        name = f'{k[0]} {k[1] // 1024}K qd{k[2]}'
        print(f'{name:<16}{statistics.mean(lm):>9.0f} ({min(lm):.0f}..{max(lm):.0f}){statistics.mean(xm):>12.0f} ({min(xm):.0f}..{max(xm):.0f})'
              f'{ratio:>8.3f}{lp:>8.0f}/{xp:<6.0f}')
    if worst is not None:
        print(f'worst ratio {worst:.3f}')


if __name__ == '__main__':
    main()
