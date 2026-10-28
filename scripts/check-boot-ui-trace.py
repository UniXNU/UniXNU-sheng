#!/usr/bin/env python3
"""Run actual UI capture, stack grouping and display encoding with VM/task stand-ins."""
from pathlib import Path
import json,hashlib,subprocess
root=Path(__file__).resolve().parents[1];out=root/'build/cpu-desktop/ui-trace';out.mkdir(parents=True,exist_ok=True)
s=(root/'kits/sheng/BootUiTraceProbe.c').read_text();body=s[s.index('static struct bt_entry samples'):s.index('// One kernel thread found')]
prefix=r'''
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>
#include <pthread.h>
#include "BootUiTraceCore.h"
#include "BootUiTraceDetails.h"
#define KERN_SUCCESS 0
struct saved {uint64_t pc,lr,fp;bool valid;};typedef struct saved arm_saved_state_t;
struct thread {bool sched_lock;struct {struct saved *upcb;} machine;struct thread *next;uint64_t thread_id;uint32_t c_switch;uint64_t kernel_stack;uint64_t wait_event;int state;uint8_t block_hint;int16_t sched_pri;};typedef struct thread *thread_t;
static uint64_t recount_thread_time_mach(struct thread *thread){(void)thread;return 0;}
static void proc_name(int pid,char *name,int size){snprintf(name,(unsigned)size,"pid%d",pid);}
struct kstate {struct {struct {uint64_t fp,lr,sp;} ss;} machine;};typedef struct kstate *thread_kernel_state_t;
static struct kstate fake_kstate;static uint64_t kernel_stack_size=16384;static uint64_t fake_kstack[2048] __attribute__((aligned(16)));
#define thread_get_kernel_state(t) (&fake_kstate)
static uint64_t fake_now=1000;static uint64_t mach_absolute_time(void){return fake_now;}
static void absolutetime_to_nanoseconds(uint64_t t,uint64_t *ns){*ns=t;}
struct task;
struct port {bool locked;int pid;};typedef struct port *ipc_port_t;
#define IP_NULL NULL
struct space {unsigned refs;struct task *owner;bool fail;struct port port;};typedef struct space *ipc_space_t;
static uint64_t native_regs[17];
static bool image_present;
struct map {struct task *owner;unsigned refs;int fail;uint64_t fp[32],lr[32];};typedef struct map *vm_map_t;
struct task {bool busy;vm_map_t map;unsigned thread_count;thread_t threads;ipc_space_t itk_space;};typedef struct task *task_t;
static bool interrupts=true;
static int thread_lck_grp;
static bool task_lock_try(task_t t){if(t->busy)return false;t->busy=true;return true;}
static void task_unlock(task_t t){assert(t->busy);t->busy=false;}
static uint64_t task_get_all_image_info_addr(task_t t){assert(t->busy);return image_present?0x170001000:0;}
static void is_reference(ipc_space_t s){assert(s->owner->busy);++s->refs;}
static void is_release(ipc_space_t s){assert(!s->owner->busy && interrupts && s->refs && !s->port.locked);--s->refs;}
static int ipc_port_translate_send(ipc_space_t s,unsigned name,ipc_port_t *p){assert(interrupts && !s->owner->busy && s->refs && name==0x1234);if(s->fail)return 1;assert(!s->port.locked);s->port.locked=true;*p=&s->port;return 0;}
static int ipc_port_get_receiver_task_locked(ipc_port_t p,uintptr_t *ignored){assert(p->locked && !ignored);return p->pid;}
static void ip_mq_unlock(ipc_port_t p){assert(p->locked);p->locked=false;}
#define get_saved_state_reg(s,n) ((void)(s),native_regs[n])
static bool ml_get_interrupts_enabled(void){return interrupts;}
static bool ml_set_interrupts_enabled(bool v){bool old=interrupts;interrupts=v;return old;}
static bool simple_lock_try(bool *lock,int *grp){assert(!interrupts&&grp==&thread_lck_grp);if(*lock)return false;*lock=true;return true;}
static void thread_unlock(thread_t t){assert(!interrupts&&t->sched_lock);t->sched_lock=false;}
static void vm_map_reference(vm_map_t m){assert(m->owner->busy);++m->refs;}
static void vm_map_deallocate(vm_map_t m){assert(!m->owner->busy&&interrupts&&m->refs);--m->refs;}
#define get_saved_state_pc(s) ((s)->pc)
#define get_saved_state_lr(s) ((s)->lr)
#define get_saved_state_fp(s) ((s)->fp)
#define is_saved_state64(s) ((s)->valid)
#define queue_iterate(head,t,type,field) for(t=*(head);t;t=t->next)
static pthread_mutex_t gate=PTHREAD_MUTEX_INITIALIZER;static pthread_cond_t changed=PTHREAD_COND_INITIALIZER;
static bool hold,entered;
static int vm_map_read_user(vm_map_t m,uint64_t address,void *out,unsigned size){
 assert(!m->owner->busy&&interrupts&&(size==16||size==24)&&m->refs);
 if(address==0x170001000 && size==16){uint64_t info[2]={UINT64_C(1)<<32,0x170001100};memcpy(out,info,16);return 0;}
 if(address==0x170001100 && size==24){uint64_t image[3]={0x118ce8000,0,0};memcpy(out,image,24);return 0;}
 assert(size==16);
 assert(!pthread_mutex_lock(&gate));if(hold){entered=true;assert(!pthread_cond_broadcast(&changed));}
 while(hold)assert(!pthread_cond_wait(&changed,&gate));assert(!pthread_mutex_unlock(&gate));
 if(m->fail)return m->fail;
 for(unsigned i=0;i<32;++i)if(m->fp[i]==address){uint64_t pair[2]={i<31?m->fp[i+1]:0,m->lr[i]};memcpy(out,pair,16);return 0;}
 return 1;
}
'''
main=r'''
static struct saved regs={0x1e3dbcb58,0x1e3dc0224,0x170000000,true};
static struct thread threads[65];static struct map map;static struct task task;
static void setup(unsigned count){
 memset(native_regs,0,sizeof(native_regs));image_present=false;
 memset(threads,0,sizeof(threads));memset(&map,0,sizeof(map));memset(&task,0,sizeof(task));
 task.map=&map;task.thread_count=count;task.threads=threads;map.owner=&task;
 for(unsigned i=0;i<count;++i){threads[i].machine.upcb=&regs;threads[i].next=i+1<count?&threads[i+1]:NULL;}
 for(unsigned i=0;i<32;++i){map.fp[i]=0x170000000+i*16;map.lr[i]=0x1e3dc0000+i*4;}
}
static void *capture_worker(void *unused){(void)unused;boot_ui_trace_begin();boot_ui_trace_task(&task,1,57,"backboardd",0x1863c000);boot_ui_trace_publish();return NULL;}
static uint64_t decode(const char *s){static const char digits[]="0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz-_";uint64_t v=0;for(;*s;++s){const char *p=strchr(digits,*s);assert(p);v=v*64+(unsigned)(p-digits);}return v;}
int main(void){
 for(unsigned i=0;i<39;++i){uint64_t v=(UINT64_C(1)<<i)-1;char token[9];bt_address(token,v);assert(strlen(token)<=7&&decode(token)==v);}
 for(unsigned n=0;n<10000;++n){uint64_t v=((uint64_t)n*UINT64_C(771723231))&((UINT64_C(1)<<39)-1);char token[9];bt_address(token,v);assert(decode(token)==v);}
 setup(3);boot_ui_trace_begin();boot_ui_trace_task(&task,1,57,"backboardd",0x1863c000);assert(!task.busy&&interrupts&&!map.refs&&draft.threads==3&&draft.count==1&&draft.entries[0].count==3);
 boot_ui_trace_task(&task,2,26,"SpringBoard",0x1863c000);assert(draft.count==2&&draft.threads==6);boot_ui_trace_publish();boot_ui_trace_snapshot(0);
 assert(boot_ui_trace_ready&&boot_ui_trace_groups==2&&boot_ui_trace_pages==2&&strstr(boot_ui_trace_label,"B=p57+1863c000")&&strstr(boot_ui_trace_rows[0],"B01x3")&&strstr(boot_ui_trace_rows[0]+60,"S01x3"));
 setup(1);struct space space={.owner=&task,.port={.pid=1}};task.itk_space=&space;
 native_regs[16]=UINT64_MAX-46;native_regs[1]=3;native_regs[3]=0x1234;native_regs[4]=UINT64_C(0x10000000)<<32;image_present=true;
 boot_ui_trace_begin();boot_ui_trace_task(&task,1,57,"backboardd",0xc14000);
 assert(!space.refs&&!space.port.locked&&draft.entries[0].peer==1&&draft.entries[0].peer_code==0&&draft_main[0].frames[31]==map.lr[31]&&!draft_main[0].error&&draft_main[0].image==0x118ce8000&&!draft_main[0].image_error);
 native_regs[1]=2;boot_ui_trace_begin();boot_ui_trace_task(&task,1,57,"backboardd",0xc14000);assert(draft.entries[0].peer_code==5&&draft.entries[0].peer==1&&!space.refs);
 native_regs[1]=3;
 boot_ui_trace_publish();boot_ui_trace_snapshot(8);for(unsigned row=0;row<BT_ROWS;++row)puts(boot_ui_trace_rows[row]);assert(strstr(boot_ui_trace_rows[0],"MAIN B p57")&&strstr(boot_ui_trace_rows[2],"B FP01")&&strstr(boot_ui_trace_rows[5],"B FP25"));
 space.fail=true;boot_ui_trace_begin();boot_ui_trace_task(&task,1,57,"backboardd",0);assert(draft.entries[0].peer_code==4&&!space.refs);
 native_regs[3]=0;boot_ui_trace_begin();boot_ui_trace_task(&task,1,57,"backboardd",0);assert(draft.entries[0].peer_code==2&&!space.refs);
 native_regs[3]=0x1234;task.itk_space=NULL;boot_ui_trace_begin();boot_ui_trace_task(&task,1,57,"backboardd",0);assert(draft.entries[0].peer_code==3);
 setup(1);boot_ui_trace_begin();boot_ui_trace_task(&task,1,97,"chronod",0);boot_ui_trace_publish();boot_ui_trace_snapshot(8);assert(strstr(boot_ui_trace_rows[0],"MAIN C p97"));
 setup(2);boot_ui_trace_begin();boot_ui_trace_task(&task,1,140,"configd",0x1000);boot_ui_trace_task(&task,2,110,"nehelper",0x1000);boot_ui_trace_task(&task,3,122,"profiled",0x1000);boot_ui_trace_task(&task,4,27,"SpringBoard",0x1000);boot_ui_trace_task(&task,5,99,"ignored",0);
 assert(draft_pid[2]==122&&draft_pid[3]==27&&draft.threads==8);boot_ui_trace_publish();boot_ui_trace_snapshot(0);
 assert(strstr(boot_ui_trace_label,"T=p122 U=p27")&&boot_ui_trace_pages==3);
 boot_ui_trace_snapshot(8*(boot_ui_trace_pages-1));assert(strstr(boot_ui_trace_rows[0],"MAIN T p122")&&strstr(boot_ui_trace_rows[7],"MAIN U p27"));
 boot_ui_trace_snapshot(8*(boot_ui_trace_pages-2));assert(strstr(boot_ui_trace_rows[0],"MAIN B p140")&&strstr(boot_ui_trace_rows[7],"MAIN S p110"));
 {struct {char rows[12][120];char guard[16];} live;memset(&live,'Z',sizeof(live));unsigned got=boot_ui_trace_role_rows(1,live.rows,12,1);
  for(unsigned i=0;i<got;++i){assert(strlen(live.rows[i])<120);puts(live.rows[i]);}for(unsigned i=0;i<16;++i)assert(live.guard[i]=='Z');
  assert(got==6&&strstr(live.rows[0],"STKS p110")&&strstr(live.rows[1],"STKS trap=")&&strstr(live.rows[2],"STKS 01")&&strstr(live.rows[4],"STKS 17")&&strstr(live.rows[5],"STKS#1 x2 p-1"));
  assert(boot_ui_trace_role_rows(1,live.rows,5,1)==0&&!trace_lock);trace_lock=1;assert(boot_ui_trace_role_rows(1,live.rows,12,1)==0&&boot_ui_trace_role_peer(1)==-1);trace_lock=0;
  got=boot_ui_trace_role_rows(3,live.rows,12,0);for(unsigned i=0;i<got;++i)puts(live.rows[i]);
  assert(got==2&&strstr(live.rows[0],"STKU SpringBoard p27 cache+1000")&&strstr(live.rows[1],"STKU#1 x2")&&boot_ui_trace_role_peer(3)==0&&!trace_lock);}
 struct {char row[60];char guard[16];} bounded;memset(&bounded,'Z',sizeof(bounded));struct bt_entry maximum={.role='S',.index=64,.count=64,.pc=(UINT64_C(1)<<39)-1,.lr=(UINT64_C(1)<<39)-1,.error=3};for(unsigned i=0;i<8;++i)maximum.frames[i]=maximum.pc;bt_format(&maximum,0,bounded.row);assert(strlen(bounded.row)<60);for(unsigned i=0;i<16;++i)assert(bounded.guard[i]=='Z');
 setup(65);boot_ui_trace_begin();boot_ui_trace_task(&task,1,57,"backboardd",0);assert(draft.threads==64&&draft_omitted==1&&!map.refs);
 setup(3);task.busy=true;boot_ui_trace_begin();boot_ui_trace_task(&task,1,57,"backboardd",0);assert(draft_busy==1&&!draft.threads);task.busy=false;
 threads[0].sched_lock=true;boot_ui_trace_begin();boot_ui_trace_task(&task,1,57,"backboardd",0);assert(draft_busy==1&&draft.threads==2&&threads[0].sched_lock&&interrupts);
 setup(1);map.fail=1;boot_ui_trace_begin();boot_ui_trace_task(&task,1,57,"backboardd",0);assert(draft.read_failures==1&&draft.entries[0].error==1&&!map.refs);
 map.fail=0;map.fp[1]=map.fp[0];boot_ui_trace_begin();boot_ui_trace_task(&task,1,57,"backboardd",0);assert(draft.entries[0].error==3&&!map.refs);
 struct bt_groups groups={0};for(unsigned i=0;i<128;++i){struct bt_entry e=maximum;e.pc=i;e.index=i%64+1;bt_add(&groups,&e);}char rows[BT_ROWS][120];bt_rows(&groups,8,rows);assert(strstr(rows[0],"S49x1"));
 setup(1);boot_ui_trace_begin();boot_ui_trace_task(&task,1,57,"backboardd",0);boot_ui_trace_publish();boot_ui_trace_snapshot(0);char before[sizeof(boot_ui_trace_rows)];memcpy(before,boot_ui_trace_rows,sizeof(before));
 assert(!pthread_mutex_lock(&gate));hold=true;entered=false;assert(!pthread_mutex_unlock(&gate));pthread_t worker;assert(!pthread_create(&worker,NULL,capture_worker,NULL));
 assert(!pthread_mutex_lock(&gate));while(!entered)assert(!pthread_cond_wait(&changed,&gate));assert(!pthread_mutex_unlock(&gate));
 for(unsigned i=0;i<8;++i){boot_ui_trace_snapshot(i);assert(!memcmp(before,boot_ui_trace_rows,sizeof(before)));assert(boot_ui_trace_phase==2&&boot_ui_trace_index==1);}
 assert(!pthread_mutex_lock(&gate));hold=false;assert(!pthread_cond_broadcast(&changed));assert(!pthread_mutex_unlock(&gate));assert(!pthread_join(worker,NULL));assert(!map.refs);
 trace_lock=1;boot_ui_trace_snapshot(9);assert(!memcmp(before,boot_ui_trace_rows,sizeof(before)));trace_lock=0;
 /* Hot threads: a thread whose context-switch count grew by 800 in one second shows with its rate and stack; a quiet one (and the first sighting) does not. */
 {setup(2);threads[0].thread_id=11;threads[1].thread_id=12;threads[0].c_switch=100;threads[1].c_switch=50;fake_now=5000000000ull;
  boot_ui_trace_begin();boot_ui_trace_task(&task,1,57,"backboardd",0);boot_ui_trace_publish();
  char hot[8][120];assert(boot_ui_trace_hot_rows(hot,8)==0);
  fake_now+=1000000000ull;threads[0].c_switch=900;threads[1].c_switch=52;
  /* A kernel stack for thread 0: link register, then a two-frame chain inside the stack. */
  threads[0].kernel_stack=(uint64_t)fake_kstack;fake_kstate.machine.ss.lr=0xfffffff0071a0000ull;fake_kstate.machine.ss.fp=(uint64_t)&fake_kstack[8];
  fake_kstack[8]=(uint64_t)&fake_kstack[16];fake_kstack[9]=0xfffffff0071a1111ull;fake_kstack[16]=0;fake_kstack[17]=0xfffffff0071a2222ull;
  boot_ui_trace_begin();boot_ui_trace_task(&task,1,57,"backboardd",0);boot_ui_trace_publish();
  unsigned rows=boot_ui_trace_hot_rows(hot,8);for(unsigned i=0;i<rows;++i)puts(hot[i]);
  assert(rows==3 && strstr(hot[0],"HOTB p57 tid=b sw/s=800") && strstr(hot[0],"pc=1e3dbcb58") && !strncmp(hot[1],"HOT+ b ",7) &&
   !strncmp(hot[2],"HOTK b st=",10) && strstr(hot[2]," fffffff0071a0000 fffffff0071a1111 fffffff0071a2222") && !trace_lock);}
 puts("PASS: actual UI capture/formatter; lossless address tokens, distinct tasks, grouping, 64-thread cap, VM/IPC reads outside task locks, map/space/port reference/lock/IRQ restoration, main32/image/peer/lookup failures, invalid chains, copy failure, pages and blocked-reader isolation");
}
'''
(out/'test.c').write_text(prefix+body+main)
subprocess.run(['clang','-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-function','-O1','-g','-fsanitize=address,undefined','-pthread','-I',str(root/'kits/sheng'),str(out/'test.c'),'-o',str(out/'test')],check=True,timeout=45)
r=subprocess.run([str(out/'test')],check=True,capture_output=True,text=True,timeout=15)
(out/'formatted.txt').write_text(r.stdout)
import importlib.util
spec=importlib.util.spec_from_file_location('decoder',root/'scripts/decode-boot-ui-trace.py');decoder=importlib.util.module_from_spec(spec);spec.loader.exec_module(decoder)
rows=decoder.decode(r.stdout);backboard=next(row for row in rows if row['role']=='B')
for first in (1,9,17,25):
 assert backboard['frames_%d_%d'%(first,first+7)]==[hex(0x1e3dc0000+i*4) for i in range(first-1,first+7)]
print(r.stdout.splitlines()[-1]);print('PASS: actual MAIN32 screen rows decode to all 32 exact original return addresses')
(out/'report.json').write_text(json.dumps(dict(passed=True,source_body_sha256=hashlib.sha256(body.encode()).hexdigest(),kernel_VM_and_task_APIs_substituted=True,hardware_wait_dependency_identified=False),indent=2)+'\n')
