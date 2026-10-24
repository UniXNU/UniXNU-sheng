#!/usr/bin/env python3
"""Real host GPU namespace isolation; no guest/system/device qualification."""
import argparse
import hashlib
import json
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import time
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--function-constants',action='store_true');p.add_argument('--queue-priority',action='store_true');p.add_argument('--disconnect-control',action='store_true');p.add_argument('--enqueue',action='store_true');p.add_argument('--rgba',action='store_true');p.add_argument('--private-texture',action='store_true');p.add_argument('--depth-stencil',action='store_true');p.add_argument('--combined-depth-stencil',action='store_true');p.add_argument('--unfinished-encoder-control',action='store_true');p.add_argument('--ended-encoder-control',action='store_true');a=p.parse_args()
if a.combined_depth_stencil:a.depth_stencil=True
if sum((a.function_constants,a.disconnect_control,a.unfinished_encoder_control,a.ended_encoder_control,a.queue_priority,a.enqueue,a.rgba,a.private_texture,a.depth_stencil))>1:p.error('Select one independent qualification')
ROOT=Path(__file__).resolve().parents[1];OUT=ROOT/('build/host-metal-function-constants' if a.function_constants else 'build/host-metal-ended-encoder' if a.ended_encoder_control else 'build/host-metal-unfinished-encoder' if a.unfinished_encoder_control else 'build/host-metal-depth-stencil-combined' if a.combined_depth_stencil else 'build/host-metal-depth-stencil' if a.depth_stencil else 'build/host-metal-private-texture' if a.private_texture else 'build/host-metal-rgba-sessions' if a.rgba else 'build/host-metal-enqueue' if a.enqueue else 'build/host-metal-queue-priority' if a.queue_priority else 'build/host-metal-disconnect' if a.disconnect_control else 'build/host-metal-sessions');OUT.mkdir(exist_ok=True)
def require(v,s):
    if not v:raise SystemExit(s)
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
base=ROOT/'.cache/firmware/target-volumes/os-mount/System/Library'
paths=[base/'Frameworks/QuartzCore.framework/default.metallib',base/'PrivateFrameworks/RenderBox.framework/default.metallib']
require([sha(p) for p in paths]==['606f913adf520420f9b4bb666efb782a68c830f082f1e559e17b20d0d13adbc9','d76883f626b1527c36fb0940cf27dcade503a55cf2fa05c8f7a247a40ccebec4'],'Exact original libraries')
exe=ROOT/'build/host-metal/host-metal-originals';build=json.loads((exe.parent/'qualification.json').read_text())
require(sha(exe)==build['process_sha256'] and sha(ROOT/'kits/sheng/HostMetalDiagnostic.mm')==build['source_sha256'],'Exact compiled real GPU host')
for name,digest in build['backend_sources'].items():require(sha(ROOT/name)==digest,'Exact compiled backend '+name)
def words(*v):return struct.pack('<'+'Q'*len(v),*v)
requests={};observed=[]
with tempfile.TemporaryDirectory(prefix='metal-sessions-',dir='/tmp') as directory:
    path=Path(directory)/'gpu.sock'
    with (OUT/'host-report.json').open('w') as output,(OUT/'host-stderr.log').open('w') as errors:
        host=subprocess.Popen([str(exe),*map(str,paths),'--metal-session-socket',str(path)],stdout=output,stderr=errors)
        try:
            deadline=time.monotonic()+15
            while not path.exists():
                require(host.poll() is None and time.monotonic()<deadline,'Host GPU startup failed');time.sleep(.02)
            with socket.socket(socket.AF_UNIX,socket.SOCK_STREAM) as channel:
                channel.settimeout(10);channel.connect(str(path))
                def read(n):
                    b=bytearray()
                    while len(b)<n:
                        p=channel.recv(n-len(b));require(p,'Actual framed host reply ended');b+=p
                    return bytes(b)
                def call(session,operation,payload=b'',error=0,sequence=None):
                    expected=requests.get(session,1)
                    number=expected if sequence is None else sequence
                    if session and sequence is None:requests[session]=expected+1
                    channel.sendall(struct.pack('<IHHIIQ',0x5550474e,3,operation,len(payload),number,session)+payload)
                    magic,version,op,size,actual,sid=struct.unpack('<IHHIIQ',read(24))
                    require((magic,version,op,actual,sid)==(0x5550474e,3,operation|0x8000,number,session) and 16<=size<=16*1024*1024+16,'Actual session reply identity and bounded size')
                    status,reserved,handle=struct.unpack('<iIQ',read(16));body=read(size-16)
                    require(status==error and not reserved,'Real operation result differs')
                    observed.append(dict(session=session,request=number,operation=operation,error=status,handle=handle))
                    return handle,body
                if a.disconnect_control or a.unfinished_encoder_control or a.ended_encoder_control:
                    sid=call(0,0x140)[0];require(json.loads(call(sid,0x100)[1])['name'],'Real device queried before actual disconnect')
                    if a.unfinished_encoder_control or a.ended_encoder_control:
                        queue=call(sid,0x101)[0];texture=call(sid,0x104,words(2,80,64,64,1,1,1,1,5,0,0))[0]
                        command=call(sid,0x106,words(queue))[0];require(call(sid,0x121,words(command))[1]==words(1),'Actual interrupted command really Enqueued')
                        payload=words(command)+struct.pack('<3Q4d',texture,2,1,.2,.4,.6,1)+bytes(7*56)
                        encoder=call(sid,0x107,payload)[0];require(encoder,'Actual active render encoder')
                        if a.unfinished_encoder_control:
                            call(sid,0x10c,words(command),error=-2);call(sid,0x117,words(command),error=-2)
                        else:
                            call(sid,0x10b,words(encoder));call(sid,0x10b,words(encoder),error=-2)
                            call(sid,0x10a,words(encoder,4,0,4,1),error=-2);call(sid,0x109,words(encoder,0),error=-2)
                            call(sid,0x125,words(encoder,1,1),error=-2)
                            blit=call(sid,0x117,words(command))[0];call(sid,0x10b,words(blit))
                            source=call(sid,0x111,words(64,0))[0];destination=call(sid,0x111,words(64,0))[0]
                            call(sid,0x118,words(blit,source,0,destination,0,64),error=-2)
                    channel.close();require(host.wait(timeout=10)==2,'Incomplete real transport must exit with failure')
                    d=json.loads((OUT/'host-report.json').read_text())
                    require(not d['transport_healthy'] and not d['session_finished'] and d['opened_sessions']==1 and
                        not d['closed_sessions'] and len(d['operations'])==(17 if a.ended_encoder_control else 9 if a.unfinished_encoder_control else 2) and not d['completed_commands'] and not d['texture_readbacks'],
                        'Actual incomplete ledger retains the two real operations without fabricating GPU completion')
                    if a.unfinished_encoder_control:
                        aborted=d['aborted_encoders'];require(len(aborted)==1 and not d['open_encoders'] and d['pending_enqueues']==1 and
                            aborted[0]['encoder']==encoder and aborted[0]['command']==command and aborted[0]['actual_command_status']==1 and
                            aborted[0]['encoding_ended'] and not aborted[0]['gpu_submission_created_by_cleanup'] and aborted[0]['reason']=='transport-lost',
                            'Actual abandoned encoder ends without any invented GPU submission or completed frame')
                    if a.ended_encoder_control:
                        require(not d['aborted_encoders'] and not d['open_encoders'] and d['pending_enqueues']==1 and
                            sum(bool(o['error']) for o in d['operations'])==5, 'All five ended-encoder operations refuse before host APIs, without implicit submission')
                    (OUT/('ended-encoder-checkpoint.json' if a.ended_encoder_control else 'unfinished-encoder-checkpoint.json' if a.unfinished_encoder_control else 'disconnect-checkpoint.json')).write_text(json.dumps(dict(process_sha256=sha(exe),report_sha256=sha(OUT/'host-report.json'),
                        incomplete_ledger_preserved=True,actual_failure_exit=2,aborted_encoder_without_submission=a.unfinished_encoder_control,ended_encoder_calls_refused=a.ended_encoder_control,original_desktop=False,adreno_execution=False),indent=2)+'\n')
                    print('PASS: five actual ended render/blit encoder calls refuse safely, with no implicit GPU submission' if a.ended_encoder_control else 'PASS: actual abandoned active encoder ends without submission; complete partial ledger retained and failure returned' if a.unfinished_encoder_control else 'PASS: real client disconnect retains its exact incomplete ledger and returns failure, without completed GPU commands')
                    raise SystemExit(0)
                first=call(0,0x140)[0];second=call(0,0x140)[0]
                require(first and second and first!=second,'Actual independent session identities')
                for sid in (first,second):require(json.loads(call(sid,0x100)[1])['name'],'Actual host device metadata')
                buffer=call(first,0x111,words(64,0))[0];require(buffer,'Actual first-session GPU buffer')
                data=bytes(range(64));call(first,0x112,words(buffer,0,64)+data)
                call(second,0x113,words(buffer,0,64),error=-2)
                call(second,0x10e,words(buffer),error=-2)
                call(0x10000,0x113,words(buffer,0,64),error=-2)
                call(first,0x113,words(buffer,0,64),error=-2,sequence=1)
                require(call(first,0x113,words(buffer,0,64))[1]==data,'Refusals preserve actual owner buffer')
                queue1=call(first,0x101)[0];queue2=call(second,0x101)[0]
                if a.function_constants:
                    library=call(first,0x102,bytes.fromhex('d76883f626b1527c36fb0940cf27dcade503a55cf2fa05c8f7a247a40ccebec4'))[0]
                    def specialized(name,rows,handle=library,count=None):
                        return words(handle)+name.encode().ljust(128,b'\0')+words(len(rows) if count is None else count)+b''.join(rows)
                    def constant(value=(2,0,5502022,4),named=False,kind=36,size=16,index=0):
                        return words(kind,index,size,int(named))+('RB::Shader::Constant::_shader_state'.encode() if named else b'').ljust(128,b'\0')+struct.pack('<4I',*value).ljust(32,b'\0')
                    vertex=call(first,0x103,specialized('plane__vertex',[constant()]))[0]
                    call(second,0x103,specialized('plane__vertex',[constant()]),error=-2)
                    call(first,0x103,specialized('plane_color_fragment',[constant(size=8)]),error=-2)
                    call(first,0x103,specialized('plane_color_fragment',[constant(kind=5,size=12)]),error=-2)
                    call(first,0x103,specialized('plane_color_fragment',[constant(index=65536)]),error=-2)
                    call(first,0x103,specialized('plane_color_fragment',[constant(),constant()]),error=-2)
                    call(first,0x103,specialized('plane_color_fragment',[],count=17),error=-2)
                    call(first,0x103,specialized('x'*128,[constant()]),error=-2)
                    call(first,0x103,specialized('plane_color_fragment',[constant()])[:-1],error=-2)
                    call(first,0x103,specialized('__native_missing__',[constant()]),error=-4)
                    rgba=call(first,0x104,words(2,70,64,64,1,1,1,1,5,0,0))[0]
                    coverage=call(first,0x104,words(2,65,64,64,1,1,1,1,5,2,0))[0]
                    globals_data=struct.pack('<7f4e',2,0,0,2,-1,-1,0,1,0,0,1)
                    globals_buffer=call(first,0x111,words(64,0))[0];call(first,0x112,words(globals_buffer,0,len(globals_data))+globals_data)
                    indices=struct.pack('<6H',0,1,2,0,2,3);index_buffer=call(first,0x111,words(16,0))[0];call(first,0x112,words(index_buffer,0,len(indices))+indices)
                    for row,pixel in ((constant(),0xff0000ff),(constant(named=True),0xff0000ff),(constant(value=(2,0,5502022|(1<<26),4)),0xffffffff)):
                        fragment=call(first,0x103,specialized('plane_color_fragment',[row]))[0]
                        pipeline=call(first,0x105,words(vertex,fragment,1,70,65,*([0]*6),*([15]*8)))[0]
                        command=call(first,0x106,words(queue1))[0]
                        render=words(command)+struct.pack('<3Q4d',rgba,2,1,.2,.4,.6,1)+struct.pack('<3Q4d',coverage,2,1,0,0,0,0)+bytes(6*56)
                        encoder=call(first,0x107,render)[0];call(first,0x108,words(encoder,pipeline));call(first,0x109,words(encoder,0))
                        call(first,0x114,words(encoder,globals_buffer,0,0));call(first,0x115,words(encoder,globals_buffer,0,0));call(first,0x116,words(encoder,3,6,0,index_buffer,0,1,0,0))
                        call(first,0x10b,words(encoder));status,start,end=struct.unpack('<3Q',call(first,0x10c,words(command))[1])
                        require(status==4 and 0<start<end,'Actual specialized original shader command completes on GPU')
                        pixels=call(first,0x10d,words(rgba,0,0,64,64,0))[1]
                        require(pixels==struct.pack('<I',pixel)*4096,
                            'Actual specialized GPU pixel values differ: '+repr(sorted({hex(v) for v in struct.unpack('<4096I',pixels)})))
                if a.queue_priority:
                    def priority(sid,queue,background=0):
                        body=call(sid,0x11f,words(queue,background,0))[1]
                        require(len(body)==8,'Actual private queue getter body');return struct.unpack('<Q',body)[0]
                    before=priority(first,queue1);background=priority(first,queue1,1);peer=priority(second,queue2)
                    alternate=0 if before else 1
                    result=call(first,0x120,words(queue1,0,alternate))[1]
                    require(result==words(1) and priority(first,queue1)==alternate,'Real host foreground setter succeeds and actual getter observes the changed priority')
                    require(priority(second,queue2)==peer,'A real priority change preserves the peer queue')
                    require(call(first,0x120,words(queue1,0,before))[1]==words(1) and priority(first,queue1)==before,'Real original foreground priority restored')
                    # Exercise the real background setter at its actual current value.
                    require(call(first,0x120,words(queue1,1,background))[1]==words(1) and priority(first,queue1,1)==background,'Actual host background setter/getter, without claiming a change')
                    call(second,0x11f,words(queue1,0,0),error=-2)
                    call(first,0x11f,words(buffer,0,0),error=-2)
                    call(first,0x11f,words(queue1,2,0),error=-2)
                    call(first,0x11f,words(queue1,0,1),error=-2)
                    call(first,0x120,words(queue1,0,6),error=-4)
                    call(first,0x120,words(queue1,0,3),error=-4)
                texture1=call(first,0x104,words(2,70 if a.rgba else 80,64,64,1,1,1,1,5,0,0))[0]
                texture2=call(second,0x104,words(2,70 if a.rgba else 80,64,64,1,1,1,1,5,0,0))[0]
                command1=call(first,0x106,words(queue1))[0];command2=call(second,0x106,words(queue2))[0]
                if a.enqueue:
                    call(second,0x121,words(command1),error=-2)
                    call(first,0x121,words(buffer),error=-2)
                    call(first,0x121,b'',error=-2)
                    require(call(first,0x121,words(command1))[1]==words(1) and call(second,0x121,words(command2))[1]==words(1),
                        'Both actual host commands enter Enqueued, not Completed')
                    call(first,0x121,words(command1),error=-2)
                    later=call(first,0x106,words(queue1))[0]
                    call(first,0x121,words(later),error=-4)
                    call(first,0x10c,words(later),error=-4)
                def render(command,texture,color):return words(command)+struct.pack('<3Q4d',texture,2,1,*color)+bytes(7*56)
                if a.private_texture:
                    private=call(first,0x104,words(2,65,64,64,1,1,1,1,5,2,0))[0]
                    shared=call(first,0x104,words(2,65,64,64,1,1,1,1,5,0,0))[0]
                    call(first,0x110,words(private,0,0,64,1,0,256)+bytes(256),error=-2)
                    call(first,0x10d,words(private,0,0,64,64,0),error=-2)
                    cm=call(first,0x106,words(queue1))[0];require(call(first,0x121,words(cm))[1]==words(1),'Actual private-render command enqueued')
                    for texture,color in ((private,(.25,.75,0,1)),(shared,(.75,.25,0,1))):
                        encoder=call(first,0x107,render(cm,texture,color))[0];call(first,0x10b,words(encoder))
                    blit=call(first,0x117,words(cm))[0]
                    def copy(source,destination,x=0):return words(blit,source,destination,0,0,x,0,0,64,64,1,0,0,0,0,0)
                    call(second,0x122,copy(private,shared),error=-2)
                    call(first,0x122,copy(private,shared,63),error=-2)
                    call(first,0x122,copy(private,private),error=-2)
                    call(first,0x122,copy(private,texture1),error=-2)
                    call(first,0x122,copy(private,shared));call(first,0x10b,words(blit))
                    status,start,end=struct.unpack('<3Q',call(first,0x10c,words(cm))[1]);require(status==4 and 0<start<end,'Real private render/copy completed on GPU')
                    require(call(first,0x10d,words(shared,0,0,64,64,0))[1]==struct.pack('<I',0x3a003400)*4096,
                        'All private half-float GPU pixels replace a distinct real destination clear through a GPU blit')
                if a.depth_stencil:
                    color=call(first,0x104,words(2,70,64,64,1,1,1,1,5,0,0))[0]
                    stencil=call(first,0x104,words(2,260 if a.combined_depth_stencil else 253,64,64,1,1,1,1,4,2,0))[0]
                    depth=stencil if a.combined_depth_stencil else call(first,0x104,words(2,252,64,64,1,1,1,1,4,2,0))[0]
                    call(first,0x104,words(2,253,64,64,1,1,1,1,4,0,0),error=-4)
                    library=call(first,0x102,bytes.fromhex('606f913adf520420f9b4bb666efb782a68c830f082f1e559e17b20d0d13adbc9'))[0]
                    def function(name):return call(first,0x103,words(library)+name.encode().ljust(128,b'\0'))[0]
                    vertex=function('read_surf_vert');fragment=function('attachment_clear_frag_lpf')
                    pipeline=call(first,0x126,words(vertex,fragment,1,*([70]+[0]*7),*([15]*8))+words(0,1,0,0,1,0,0)*8+words(260 if a.combined_depth_stencil else 252,260 if a.combined_depth_stencil else 253))[0]
                    face=[1,2,0,0,0,255,0]
                    always=call(first,0x123,words(7,0,*face,*face))[0];less=call(first,0x123,words(1,0,*face,*face))[0]
                    call(first,0x123,words(8,0,*face,*face),error=-2);call(first,0x123,words(7,2,*face,*face),error=-2)
                    for test in range(4):
                        cm=call(first,0x106,words(queue1))[0];require(call(first,0x121,words(cm))[1]==words(1),'Actual depth/stencil command enqueued')
                        colors=words(cm)+struct.pack('<3Q4d',color,1 if test&1 else 2,1,.2,.4,.6,1)+bytes(7*56)
                        attachments=colors+words(depth,2,1)+struct.pack('<d',0 if test==2 else 1)+words(stencil,1 if test else 2,1,1)
                        encoder=call(first,0x127,attachments)[0]
                        if not test:
                            call(second,0x124,words(encoder,always),error=-2)
                            call(first,0x124,words(encoder,color),error=-2)
                            call(first,0x125,words(encoder,1<<32,1),error=-2)
                        call(first,0x108,words(encoder,pipeline));call(first,0x124,words(encoder,always if test<2 else less));call(first,0x109,words(encoder,0))
                        reference=2 if not test else 1;call(first,0x125,words(encoder,reference,reference));call(first,0x10a,words(encoder,4,0,4,1));call(first,0x10b,words(encoder))
                        status,start,end=struct.unpack('<3Q',call(first,0x10c,words(cm))[1]);require(status==4 and 0<start<end,'Actual original-shader depth/stencil GPU completion')
                        require(call(first,0x10d,words(color,0,0,64,64,0))[1]==struct.pack('<I',0 if test&1 else 0xff996633)*4096,
                            'Every GPU pixel agrees with actual stencil mismatch/match or depth reject/pass')
                    call(first,0x10d,words(stencil,0,0,64,64,0),error=-2)
                call(second,0x107,render(command2,texture1,(.2,.4,.6,1)),error=-2)
                encoder1=call(first,0x107,render(command1,texture1,(.2,.4,.6,1)))[0]
                encoder2=call(second,0x107,render(command2,texture2,(.8,.4,.2,1)))[0]
                call(first,0x10b,words(encoder1));call(second,0x10b,words(encoder2))
                for sid,command in ((first,command1),(second,command2)):
                    status,start,end=struct.unpack('<3Q',call(sid,0x10c,words(command))[1])
                    require(status==4 and 0<start<end,'Actual real GPU completion and ordered timestamps')
                if a.enqueue:
                    require(call(first,0x121,words(later))[1]==words(1),'Actual predecessor completed, so next queue reservation now succeeds')
                    encoder=call(first,0x107,render(later,texture1,(.2,.4,.6,1)))[0];call(first,0x10b,words(encoder))
                    status,start,end=struct.unpack('<3Q',call(first,0x10c,words(later))[1]);require(status==4 and 0<start<end,'Actual next enqueued command completes on GPU')
                expected1=struct.pack('<I',0xff996633 if a.rgba else 0xff336699)*4096;expected2=struct.pack('<I',0xff3366cc if a.rgba else 0xffcc6633)*4096
                require(call(first,0x10d,words(texture1,0,0,64,64,0))[1]==expected1,'All real first-session GPU pixels')
                require(call(second,0x10d,words(texture2,0,0,64,64,0))[1]==expected2,'All real second-session GPU pixels')
                call(first,0x10d,words(texture2,0,0,64,64,0),error=-2)
                call(first,0x141)
                call(first,0x113,words(buffer,0,64),error=-2)
                require(call(second,0x10d,words(texture2,0,0,64,64,0))[1]==expected2,'Peer GPU output remains readable after first namespace closes')
                # Actual GPU submission also remains possible after peer close.
                command3=call(second,0x106,words(queue2))[0]
                encoder3=call(second,0x107,render(command3,texture2,(.8,.4,.2,1)))[0]
                call(second,0x10b,words(encoder3));status,start,end=struct.unpack('<3Q',call(second,0x10c,words(command3))[1])
                require(status==4 and 0<start<end,'Peer still executes real GPU commands after first namespace closes')
                require(call(second,0x10d,words(texture2,0,0,64,64,0))[1]==expected2,'All peer pixels after independent lifetime transition')
                call(second,0x141)
            require(host.wait(timeout=10)==0,'Actual host session server failed')
        finally:
            if host.poll() is None:host.terminate();host.wait(timeout=5)
d=json.loads((OUT/'host-report.json').read_text())
require(d['transport_healthy'] and d['session_finished'] and not d['aborted_encoders'] and not d['open_encoders'] and d['isolated_sessions'] and d['opened_sessions']==d['closed_sessions']==2 and not d['cpu_pixel_renderer'] and
        not d['guest_system_device_discovery'] and not d['adreno_execution'] and not d['original_desktop'],'Two closed real host namespaces; offline-only scope')
require(len(d['operations'])==len(observed) and sum(bool(o['error']) for o in d['operations'])==(16 if a.function_constants else 14 if a.depth_stencil else 13 if (a.queue_priority or a.enqueue or a.private_texture) else 7) and
        len(d['completed_commands'])==(6 if a.function_constants else 7 if a.depth_stencil else 4 if (a.enqueue or a.private_texture) else 3) and len(d['texture_readbacks'])==(7 if a.function_constants else 8 if a.depth_stencil else 5 if a.private_texture else 4),'Exact actual operations, refusals, real GPU completions and whole-surface readbacks')
for expected,actual in zip(observed,d['operations']):
    require(all(actual[k if k!='request' else 'client_request']==v for k,v in expected.items()),'Actual server operation ledger matches each independent request')
if a.queue_priority:
    require(len(d['queue_priorities'])==10 and all(r['before']==r['after'] for r in d['queue_priorities'] if not r['writing']), 'Real host priority ledger agrees with getter/setter calls')
    print('PASS: real foreground priority change/restoration, background setter, peer isolation and six additional priority refusals')
if a.function_constants:
    rows=d['function_specializations'];require(len(rows)==5 and sum(r['actual_success'] for r in rows)==4 and
        rows[0]['constants'][0]['data_hex']=='020000000000000046f4530004000000' and
        rows[-2]['constants'][0]['name']=='RB::Shader::Constant::_shader_state' and
        rows[-1]['constants'][0]['data_hex']=='020000000000000046f4530404000000',
        'Actual compiler ledger retains exact indexed/named UInt4 inputs and the bit-changing control')
    print('PASS: real original shader specialization, indexed/named equivalence, whole-surface constant-bit effect and nine genuine refusals')
print('Peer executes after independent close; guest broker/native process isolation/system Metal/Adreno remain open')

if a.enqueue:
    require(len(d['enqueued_commands'])==3 and not d['pending_enqueues'] and all(r['status']==1 for r in d['enqueued_commands']),
        'Three genuine queue reservations, no fake scheduled/completed status and no pending reservation after teardown')
    print('PASS: real enqueue-before-encoding, GPU completion, same-queue predecessor refusal/retry and namespace/duplicate/argument refusals')
if a.private_texture:
    copies=d['texture_copies'];require(len(copies)==1 and copies[0]['format']==65 and copies[0]['source_storage']==2 and
        copies[0]['destination_storage']==0 and copies[0]['width']==copies[0]['height']==64 and
        len(d['enqueued_commands'])==1 and not d['pending_enqueues'], 'Actual private-to-shared whole-texture GPU copy ledger')
    print('PASS: private RG16Float GPU render/copy, exact half-float output and six real CPU-access/copy-isolation/argument refusals')
if a.depth_stencil:
    require(len(d['depth_stencil_states'])==2 and len(d['attachment_passes'])==len(d['stencil_references'])==4 and
        [r['depth_clear'] for r in d['attachment_passes']]==[1,1,0,1] and [r['front'] for r in d['stencil_references']]==[2,1,1,1] and
        len(d['enqueued_commands'])==4 and not d['pending_enqueues'], 'Actual private attachments, GPU states and reference/depth-clear controls')
    print('PASS: unchanged original shader obeys genuine stencil mismatch/match and depth rejection/pass; seven new real resource/state/isolation refusals')
sources=[ROOT/'kits/sheng/HostMetalDiagnostic.mm',*(ROOT/'drivers/AdrenoMetal'/n for n in ('HostMetalCommandServer.mm','HostMetalCommandServer.h','OfflineMetalWire.h','OfflineMetalFunctionWire.h','OfflineMetalQueueWire.h','OfflineMetalTextureWire.h','OfflineMetalDepthStencilWire.h','OfflineMetalSessionWire.h'))]
(OUT/('function-constant-checkpoint.json' if a.function_constants else 'depth-stencil-checkpoint.json' if a.depth_stencil else 'private-texture-checkpoint.json' if a.private_texture else 'rgba-checkpoint.json' if a.rgba else 'enqueue-checkpoint.json' if a.enqueue else 'queue-priority-checkpoint.json' if a.queue_priority else 'checkpoint.json')).write_text(json.dumps(dict(source_sha256={str(p.relative_to(ROOT)):sha(p) for p in sources},
    process_sha256=sha(exe),report_sha256=sha(OUT/'host-report.json'),operations=len(observed),isolated_sessions=2,
    real_gpu_completions=6 if a.function_constants else 7 if a.depth_stencil else 4 if (a.enqueue or a.private_texture) else 3,refusals=16 if a.function_constants else 14 if a.depth_stencil else 13 if (a.queue_priority or a.enqueue or a.private_texture) else 7,actual_function_constants=a.function_constants,actual_queue_priority=a.queue_priority,actual_enqueue=a.enqueue,rgba8_channel_order=a.rgba,private_rg16float_gpu_copy=a.private_texture,actual_gpu_depth_stencil=a.depth_stencil,combined_depth_stencil=a.combined_depth_stencil,guest_kernel_broker=False,native_process_isolation=False,
    active_adreno_4k=False,system_metal=False,original_desktop=False),indent=2)+'\n')
print(f"PASS: two real host GPU namespaces, actual namespace/argument refusals and {len(d['completed_commands'])} real GPU completions with whole-surface pixels")
