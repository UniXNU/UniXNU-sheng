#!/usr/bin/env python3
"""Extract actual Probe helpers; only ARM barriers become host memory barriers."""
from pathlib import Path
import argparse, hashlib, json

def function(text, signature):
    start = text.index(signature)
    opening = text.index('{', start)
    depth = 0
    state = 'code'
    i = opening
    while i < len(text):
        ch = text[i]
        nxt = text[i:i+2]
        if state == 'line':
            if ch == '\n': state = 'code'
        elif state == 'block':
            if nxt == '*/': state = 'code'; i += 1
        elif state in ('string', 'char'):
            if ch == '\\': i += 1
            elif ch == ('"' if state == 'string' else "'"): state = 'code'
        elif nxt == '//': state = 'line'; i += 1
        elif nxt == '/*': state = 'block'; i += 1
        elif ch == '"': state = 'string'
        elif ch == "'": state = 'char'
        elif ch == '{': depth += 1
        elif ch == '}':
            depth -= 1
            if not depth: return text[start:i+1]
        i += 1
    raise ValueError('Unclosed function: '+signature)

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--probe',type=Path,required=True)
    ap.add_argument('--out',type=Path,required=True)
    args = ap.parse_args()
    source = args.probe
    text = source.read_text(encoding='utf-8')
    first = text.index('struct Stroke {')
    last = text.index('uint32_t touchMs()',first)
    declarations = text[first:last]
    signatures = [
        'void trackStrokes(', 'bool publishAt(', 'static uint64_t touchUs(',
        'static void aggressiveStats(', 'static bool sendTimeline(',
        'static bool captureTimeline(', 'static unsigned inputWaitMs(',
        'static void expireTouchStream(']
    bodies = [function(text,s) for s in signatures]
    barrier = '__asm__ volatile("dmb ish" ::: "memory");'
    assert sum(body.count(barrier) for body in bodies)==2, 'ARM barrier substitution changed'
    host_bodies = [body.replace(barrier,'host_barrier();') for body in bodies]
    output = '// Generated exact actual Probe helper bodies. Do not edit.\n'+declarations
    output += '\nstatic bool timelineResetPending;\n'
    output += '\n\n'.join(host_bodies)+'\n'
    args.out.mkdir(parents=True,exist_ok=True)
    (args.out/'probe_extraction.inc').write_text(output,encoding='utf-8')
    record = dict(source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),
                  output_sha256=hashlib.sha256(output.encode()).hexdigest(),
                  functions=signatures, substitutions={'ARM_dmb_to_host_barrier':2},
                  platform_dependencies='globals/clock/HID ACK/trace sinks stubbed by test; actual Engine/Timeline/Core retained')
    (args.out/'probe-extraction.json').write_text(json.dumps(record,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(record))
if __name__=='__main__': main()
