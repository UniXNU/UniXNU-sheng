#!/usr/bin/env python3
"""Exercise the patched XNU event allocation/copy path with explicit mbuf mocks."""
from pathlib import Path
import hashlib,json,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
source=root/'../UniXNU/out/sheng/ipados-27.0.1-24A446/sheng_ipados27/tree/.cache/xnu-work/bsd/kern/kern_event.c'
s=source.read_text()
if 'iPadOS 27 association events carry' not in s:
    with tempfile.TemporaryDirectory() as temporary:
        staged=Path(temporary)/'bsd/kern/kern_event.c';staged.parent.mkdir(parents=True);staged.write_text(s)
        subprocess.run(['patch','-p1','-d',temporary],input=(root/'patches/xnu-12377/sheng-wifi-association-event.patch').read_text(),text=True,capture_output=True,check=True)
        s=staged.read_text()
body=s.split('\tu_int32_t maximum =',1)[1].split('\n\tlck_rw_lock_shared(&kev_rwlock);',1)[0]
out=root/'build/wifi-event';out.mkdir(exist_ok=True)
prefix=r'''
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
typedef uint32_t u_int32_t;
#define MLEN 224
#define MCLBYTES 2048
#define KEV_VENDOR_APPLE 1
#define KEV_IEEE80211_CLASS 6
#define KEV_MSG_HEADER_SIZE 24
#define MT_DATA 1
#define relaxed 0
#define os_atomic_inc(p, order) (++*(p))
#define bcopy(src,dst,len) memcpy(dst,src,len)
#define mtod(m,type) ((type)((m)->data))
struct kern_event_msg { uint32_t total_size,vendor_code,kev_class,kev_subclass,id,event_code; char event_data[1]; };
struct kev_msg {uint32_t vendor_code,kev_class,kev_subclass,event_code; struct {uint32_t data_length;void *data_ptr;} dv[5];};
struct mbuf {unsigned m_len; _Alignas(8) unsigned char data[MCLBYTES];};
static struct {unsigned kes_toobig,kes_nomem;} kevtstat;
static unsigned static_event_id,small_allocs,cluster_allocs;static int fail;
static struct mbuf *last;
static struct mbuf *m_get(int wait,int type) {(void)wait;(void)type;small_allocs++;return fail?NULL:calloc(1,sizeof(struct mbuf));}
static struct mbuf *m_getcl(int wait,int type,int flags) {(void)wait;(void)type;(void)flags;cluster_allocs++;return fail?NULL:calloc(1,sizeof(struct mbuf));}
static int post(struct kev_msg *event_msg,int wait) {struct mbuf *m;struct kern_event_msg *ev;char *tmp;unsigned total_size;int i;
'''
tests=r'''
 last=m;return 0;}
int main(void){
 char name[16]="en0";unsigned char payload[540];for(unsigned i=0;i<sizeof payload;i++)payload[i]=(unsigned char)i;
 struct kev_msg event={.vendor_code=1,.kev_class=6,.kev_subclass=1,.event_code=9,.dv={{16,name},{540,payload}}};
 assert(!post(&event,0)&&cluster_allocs==1&&!small_allocs);
 struct kern_event_msg *message=(void *)last->data;assert(last->m_len==580&&message->total_size==580);
 assert(!memcmp(message->event_data,name,16)&&!memcmp(message->event_data+16,payload,540));free(last);last=NULL;
 event.dv[1].data_length=4;assert(!post(&event,0)&&small_allocs==1);free(last);last=NULL;
 event.dv[1].data_length=540;event.kev_class=1;assert(post(&event,0)==EMSGSIZE&&!last&&cluster_allocs==1);
 event.kev_class=6;event.dv[1].data_length=UINT32_MAX;assert(post(&event,0)==EMSGSIZE&&!last);
 event.dv[1].data_length=2048;assert(post(&event,0)==EMSGSIZE&&!last);
 event.dv[1].data_length=540;fail=1;assert(post(&event,0)==ENOMEM&&!last);assert(kevtstat.kes_toobig==3&&kevtstat.kes_nomem==1);
 puts("PASS: full 540-byte Wi-Fi association event survives XNU allocation/copy; small/non-Wi-Fi bounds, oversized vectors, overflow and allocation failure");
}
'''
code=prefix+'u_int32_t maximum ='+body+tests
(out/'test.c').write_text(code)
subprocess.run(['clang','-std=c11','-O1','-g','-fsanitize=address,undefined',str(out/'test.c'),'-o',str(out/'test')],check=True)
r=subprocess.run([str(out/'test')],capture_output=True,text=True,check=True);print(r.stdout,end='')
(out/'report.json').write_text(json.dumps({'passed':True,'mbuf_simulated':True,'hardware_verified':False,'patched_source_sha256':hashlib.sha256(s.encode()).hexdigest(),'output':r.stdout},indent=2)+'\n')
