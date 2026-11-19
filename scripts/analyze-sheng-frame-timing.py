#!/usr/bin/env python3
"""Analyze existing UFS text snapshots offline; never operates the device.

The K records' historical `gpu=` field is CPU commit-to-completion-callback time,
not GPU timestamp-query time. W records report swap submission/copy completion,
not panel presentation. Sparse ring snapshots and idle gaps cannot qualify FPS.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import re
import statistics

COMMAND = re.compile(r'^K(\d+) t=(\d+)\.(\d{3})s rp(\d+) dr(\d+) kpx(\d+) cs(\d+) bl(\d+)(?: np(\d+) gpu=| ctc=)(\d+)us$', re.M)
SWAP = re.compile(r'^W(\d+) t=(\d+)\.(\d{3})s \+(\d+)ms offer=(-?\d+)ms done=(-?\d+)ms$', re.M)
PHASE = re.compile(r'^E(\d+) age=(\d+)us scheduled=(-?\d+)us$', re.M)


def distribution(values, budget_us=None):
    ordered = sorted(values)
    result = {'samples': len(ordered)}
    if not ordered:
        return result
    result.update(min=min(ordered), median=statistics.median(ordered),
                  p95=ordered[math.ceil(len(ordered) * .95) - 1], max=max(ordered))
    if budget_us is not None:
        result['over_120hz_budget'] = sum(v > budget_us for v in ordered)
    return result


def analyze(text):
    # Byte-decoded snapshots retain CRLF; normalize only the parser's text so the
    # CLI continues to hash the original input bytes.
    text = text.replace('\r\n', '\n')
    # Each snapshot reprints the rings. Last observation wins, including a later callback
    # replacing an initially zero completion time. Never count the reprints as new work.
    commands, swaps = {}, {}
    for m in COMMAND.finditer(text):
        n, sec, ms, passes, draws, kpx, dispatches, blits, pipelines, us = [int(v) if v is not None else None for v in m.groups()]
        commands[n] = dict(commit_ms=sec * 1000 + ms, passes=passes, draws=draws,
                           kpx=kpx, dispatches=dispatches, blits=blits,
                           new_metal_pipeline_states=pipelines, commit_to_callback_us=us,
                           scope='v1_global_deltas' if pipelines is not None else 'v2_per_buffer_api_calls')
    for m in SWAP.finditer(text):
        n, sec, ms, gap, offer, done = map(int, m.groups())
        swaps[n] = dict(submitted_ms=sec * 1000 + ms, reported_gap_ms=gap,
                        offer_ms=offer, copy_done_ms=done)
    budget = 1_000_000 / 120
    phases = {int(m[1]): (int(m[2]), int(m[3])) for m in PHASE.finditer(text)}
    completed = [c for c in commands.values() if c['commit_to_callback_us'] > 0]
    render = [c for c in completed if c['passes'] > 0]
    multipass = [c for c in render if c['passes'] >= 10]
    blit = [c for c in completed if not c['passes'] and c['blits'] > 0]
    def costs(rows):
        return distribution([c['commit_to_callback_us'] for c in rows], budget)
    report = {
        'evidence': 'offline analysis of previously collected hardware text snapshots',
        'budget_us': budget,
        'commands_observed': len(commands),
        'commands_without_callback_time': len(commands) - len(completed),
        'counter_scopes': sorted({c['scope'] for c in commands.values()}),
        'command_age_before_commit_us_including_caller_idle': distribution([p[0] for n, p in phases.items() if n in commands]),
        'commit_to_scheduled_callback_us': distribution([p[1] for n, p in phases.items() if n in commands and p[1] >= 0]),
        'render_counter_records_commit_to_callback_us': costs(render),
        'multipass_counter_records_commit_to_callback_us': costs(multipass),
        'blit_counter_records_commit_to_callback_us': costs(blit),
        'multipass_records_with_no_new_metal_pipeline_state': sum(c['new_metal_pipeline_states'] == 0 for c in multipass),
        'swaps_observed': len(swaps),
        'swap_reported_gap_ms_including_idle': distribution([s['reported_gap_ms'] for s in swaps.values() if s['reported_gap_ms'] > 0]),
        'swap_offer_ms': distribution([s['offer_ms'] for s in swaps.values() if s['offer_ms'] >= 0]),
        'swap_copy_done_ms': distribution([s['copy_done_ms'] for s in swaps.values() if s['copy_done_ms'] >= 0]),
        'qualifies_stable_120fps': False,
        'limitations': [
            'Sparse ring snapshots omit work; sample percentiles are not whole-run percentiles.',
            'v1 K work counts are global deltas between commits on multiple queues; v2 counts belong to each buffer but count API calls, not proven GPU operations.',
            'commit-to-callback includes host submission, queueing and callback latency; it is not pure GPU execution time.',
            'No new Metal pipeline state does not exclude a later Vulkan pipeline variant compile.',
            'Swap gaps include idle time. A copied framebuffer is not evidence of panel vblank or presentation.',
            'No synchronized input/presentation trace or cold/warm shader-variant coverage; this log cannot qualify 120 FPS.',
            'E age includes idle time between command-buffer creation and commit; it is not CPU busy time. scheduled=-1 means no scheduling callback observed.',
        ],
    }
    for label, pattern in [
        ('metal_pipeline_creation', r'^T pipeline n=(\d+) total=(\d+)ms max=(\d+)ms'),
        ('vulkan_pipeline_creation', r'^V vkpipe n=(\d+) total=(\d+)ms max=(\d+)ms'),
    ]:
        found = list(re.finditer(pattern, text, re.M))
        if found:
            n, total, maximum = map(int, found[-1].groups())
            report[label] = dict(count=n, total_ms=total, max_ms=maximum)
    wait = list(re.finditer(r'^\[scanout wait\] calls=(\d+) timeouts=(\d+) asked=(\d+)ms', text, re.M))
    if wait:
        n, timeouts, asked = map(int, wait[-1].groups())
        report['scanout_consumer_wait'] = dict(calls=n, timeouts=timeouts, requested_timeout_ms=asked,
            meaning='consumer waits for an offer; timeout counts alone do not establish delayed offers or vblank causality')
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('snapshots', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    raw = args.snapshots.read_bytes()
    report = analyze(raw.decode(errors='replace'))
    report['input_sha256'] = hashlib.sha256(raw).hexdigest()
    result = json.dumps(report, indent=2) + '\n'
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(result)
    else:
        print(result, end='')
    if not report['commands_observed'] or not report['swaps_observed']:
        parser.exit(1, 'No paired command/swap record classes; cannot analyze timing.\n')


if __name__ == '__main__':
    main()
