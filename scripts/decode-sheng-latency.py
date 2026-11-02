#!/usr/bin/env python3
"""Decode the latency ring and the frame trace into JSONL; with --touch, also the touch windows and the records inside them."""
import argparse,importlib.util,json
from pathlib import Path
spec=importlib.util.spec_from_file_location('gpu_trace',Path(__file__).with_name('decode-gpu-trace.py'))
module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('input',type=Path);parser.add_argument('--out',type=Path,required=True)
    parser.add_argument('--frames',type=Path)
    parser.add_argument('--touch',type=Path,help='kernel.touch.bin: limit the *.touch.* outputs to touch windows')
    args=parser.parse_args();head,rows=module.latency_records(args.input.read_bytes())
    args.out.parent.mkdir(parents=True,exist_ok=True)
    with args.out.open('w') as out:
        for row in rows:out.write(json.dumps(row)+'\n')
    args.out.with_suffix('.analysis.json').write_text(json.dumps(module.latency_analysis(rows),indent=2)+'\n')
    if args.frames:
        details=module.frontend_details(args.frames.read_bytes())
        args.out.with_suffix('.frontend.jsonl').write_text(''.join(json.dumps(row)+'\n' for row in details))
        head['frontend_details']=len(details)
    if args.touch:
        touch=module.touch_windows(args.touch.read_bytes());windows=touch['windows']
        args.out.with_suffix('.touch-windows.json').write_text(json.dumps(touch,indent=2)+'\n')
        inside=[row for row in rows if module.in_windows(row['us'],windows)]
        args.out.with_suffix('.touch.analysis.json').write_text(json.dumps(module.latency_analysis(inside),indent=2)+'\n')
        if args.frames:
            args.out.with_suffix('.frontend.touch.jsonl').write_text(''.join(json.dumps(row)+'\n' for row in details
                if 'us' in row and module.in_windows(row['us'],windows)))
        head['touch_windows']=len(windows)
    print(json.dumps(head))
