#!/usr/bin/env python3
"""Bounded checks for deduplication and missing completion evidence in the offline analyzer."""
from pathlib import Path
import runpy

analyze = runpy.run_path(str(Path(__file__).with_name('analyze-sheng-frame-timing.py')))['analyze']
initial = '''K1 t=1.000s rp15 dr140 kpx24273 cs0 bl0 np0 gpu=0us
W1 t=1.000s +0ms offer=1ms done=-1ms
'''
completed = '''K1 t=1.000s rp15 dr140 kpx24273 cs0 bl0 np0 gpu=14000us
K2 t=1.016s rp0 dr0 kpx0 cs0 bl1 np0 gpu=4000us
W1 t=1.000s +0ms offer=1ms done=7ms
W2 t=2.000s +1000ms offer=0ms done=6ms
'''
pending = analyze(initial)
assert pending['commands_without_callback_time'] == 1
assert pending['render_counter_records_commit_to_callback_us']['samples'] == 0
assert pending['swap_copy_done_ms']['samples'] == 0
report = analyze(initial + completed + completed)
assert report['commands_observed'] == 2 and report['swaps_observed'] == 2
assert report['commands_without_callback_time'] == 0
assert report['multipass_counter_records_commit_to_callback_us']['over_120hz_budget'] == 1
assert report['blit_counter_records_commit_to_callback_us']['over_120hz_budget'] == 0
assert report['swap_reported_gap_ms_including_idle']['max'] == 1000
assert report['multipass_records_with_no_new_metal_pipeline_state'] == 1
assert not report['qualifies_stable_120fps']
assert not analyze('')['qualifies_stable_120fps']
v2_text = '''K1 t=1.000s rp1 dr2 kpx512 cs0 bl0 ctc=5000us
E1 age=10000us scheduled=-1us
K2 t=1.010s rp0 dr0 kpx0 cs0 bl1 ctc=4000us
E2 age=200us scheduled=20us
'''
v2 = analyze(v2_text)
assert v2['commands_observed'] == 2 and v2['counter_scopes'] == ['v2_per_buffer_api_calls']
assert v2['command_age_before_commit_us_including_caller_idle']['max'] == 10000
assert v2['commit_to_scheduled_callback_us']['samples'] == 1
assert v2['commit_to_scheduled_callback_us']['max'] == 20
# CRLF and mixed endings must preserve all K/W/E records and callback updates.
for snapshot in (initial, initial + completed + completed, v2_text):
    expected = analyze(snapshot)
    for converted in (snapshot.replace('\n', '\r\n'),
                      snapshot.replace('\n', '\r\n', 1),
                      snapshot.rstrip('\n').replace('\n', '\r\n')):
        assert analyze(converted) == expected
assert analyze(initial + completed.replace('\n', '\r\n')) == analyze(initial + completed)
print('PASS: snapshot reprints deduplicate, missing callbacks remain unqualified, idle gaps preserved, LF/CRLF equivalent, no panel/FPS claim')
