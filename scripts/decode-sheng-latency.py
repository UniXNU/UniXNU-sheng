#!/usr/bin/env python3
"""Decode bounded IPC, exception, GPU-vote and touch records into JSONL."""
import argparse,importlib.util,json
from pathlib import Path
spec=importlib.util.spec_from_file_location('gpu_trace',Path(__file__).with_name('decode-gpu-trace.py'))
module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('input',type=Path);parser.add_argument('--out',type=Path,required=True)
    parser.add_argument('--frames',type=Path)
    args=parser.parse_args();head,rows=module.latency_records(args.input.read_bytes())
    args.out.parent.mkdir(parents=True,exist_ok=True)
    with args.out.open('w') as out:
        for row in rows:out.write(json.dumps(row)+'\n')
    args.out.with_suffix('.analysis.json').write_text(json.dumps(module.latency_analysis(rows),indent=2)+'\n')
    if args.frames:
        details=module.frontend_details(args.frames.read_bytes())
        args.out.with_suffix('.frontend.jsonl').write_text(''.join(json.dumps(row)+'\n' for row in details))
        head['frontend_details']=len(details)
    print(json.dumps(head))
