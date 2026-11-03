#!/usr/bin/env python3
"""Decode gpu.trace.bin, kernel.latency.bin and frames.trace from read-sheng-log.py's diag/files.

Times are kernel uptime microseconds. The kernel GPU ring holds submission,
retirement, display arrival, fence and latch, build identity and the render
server's sampled thread identities; the frame trace holds the rest.
"""
import argparse
import json
from pathlib import Path
import struct

HEADER = struct.Struct('<IIIIQQ')
RECORD = struct.Struct('<QQII12Q')
FIELDS = {
 1: ('config', 'kind table_mode preemption input_bits output_bits priority_mode'),
 3: ('kernel_build', 'source_id'),
 10: ('submit', 'fence queue ring priority role loaded switching context submitted_us buffers'),
 11: ('retire', 'fence queue ring submitted_us gpu_start_ticks gpu_end_ticks stamped replays'),
 20: ('display', 'request phase pointer surface_object physical slot shown pending queued'),
 21: ('display_surface', 'request physical width height stride format allocation mapped_bytes'),
 22: ('display_fence', 'request reason target submitted retired elapsed_ns ring0 ring1 query_reason repeated_queries'),
 23: ('display_latch', 'request flush elapsed_ns shown pending'),
 36: ('turnip_pass', 'command pass mode width height colors samples flags format swap draws ns'),
 37: ('turnip_gmem_side', 'count site image format linear mutableen tile_swap view_swap view_mutable mrt_buf_info a2d_dest_info resolve_dest_info'),
 44: ('turnip_attachment', 'pass index mode image format view_swap view_mutable tile_mode load_store_unpadded samples mrt_buf_info'),
 63: ('thread', 'thread role name0 name1 name2 name3'),
 65: ('problem', 'part'),
}
LATENCY_RECORD = struct.Struct('<QQII6Q')
LATENCY_FIELDS = {
 3: ('user_exception', 'exception code subcode pc lr sp'),
 4: ('gpu_vote', 'previous_level requested_level reason busy_percent needed_mhz accepted_level'),
 6: ('fault_state','fault fp sp far esr shared_cache_slide'),
 7: ('fault_registers','fault first x0 x1 x2 x3'),
 8: ('fault_frames','fault first pc0 pc1 pc2 pc3'),
 9: ('fault_images','fault shared_cache_slide dyld_info version images main_image'),
 10: ('fault_stack','fault first word0 word1 word2 word3'),
 11: ('fault_codes','fault exception code subcode pc lr'),
 12: ('fault_result','fault flags error frames delay_us stack_error'),
 14: ('task_identity','task task_pid name0 name1 name2 name3'),
 16: ('gpu_counter_config','counter select_reg countable low_reg generation selector_readback'),
 17: ('gpu_counter_values','sample first v0 v1 v2 v3'),
 18: ('gpu_counter_interval','sample began_us ended_us busy_ticks flags epoch'),
 19: ('gpu_counter_validity','sample selector_mismatch unstable reset epoch_before epoch_after'),
 22: ('signal_exit','subject_pid status reason_namespace reason_code current_thread_owner unused'),
 23: ('signal_description','subject_pid status part text0 text1 text2'),
 24: ('exit_process_name','subject_pid unused0 unused1 text0 text1 text2'),
 25: ('fault_cache_uuid','fault cache_base uuid0 uuid1 main_image unused'),
 26: ('fault_image_uuid','fault image uuid0 uuid1 index unused'),
 27: ('fault_uuid_result','fault total captured error limit unused'),
 28: ('thread_wait','capture state wait pc lr switches'),
 29: ('thread_wait_frames','capture first pc0 pc1 pc2 pc3'),
 30: ('thread_wait_peer','capture peer_pid peer_status stack_error unused0 unused1'),
 31: ('thread_wait_coverage','subject_pid visited recorded omitted lock_busy capacity'),
 32: ('thread_wait_kernel','capture first pc0 pc1 pc2 pc3'),
 34: ('capture_budget','normal_total normal_kept critical_total critical_kept unused0 unused1'),
 35: ('unnest','unnest start end pc lr sp'),
 36: ('unnest_kernel','unnest first_depth pc0 pc1 pc2 pc3'),
}
RUNTIME_STAGES = {40:'memory_begin',41:'memory_pool',42:'memory_kib',43:'available_kib',
 44:'allocated',45:'reused',46:'memory_low',47:'memory_high',48:'cached_kib',49:'deferred_kib',
 50:'free_begin',51:'free_end',52:'release_queue_full',53:'complete_begin',54:'complete_state',
 55:'complete_handlers',56:'complete_retained',57:'complete_end'}

CPU_DETAIL_KINDS=dict(enumerate(('encoding','closure_release','completed_callback','buffer','texture','owner_deallocator','surface_release','function','library','compute_pipeline','render_pipeline','sampler','depth_stencil','event','fence','encoder','command_buffer','queue','heap','binary_archive','memory_owner'),1))
FRAME_RECORD=struct.Struct('<II4Q8I')
def frontend_details(blob):
    if len(blob)%FRAME_RECORD.size:raise ValueError('truncated frontend stream')
    rows=[]
    for at in range(0,len(blob),FRAME_RECORD.size):
        kind,seq,t0,t1,t2,t3,a,b,c,d,tid,cpu0,cpu1,tid2=FRAME_RECORD.unpack_from(blob,at)
        if kind==1:
            rows.append(dict(name='command_summary',sequence=seq,created_us=t0,committed_us=t1,scheduled_us=t2,completed_us=t3,
                             passes=a,draws=b,kpx=c,blits=d,creator_thread=tid,creator_cpu_begin=cpu0,creator_cpu_commit=cpu1,commit_thread=tid2))
        elif kind==7:
            rows.append(dict(name='gpu_pass',us=t0,sequence=t1,width=a&65535,height=a>>16,mode=b&255,
                             colors=(b>>8)&255,flags=(b>>16)&255,draws=c&65535,format=c>>16,kpx=d,
                             gpu_begin_us=t2,gpu_end_us=t3,gpu_elapsed_us=t3-t2 if t3>=t2 and t2 else None,
                             gpu_clock='hardware counter; CPU alignment requires calibration'))
        elif kind==8:
            rows.append(dict(name='kernel_submit',us=t0,sequence=t1,fence=a,sync_wait_us=b,call_us=c,space_wait_us=d,busy=cpu0&65535))
        elif kind==12:
            rows.append(dict(name='runtime_stage',us=t0,runtime=t1,stage=a,argument=b,stage_sequence=c))
        elif kind==14:
            rows.append(dict(name='encode_batch',us=t0,batch=t1,queued_us=t2,phase=a,tasks=b,depth=c,thread=tid))
        elif kind==15:
            rows.append(dict(name='frontend_stage',us=t0,command=t1,commit=t2,batch=t3,phase=a,thread=b,surfaces=c,kib=d))
        elif kind==17:
            rows.append(dict(name='cpu_detail',sequence=seq,us=t0,operation=t1,command=t2,object=t3,
                             kind=a&65535,detail_name=CPU_DETAIL_KINDS.get(a&65535,'unknown'),phase=a>>16,thread=b,function=c|(d<<32),elapsed_us=cpu0,line=cpu1))
        elif kind==18:rows.append(dict(name='cpu_detail_link',us=t0,operation=t1,parent=t2,queued_us=t3,began_us=a|(b<<32)))
        elif kind==22:rows.append(dict(name='runtime_stage_wide',us=t0,runtime=t1,stage=a,argument=b))
        elif kind==13:
            rows.append(dict(name='gpu_pass_detail',us=t0,sequence=t1,fs_instructions=a&65535,fs_texture=a>>16,fs_sy=b&65535,fs_ss=b>>16,
                             fs_waves=c&255,fs_gprs=(c>>8)&255,samples=(c>>16)&255,depth_stencil=c>>24,bandwidth=d,vertices=tid,fs_hash=cpu0))
        elif kind==16:
            rows.append(dict(name='attachment',us=t0,command=t1,texture=t2,resolve=t3,pass_index=a,attachment=b,load=c&255,store=(c>>8)&255,
                             samples=c>>16,pixel_format=d,width=tid,height=cpu0,surface=cpu1,original_samples=tid2))
        elif kind==19:
            raw=struct.pack('<3Q2I',t1,t2,t3,b,c)
            rows.append(dict(name='cpu_definition',key=t0,part=a,text=raw.split(b'\0')[0].decode('utf-8',errors='replace')))
    return rows

GPU_COUNTER_NAMES=('cp_clocks','uche_arbiter_stall','uche_latency_cycles','uche_latency_samples',
 'uche_tp_read_beats','uche_sp_read_beats','uche_tp_requests','tp_busy','tp_l1_requests','tp_l1_misses',
 'sp_busy','sp_alu_working','sp_tp_stall','sp_fs_full_alu','sp_icache_requests','sp_icache_misses',
 'sp_pixels','cmp_read_data','cmp_write_data')

def latency_analysis(rows):
    counter_parts,previous,counters={},None,[]
    faults={};waits={};coverage=[];unnests={}
    for row in rows:
        kind,pid,us,f=row['kind'],row['pid'],row['us'],row['fields']
        if kind in (35,36):
            unnest=unnests.setdefault((pid,f['unnest']),dict(pid=pid,thread=row['thread'],unnest=f['unnest'],kernel={}))
            if kind==35:unnest.update(us=us,**{k:v for k,v in f.items() if k!='unnest'})
            else:
                first=f['first_depth']&255;unnest['kernel_depth']=f['first_depth']>>8
                unnest['kernel'].update((first+i,f['pc'+str(i)]) for i in range(4))
            continue
        if kind==17:
            part=counter_parts.setdefault(f['sample'],dict(values={}))
            part['values'].update((f['first']+i,f['v'+str(i)]) for i in range(4) if f['first']+i<len(GPU_COUNTER_NAMES))
        elif kind==18:counter_parts.setdefault(f['sample'],dict(values={}))['interval']=f
        elif kind==19:
            part=counter_parts.pop(f['sample'],dict(values={}));interval=part.get('interval')
            complete=interval and len(part['values'])==len(GPU_COUNTER_NAMES)
            result=dict(sample=f['sample'],scope='global_br',valid=False,validity=f,interval=interval)
            if complete and previous and previous['valid_end']:
                valid=(not interval['flags'] and not (f['selector_mismatch']|f['unstable']|f['reset']) and
                       f['epoch_before']==previous['epoch'] and f['epoch_after']==f['epoch_before'])
                elapsed=interval['ended_us']-previous['ended_us']
                deltas=[part['values'][i]-previous['values'][i] for i in range(len(GPU_COUNTER_NAMES))]
                valid=valid and elapsed>0 and min(deltas)>=0
                if valid:
                    d=dict(zip(GPU_COUNTER_NAMES,deltas));result.update(valid=True,elapsed_us=elapsed,delta=d,
                        cp_clocks_per_us=d['cp_clocks']/elapsed,tp_sp_read_bytes=32*(d['uche_tp_read_beats']+d['uche_sp_read_beats']))
            if complete:previous=dict(values=part['values'],epoch=f['epoch_after'],ended_us=interval['ended_us'],
                                      valid_end=not(interval['flags']&~1) and not(f['selector_mismatch']|f['unstable']))
            else:previous=None
            counters.append(result)
        elif kind in (6,7,8,9,10,11,12,25,26,27):
            if (pid,f['fault']) in unnests and kind in (8,9,12,25,26,27):
                unnest=unnests[pid,f['fault']]
                if kind==8:unnest.setdefault('frames',{}).update((f['first']+i,f['pc'+str(i)]) for i in range(4))
                elif kind==26:unnest.setdefault('images',[]).append(dict(**f,uuid=row.get('uuid')))
                else:unnest[row['name']]=dict(**f,uuid=row['uuid']) if 'uuid' in row else f
                continue
            fault=faults.setdefault((pid,f['fault']),dict(pid=pid,fault=f['fault'],registers={},frames={},stack={}))
            if kind in (7,8,10):
                target= fault['registers' if kind==7 else 'frames' if kind==8 else 'stack']
                prefix='x' if kind==7 else 'pc' if kind==8 else 'word'
                target.update((f['first']+i,f[prefix+str(i)]) for i in range(4))
            elif kind==26:fault.setdefault('images',[]).append(dict(**f,uuid=row.get('uuid')))
            elif kind==25:fault[row['name']]=dict(**f,uuid=row.get('uuid'))
            else:fault[row['name']]=f
        elif kind in (28,29,30,32):
            wait=waits.setdefault((pid,f['capture']),dict(pid=pid,thread=row['thread'],capture=f['capture'],frames={},kernel={}))
            if kind in (29,32):wait['frames' if kind==29 else 'kernel'].update((f['first']+i,f['pc'+str(i)]) for i in range(4))
            else:wait[row['name']]=f
        elif kind==31:coverage.append(dict(us=us,**f))
    return dict(gpu_counters=counters,faults=list(faults.values()),unnests=list(unnests.values()),
                waits=list(waits.values()),wait_coverage=coverage)

def touch_windows(blob,gap_us=250000,tail_us=500000):
    """Windows of touch activity from kernel.touch.bin: reports closer than gap_us (or the tail) join, each window runs tail_us past its last report.
    The latency analysis covers these windows only."""
    if len(blob)<16:raise ValueError('short touch header')
    magic,total,kept,_=struct.unpack_from('<4I',blob)
    if magic!=0x31484354 or len(blob)!=16+kept*8:raise ValueError('invalid touch stream')
    times=[t for (t,) in struct.iter_unpack('<Q',blob[16:])]
    windows=[]
    for t in times:
        if windows and t-windows[-1][1]<max(gap_us,tail_us):windows[-1][1]=t
        else:windows.append([t,t])
    return dict(total=total,kept=kept,first_us=times[0] if times else None,
                windows=[dict(begin_us=b,end_us=e+tail_us) for b,e in windows])

def in_windows(us,windows):
    return any(w['begin_us']<=us<=w['end_us'] for w in windows)

def latency_records(blob):
    if len(blob)<16:raise ValueError('short latency header')
    magic,total,kept,dropped=struct.unpack_from('<4I',blob)
    if magic!=0x3154414c or kept>524288 or len(blob)!=16+kept*LATENCY_RECORD.size:
        raise ValueError('invalid latency stream')
    rows=[]
    for at in range(16,len(blob),LATENCY_RECORD.size):
        us,tid,kind,pid,*values=LATENCY_RECORD.unpack_from(blob,at)
        name,names=LATENCY_FIELDS.get(kind,('unknown',''))
        row=dict(us=us,thread=tid,kind=kind,pid=pid,name=name,fields=dict(zip(names.split(),values)))
        if kind not in LATENCY_FIELDS:row['raw']=values
        if kind in (23,24):row['text']=struct.pack('<3Q',*values[3:]).split(b'\0')[0].decode('utf-8',errors='replace')
        if kind in (25,26):row['uuid']=struct.pack('<2Q',*values[2:4]).hex()
        if kind==14:row['text']=struct.pack('<4Q',*values[2:]).split(b'\0')[0].decode('utf-8',errors='replace')
        rows.append(row)
    rows.sort(key=lambda r:r['us'])
    result=dict(total=total,kept=kept,dropped=dropped,overwritten=max(0,total-524288))
    summary=next((r['fields'] for r in reversed(rows) if r['kind']==34),None)
    if summary:
        result.update(capture_budget=summary,
                      overwritten=max(0,summary['normal_total']-491519)+max(0,summary['critical_total']-32768),
                      not_retained=summary['normal_total']+summary['critical_total']-summary['normal_kept']-summary['critical_kept'])
    return result,rows



def records(blob):
    if len(blob) < HEADER.size:
        raise ValueError('truncated GPU trace header')
    magic, version, size, count, issued, dropped = HEADER.unpack_from(blob)
    if (magic, version, size) != (0x31545047, 1, RECORD.size):
        raise ValueError('unsupported GPU trace ABI')
    if count > 65536 + 4096 or len(blob) != HEADER.size + count * size:
        raise ValueError('invalid GPU trace length')
    seen = set()
    rows = []
    for offset in range(HEADER.size, len(blob), size):
        seq, us, event, pid, *values = RECORD.unpack_from(blob, offset)
        key = (seq, us, event, pid, *values)
        if key in seen:
            continue
        seen.add(key)
        name, names = FIELDS.get(event, ('unknown', ''))
        row = dict(sequence=seq, us=us, event=event, name=name, pid=pid,
                   fields=dict(zip(names.split(), values)), raw=values)
        if event == 63:
            row['text'] = struct.pack('<4Q', *values[2:6]).split(b'\0')[0].decode('utf-8', errors='replace')
        if event == 65:
            row['text'] = struct.pack('<11Q', *values[1:12]).split(b'\0')[0].decode('utf-8', errors='replace')
        rows.append(row)
    rows.sort(key=lambda row: (row['us'], row['sequence']))
    return dict(issued=issued, dropped=dropped, records=len(rows)), rows


def decode(directory, out):
    out.mkdir(parents=True, exist_ok=True)
    summary = {}
    rows = []
    trace = directory / 'gpu.trace.bin'
    if trace.exists():
        summary['trace'], rows = records(trace.read_bytes())
        (out / 'events.jsonl').write_text(''.join(json.dumps(row)+'\n' for row in rows))
    (out / 'summary.json').write_text(json.dumps(summary, indent=2)+'\n')
    return summary


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--out', type=Path)
    args = parser.parse_args()
    result = decode(args.directory, args.out or args.directory.parent / 'gpu')
    print(json.dumps(result.get('trace', {})))
