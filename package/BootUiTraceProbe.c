// SPDX-License-Identifier: MIT
// Cross-task reads use the native XNU VM API, outside task/scheduler locks.
// A user-page fault can block this isolated capture worker, never the monitor.
#include <kern/task.h>
#include <kern/thread.h>
#include <kern/simple_lock.h>
#include <kern/clock.h>
#include <kern/misc_protos.h>
#include <machine/machine_routines.h>
#include <arm/thread.h>
#include <arm/cpu_data_internal.h>
#include <arm/cpu_internal.h>
#include <kern/processor.h>
#include <vm/vm_map.h>
#include <ipc/ipc_space.h>
#include <ipc/ipc_port.h>
#include <string.h>
#include "BootUiTraceCore.h"
extern void proc_name(int pid, char *buf, int size);   // bsd/sys/proc.h; the header itself clashes with the Mach types used here
#include "BootUiTraceDetails.h"
static struct bt_entry samples[BT_MAX_THREADS];
// Hot threads: the context-switch rate of each sampled thread between two snapshots, so a thread that is woken hundreds of times a second
// shows even though every single stack sample finds it idle in mach_msg.
// CPU rows: each thread's run time (user and system, xnu's recount) between two snapshots, so the threads that actually burn a frame's time
// show by name of their stacks, which the interrupt-driven profile cannot weigh (it samples at interrupts, not at a fixed rate).
extern uint64_t recount_thread_time_mach(struct thread *thread);
static uint64_t sample_tid[BT_MAX_THREADS],sample_cpu[BT_MAX_THREADS];static uint32_t sample_cs[BT_MAX_THREADS];
struct bt_prev {uint64_t tid,at,cpu;uint32_t cs;};
#define BT_HOT 4
#define BT_HOT_MIN_RATE 50
#define BT_HOT_FRAMES 18
#define BT_KFRAMES 12
#define BT_CPU 4
struct bt_hotrow {unsigned rate,cpu;char role;int peer;uint64_t tid,pc,lr,frames[BT_HOT_FRAMES],kernel[BT_KFRAMES];unsigned state,hint;int pri;uint64_t event,trap,args[3];};
static uint64_t sample_k[BT_MAX_THREADS][BT_KFRAMES],sample_event[BT_MAX_THREADS];
static unsigned sample_state[BT_MAX_THREADS],sample_hint[BT_MAX_THREADS];static int sample_pri[BT_MAX_THREADS];
// Where a blocked thread sits in the kernel: the link register and frame chain its context switch saved (the thread is not running while
// it is blocked; for a running one the chain is merely stale, and every step stays inside its kernel stack).
static void bt_kernel_stack(thread_t thread,uint64_t *out) {
 memset(out,0,BT_KFRAMES*sizeof(uint64_t));
 if(!thread->kernel_stack)return;
 const uint64_t low=(uint64_t)thread->kernel_stack,high=low+(uint64_t)kernel_stack_size;
 thread_kernel_state_t state=thread_get_kernel_state(thread);
 uint64_t fp=state->machine.ss.fp;out[0]=state->machine.ss.lr;
 for(unsigned i=1;i<BT_KFRAMES && fp>=low && fp+16<=high && !(fp&15);++i) {
  const uint64_t *frame=(const uint64_t *)fp;out[i]=frame[1];
  const uint64_t next=frame[0];if(next<=fp)break;fp=next;
 }
}
#define BT_ROLES 4
static struct bt_main draft_main[BT_ROLES],published_main[BT_ROLES];
static struct bt_prev hot_prev[BT_ROLES][BT_MAX_THREADS];
static struct bt_hotrow hot_draft[BT_ROLES][BT_HOT],hot_published[BT_ROLES][BT_HOT];
// Threads inside a Mach trap other than mach_msg (vm_deallocate, semaphore_wait): kept apart from the hot ones so a busy role cannot crowd
// them out (int-235: four work-queue threads took every hot slot of backboardd while its render thread sat in vm_deallocate).
#define BT_TRAPPED 3
static struct bt_hotrow trap_draft[BT_ROLES][BT_TRAPPED],trap_published[BT_ROLES][BT_TRAPPED];
static struct bt_hotrow cpu_draft[BT_ROLES][BT_CPU],cpu_published[BT_ROLES][BT_CPU];
static struct bt_groups draft,published;
static unsigned trace_lock;
static char draft_names[BT_ROLES][32],published_names[BT_ROLES][32];
static unsigned draft_pid[BT_ROLES],published_pid[BT_ROLES],draft_slide[BT_ROLES],published_slide[BT_ROLES];
static unsigned draft_busy,published_busy,draft_omitted,published_omitted;
char boot_ui_trace_rows[BT_ROWS][120],boot_ui_trace_label[120];
unsigned boot_ui_trace_ready,boot_ui_trace_groups,boot_ui_trace_threads,boot_ui_trace_pages;
volatile unsigned boot_ui_trace_phase,boot_ui_trace_index;

void boot_ui_trace_begin(void) {
 memset(draft_main,0,sizeof(draft_main));memset(hot_draft,0,sizeof(hot_draft));memset(trap_draft,0,sizeof(trap_draft));memset(cpu_draft,0,sizeof(cpu_draft));
 memset(&draft,0,sizeof(draft));memset(draft_pid,0,sizeof(draft_pid));
 memset(draft_slide,0,sizeof(draft_slide));memset(draft_names,0,sizeof(draft_names));
 draft_busy=0;draft_omitted=0;boot_ui_trace_phase=1;
}
static int read_user(void *context,uint64_t address,void *bytes,unsigned size) {
 return vm_map_read_user((vm_map_t)context,address,bytes,size)!=KERN_SUCCESS;
}
static void lookup_peer(struct bt_entry *e,ipc_space_t space) {
 e->peer=-1;e->peer_code=1;
 if(e->trap!=UINT64_MAX-46)return;
 e->peer_code=2;if(!(unsigned)e->regs[3])return;
 e->peer_code=3;if(!space)return;
 ipc_port_t port=IP_NULL;e->peer_code=4;
 if(ipc_port_translate_send(space,(unsigned)e->regs[3],&port)!=KERN_SUCCESS)return;
 e->peer=ipc_port_get_receiver_task_locked(port,NULL);e->peer_code=(e->regs[1]&1)?0:5;ip_mq_unlock(port);
}
void boot_ui_trace_task(task_t task,unsigned index,int pid,const char *name,unsigned slide) {
 if(!ml_get_interrupts_enabled()){++draft_busy;return;}
 if(index<1 || index>BT_ROLES)return;
 unsigned role=index-1;
 draft_pid[role]=(unsigned)pid;draft_slide[role]=slide;
 strlcpy(draft_names[role],name,sizeof(draft_names[role]));
 if(!task_lock_try(task)){++draft_busy;return;}
 vm_map_t map=task->map;
 if(!map){task_unlock(task);++draft_busy;return;}
 vm_map_reference(map);
 ipc_space_t space=task->itk_space;if(space)is_reference(space);
 uint64_t images=task_get_all_image_info_addr(task);
 unsigned count=0,visited=0;thread_t thread;
 queue_iterate(&task->threads,thread,thread_t,task_threads) {
  if(visited++>=BT_MAX_THREADS){draft_omitted+=(unsigned)task->thread_count-BT_MAX_THREADS;break;}
  bool irq=ml_set_interrupts_enabled(false);
  if(!simple_lock_try(&thread->sched_lock,&thread_lck_grp)){++draft_busy;ml_set_interrupts_enabled(irq);continue;}
  arm_saved_state_t *state=thread->machine.upcb;
  if(state && is_saved_state64(state)) {
   struct bt_entry *e=&samples[count];memset(e,0,sizeof(*e));sample_tid[count]=thread->thread_id;sample_cs[count]=thread->c_switch;sample_cpu[count]=recount_thread_time_mach(thread);bt_kernel_stack(thread,sample_k[count]);sample_event[count]=(uint64_t)thread->wait_event;sample_state[count]=(unsigned)thread->state;sample_hint[count]=(unsigned)thread->block_hint;sample_pri[count]=thread->sched_pri;++count;
   e->index=visited;e->role=role?"BSTU"[role&3]:(!strcmp(name,"chronod")?'C':(!strcmp(name,"liveactivitiesd")?'L':'B'));
   e->pc=(uint64_t)get_saved_state_pc(state);e->lr=(uint64_t)get_saved_state_lr(state);
   e->fp=(uint64_t)get_saved_state_fp(state);
   e->trap=(uint64_t)get_saved_state_reg(state,16);
   for(unsigned reg=0;reg<8;++reg)e->regs[reg]=(uint64_t)get_saved_state_reg(state,reg);
   if((e->pc && !bt_user(e->pc,4)) || (e->lr && !bt_user(e->lr,4)))e->error=2;
  }
  thread_unlock(thread);ml_set_interrupts_enabled(irq);
 }
 task_unlock(task);
 // No thread pointers or suspended threads survive collection. The copied FP
 // can race with a running task, so malformed/changed chains are reported.
 for(unsigned i=0;i<count;++i) {
  boot_ui_trace_phase=role?5:4;boot_ui_trace_index=samples[i].index;
  lookup_peer(&samples[i],space);
  boot_ui_trace_phase=role?3:2;
  if(samples[i].index==1) {
   struct bt_main *main=&draft_main[role];main->entry=samples[i];main->pid=pid;main->slide=slide;
   if(!main->entry.error)bt_walk_n(&main->entry,read_user,map,main->frames,32);
   main->error=main->entry.error;
   uint64_t info[2]={0},image[3]={0};main->image_error=1;
   if(bt_user(images,sizeof(info)) && !read_user(map,images,info,sizeof(info)) &&
      (unsigned)(info[0]>>32) && bt_user(info[1],sizeof(image)) &&
      !read_user(map,info[1],image,sizeof(image)) && bt_user(image[0],32)) {
    main->image=image[0];main->image_error=0;
   }
  }
  if(!samples[i].error)bt_walk(&samples[i],read_user,map);
  bt_add(&draft,&samples[i]);
 }
 {
  // Rates against the previous snapshot of the same thread; the four hottest of the role are kept with their stacks.
  const uint64_t now=mach_absolute_time();
  for(unsigned i=0;i<count;++i) {
   struct bt_prev *slot=NULL,*free_slot=NULL;
   for(unsigned k=0;k<BT_MAX_THREADS;++k) {
    if(hot_prev[role][k].tid==sample_tid[i]){slot=&hot_prev[role][k];break;}
    if(!free_slot && !hot_prev[role][k].tid)free_slot=&hot_prev[role][k];
   }
   unsigned rate=0,cpu=0;
   if(slot && now>slot->at) {
    uint64_t ns=0;absolutetime_to_nanoseconds(now-slot->at,&ns);
    const uint32_t switches=sample_cs[i]-slot->cs;
    rate=ns?(unsigned)((uint64_t)switches*UINT64_C(1000000000)/ns):0;
    uint64_t ran=0;if(sample_cpu[i]>slot->cpu)absolutetime_to_nanoseconds(sample_cpu[i]-slot->cpu,&ran);
    cpu=ns?(unsigned)(ran*1000/ns):0;   /* per mille of one core */
   }
   if(cpu>=10) {
    unsigned at=BT_CPU;
    for(unsigned h=0;h<BT_CPU;++h)if(cpu>cpu_draft[role][h].cpu){at=h;break;}
    if(at<BT_CPU) {
     for(unsigned h=BT_CPU-1;h>at;--h)cpu_draft[role][h]=cpu_draft[role][h-1];
     struct bt_hotrow *row=&cpu_draft[role][at];memset(row,0,sizeof(*row));
     row->rate=rate;row->cpu=cpu;row->role=samples[i].role;row->peer=samples[i].peer;row->tid=sample_tid[i];
     row->pc=samples[i].pc;row->lr=samples[i].lr;
     memcpy(row->kernel,sample_k[i],sizeof(row->kernel));row->event=sample_event[i];row->state=sample_state[i];row->hint=sample_hint[i];row->pri=sample_pri[i];
     {struct bt_entry deep=samples[i];deep.error=0;bt_walk_n(&deep,read_user,map,row->frames,BT_HOT_FRAMES);}
    }
   }
   const int64_t trap=(int64_t)samples[i].trap;
   if(trap<0 && trap!=-31 && trap!=-47) {
    for(unsigned t=0;t<BT_TRAPPED;++t) {
     struct bt_hotrow *row=&trap_draft[role][t];if(row->tid)continue;
     row->rate=rate?rate:1;row->role=samples[i].role;row->peer=samples[i].peer;row->tid=sample_tid[i];
     row->pc=samples[i].pc;row->lr=samples[i].lr;
     memcpy(row->kernel,sample_k[i],sizeof(row->kernel));row->event=sample_event[i];row->state=sample_state[i];row->hint=sample_hint[i];row->pri=sample_pri[i];
     row->trap=samples[i].trap;for(unsigned a=0;a<3;++a)row->args[a]=samples[i].regs[a];
     {struct bt_entry deep=samples[i];deep.error=0;bt_walk_n(&deep,read_user,map,row->frames,BT_HOT_FRAMES);}
     break;
    }
   }
   {
    if(rate>=BT_HOT_MIN_RATE) {
     unsigned at=BT_HOT;
     for(unsigned h=0;h<BT_HOT;++h)if(rate>hot_draft[role][h].rate){at=h;break;}
     if(at<BT_HOT) {
      for(unsigned h=BT_HOT-1;h>at;--h)hot_draft[role][h]=hot_draft[role][h-1];
      struct bt_hotrow *row=&hot_draft[role][at];memset(row,0,sizeof(*row));
      row->rate=rate;row->role=samples[i].role;row->peer=samples[i].peer;row->tid=sample_tid[i];
      row->pc=samples[i].pc;row->lr=samples[i].lr;
      memcpy(row->kernel,sample_k[i],sizeof(row->kernel));row->event=sample_event[i];row->state=sample_state[i];row->hint=sample_hint[i];row->pri=sample_pri[i];
      {struct bt_entry deep=samples[i];deep.error=0;bt_walk_n(&deep,read_user,map,row->frames,BT_HOT_FRAMES);}   /* deeper than the grouped 8 frames: names the thread */
     }
    }
   }
   if(!slot)slot=free_slot?free_slot:&hot_prev[role][i%BT_MAX_THREADS];
   slot->tid=sample_tid[i];slot->cs=sample_cs[i];slot->cpu=sample_cpu[i];slot->at=now;
  }
 }
 if(space)is_release(space);
 vm_map_deallocate(map);boot_ui_trace_phase=1;
}
void boot_ui_trace_publish(void) {
 if(!__atomic_exchange_n(&trace_lock,1,__ATOMIC_ACQUIRE)) {
  published=draft;memcpy(published_main,draft_main,sizeof(draft_main));memcpy(hot_published,hot_draft,sizeof(hot_draft));memcpy(trap_published,trap_draft,sizeof(trap_draft));memcpy(cpu_published,cpu_draft,sizeof(cpu_draft));memcpy(published_pid,draft_pid,sizeof(draft_pid));
  memcpy(published_slide,draft_slide,sizeof(draft_slide));memcpy(published_names,draft_names,sizeof(draft_names));
  published_busy=draft_busy;published_omitted=draft_omitted;
  __atomic_store_n(&trace_lock,0,__ATOMIC_RELEASE);
 }
 boot_ui_trace_phase=0;
}
// The live banner's view of one traced role (1 = the boot-ui-focus2 process, SpringBoard): with detail, the main thread's saved registers and
// its first 24 frame-pointer return addresses, then every other thread group; without, a name row and every group including the main
// thread's. A group row is its Mach peer (pid of the receiver it sends to, -1 none), PC, LR and first 7 frames; groups without a user frame
// chain (idle workqueue threads) are left out. Raw hex user addresses; cache+ is the shared cache slide and image the main executable's
// header. Returns the number of rows written (0 while the trace is being published).
unsigned boot_ui_trace_role_rows(unsigned role,char rows[][120],unsigned max,unsigned detail) {
 if(role>=BT_ROLES || max<6 || __atomic_exchange_n(&trace_lock,1,__ATOMIC_ACQUIRE))return 0;
 unsigned n=0;const struct bt_main *m=&published_main[role];const struct bt_entry *e=&m->entry;const char c=bt_role_char(role,m);
 if(m->pid && detail) {
  snprintf(rows[n++],120,"STK%c p%d cache+%x image=%llx pc=%llx lr=%llx fperr=%u peer=%d/e%u",c,m->pid,m->slide,m->image,e->pc,e->lr,m->error,
   e->peer,e->peer_code);
  snprintf(rows[n++],120,"STK%c trap=%lld x0=%llx x1=%llx x2=%llx x3=%llx x4=%llx",c,(long long)e->trap,e->regs[0],e->regs[1],e->regs[2],
   e->regs[3],e->regs[4]);
  for(unsigned row=0;row<3;++row) {
   int at=snprintf(rows[n],120,"STK%c %02u",c,row*8+1);
   for(unsigned i=0;i<8 && at>0 && at<119;++i)at+=snprintf(rows[n]+at,120-at," %llx",m->frames[row*8+i]);
   ++n;
  }
 } else if(published_pid[role]) {
  snprintf(rows[n++],120,"STK%c %.31s p%u cache+%x image=%llx",c,published_names[role],published_pid[role],published_slide[role],m->image);
 }
 for(unsigned g=0;g<published.count && n<max;++g) {
  const struct bt_entry *t=&published.entries[g];
  if(t->role!=c || (detail && t->index==1 && t->count==1) || !t->frames[0])continue;   // a main group of several threads names the others
  // The name of the process this thread waits on (a synchronous XPC or mach_msg to a daemon), so a stall names its peer instead of only a pid.
  char peer[17]="";if(t->peer>0)proc_name(t->peer,peer,sizeof peer);
  int at=snprintf(rows[n],120,"STK%c#%u x%u p%d(%.12s) %llx %llx",t->role,t->index,t->count,t->peer,peer,t->pc,t->lr);
  for(unsigned i=0;i<7 && at>0 && at<119;++i)at+=snprintf(rows[n]+at,120-at," %llx",t->frames[i]);
  ++n;
  // detail 2 (the boot log): the group's next frames in a row of their own, so a thread blocked in a lock or a sync wait shows who called it.
  if(detail>=2 && t->frames[7] && n<max) {
   int more=snprintf(rows[n],120,"STK%c#%u+",t->role,t->index);
   for(unsigned i=7;i<BT_FRAMES && t->frames[i] && more>0 && more<108;++i)more+=snprintf(rows[n]+more,120-more," %llx",t->frames[i]);
   ++n;
  }
 }
 __atomic_store_n(&trace_lock,0,__ATOMIC_RELEASE);
 return n;
}
// One thread's rows: its head (TRP for a trapped thread, HOT for a hot one), deeper frames, kernel frames and trap arguments.
static unsigned hot_row(const struct bt_hotrow *r,unsigned pid,char rows[][120],unsigned n,unsigned max) {
 int at=r->cpu?snprintf(rows[n],120,"CPU%c p%u tid=%llx cpu=%u/1000 sw/s=%u pc=%llx lr=%llx",r->role?r->role:'?',pid,r->tid,r->cpu,r->rate,r->pc,r->lr):
  snprintf(rows[n],120,"%s%c p%u tid=%llx sw/s=%u peer=%d pc=%llx lr=%llx",r->trap&&(int64_t)r->trap<0&&r->trap!=(uint64_t)-31&&r->trap!=(uint64_t)-47?"TRP":"HOT",
  r->role?r->role:'?',pid,r->tid,r->rate,r->peer,r->pc,r->lr);
 for(unsigned i=0;i<4 && at>0 && at<119;++i)at+=snprintf(rows[n]+at,120-at," %llx",r->frames[i]);
 ++n;
 if(n<max) {
  int more=snprintf(rows[n],120,"HOT+ %llx",r->tid);
  for(unsigned i=4;i<BT_HOT_FRAMES && more>0 && more<110;++i)more+=snprintf(rows[n]+more,120-more," %llx",r->frames[i]);
  ++n;
 }
 if(n<max && r->kernel[0]) {
  int kernel=snprintf(rows[n],120,"HOTK %llx st=%x hint=%u pri=%d ev=%llx",r->tid,r->state,r->hint,r->pri,r->event);
  for(unsigned i=0;i<BT_KFRAMES && kernel>0 && kernel<105;++i)kernel+=snprintf(rows[n]+kernel,120-kernel," %llx",r->kernel[i]);
  ++n;
 }
 if(n<max && r->trap)snprintf(rows[n++],120,"HOTR %llx trap=%lld x0=%llx x1=%llx x2=%llx",r->tid,(long long)r->trap,r->args[0],r->args[1],r->args[2]);
 return n;
}
// The hottest threads of every traced role (context switches per second between the last two snapshots, at least BT_HOT_MIN_RATE), then
// the threads inside a Mach trap other than mach_msg (TRP rows): thread id, Mach peer, PC, LR, frames, kernel frames and trap arguments.
// Returns the number of rows (0 while the trace is being published).
unsigned boot_ui_trace_hot_rows(char rows[][120],unsigned max) {
 if(__atomic_exchange_n(&trace_lock,1,__ATOMIC_ACQUIRE))return 0;
 unsigned n=0;
 for(unsigned role=0;role<BT_ROLES;++role)
  for(unsigned t=0;t<BT_TRAPPED && n<max;++t)if(trap_published[role][t].tid)n=hot_row(&trap_published[role][t],published_pid[role],rows,n,max);
 for(unsigned role=0;role<BT_ROLES;++role)
  for(unsigned h=0;h<BT_HOT && n<max;++h)if(hot_published[role][h].rate)n=hot_row(&hot_published[role][h],published_pid[role],rows,n,max);
 for(unsigned role=0;role<BT_ROLES;++role)
  for(unsigned h=0;h<BT_CPU && n<max;++h)if(cpu_published[role][h].cpu)n=hot_row(&cpu_published[role][h],published_pid[role],rows,n,max);
 __atomic_store_n(&trace_lock,0,__ATOMIC_RELEASE);
 return n;
}
// The process a traced role is waiting on: the first of its thread groups that sends a Mach message to another task (peer above launchd,
// not itself). 0 = none, -1 = the trace is being published (keep the previous answer).
int boot_ui_trace_role_peer(unsigned role) {
 if(role>=BT_ROLES || __atomic_exchange_n(&trace_lock,1,__ATOMIC_ACQUIRE))return -1;
 const char c=bt_role_char(role,&published_main[role]);int own=(int)published_pid[role],found=0;
 for(unsigned g=0;g<published.count && !found;++g) {
  const struct bt_entry *t=&published.entries[g];
  if(t->role==c && t->peer>1 && t->peer!=own && !t->peer_code)found=t->peer;
 }
 __atomic_store_n(&trace_lock,0,__ATOMIC_RELEASE);
 return own?found:0;
}
void boot_ui_trace_snapshot(unsigned seconds) {
 if(__atomic_exchange_n(&trace_lock,1,__ATOMIC_ACQUIRE))return;
 unsigned groups_pages=(published.count+BT_ROWS-1)/BT_ROWS;if(!groups_pages)groups_pages=1;
 unsigned main_pages=0;
 if(published_main[0].pid || published_main[1].pid)main_pages=1;
 if(published_main[2].pid || published_main[3].pid)main_pages=2;
 unsigned pages=groups_pages+main_pages;
 unsigned page=(seconds/8)%pages;
 if(page>=groups_pages)bt_main_rows(published_main,(page-groups_pages)*2,boot_ui_trace_rows);
 else bt_rows(&published,page,boot_ui_trace_rows);
 snprintf(boot_ui_trace_label,sizeof(boot_ui_trace_label),
  "STACK64 %u/%u %c=p%u+%x S=p%u+%x T=p%u U=p%u T%u G%u b%u o%u rd%u %s",
  page+1,pages,!strcmp(published_names[0],"chronod")?'C':(!strcmp(published_names[0],"liveactivitiesd")?'L':'B'),published_pid[0],published_slide[0],published_pid[1],published_slide[1],published_pid[2],published_pid[3],
  published.threads,published.count,published_busy,published_omitted,published.read_failures,
  page>=groups_pages?"MAIN32":"PC/LR/FP8; last=MAIN32");
 boot_ui_trace_ready=published.threads!=0;boot_ui_trace_groups=published.count;
 boot_ui_trace_threads=published.threads;boot_ui_trace_pages=pages;
 __atomic_store_n(&trace_lock,0,__ATOMIC_RELEASE);
}
// One kernel thread found by its name (a driver asks for its network input thread when that stops taking packets): state, block hint,
// priority, wait event and up to BT_KFRAMES kernel return addresses, unslid where they lie in the kernel. 0 when found, -1 when busy or absent.
int boot_kthread_report(const char *name,char *out,unsigned size) {
 out[0]=0;
 if(!task_lock_try(kernel_task))return -1;
 int found=-1;thread_t thread;
 queue_iterate(&kernel_task->threads,thread,thread_t,task_threads) {
  char tn[64];tn[0]=0;thread_get_thread_name(thread,tn);
  if(strcmp(tn,name))continue;
  uint64_t frames[BT_KFRAMES];unsigned state=0,hint=0,stack=0;int pri=0,runq=-1,last=-1,bound=-1,chosen=-1;uint64_t event=0;
  bool irq=ml_set_interrupts_enabled(false);
  if(simple_lock_try(&thread->sched_lock,&thread_lck_grp)) {
   bt_kernel_stack(thread,frames);state=(unsigned)thread->state;hint=(unsigned)thread->block_hint;pri=thread->sched_pri;event=(uint64_t)thread->wait_event;
   processor_t q=thread_get_runq_locked(thread);if(q)runq=q->cpu_id;
   if(thread->last_processor)last=thread->last_processor->cpu_id;
   if(thread->bound_processor)bound=thread->bound_processor->cpu_id;
   if(thread->chosen_processor)chosen=thread->chosen_processor->cpu_id;
   stack=thread->kernel_stack!=0;
   thread_unlock(thread);found=0;
  }
  ml_set_interrupts_enabled(irq);
  if(found)break;
  // runq: the processor whose run queue holds it (-1 none); last/bound/chosen: processors by cpu id; stk: whether it owns a kernel stack
  // (a thread blocked with a continuation gives its stack away, and its frames are then another thread's)
  int at=snprintf(out,size,"st=%x hint=%u pri=%d ev=%llx runq %d last %d bound %d chosen %d stk %u k",state,hint,pri,
   (unsigned long long)VM_KERNEL_UNSLIDE_OR_PERM(event),runq,last,bound,chosen,stack);
  for(unsigned i=0;i<BT_KFRAMES && frames[i] && at>0 && (unsigned)at<size;++i)at+=snprintf(out+at,size-(unsigned)at," %llx",(unsigned long long)VM_KERNEL_UNSLIDE(frames[i]));
  break;
 }
 task_unlock(kernel_task);
 return found;
}
// Every other processor asked to check its ASTs again, as the scheduler's own preemption signal does. For a thread that was made runnable
// while the processor meant to run it never took the signal. out: per processor its state, the priority it runs at and the signal bits
// still pending before this one was sent (bits that stay set mean a processor that does not take its interrupts).
unsigned boot_cpu_kick(char *out,unsigned size) {
 out[0]=0;int at=0;unsigned sent=0;
 const bool irq=ml_set_interrupts_enabled(false);
 cpu_data_t *self=getCpuDatap();
 for(int i=0;i<=ml_get_max_cpu_number() && at>=0 && (unsigned)at<size;++i) {
  processor_t p=processor_array[i];if(!p)continue;
  cpu_data_t *cd=processor_to_cpu_datap(p);if(!cd)continue;
  const unsigned pending=cd->cpu_signal;
  at+=snprintf(out+at,size-(unsigned)at,"%sc%d s%d p%d g%x",at?" ":"",p->cpu_id,(int)p->state,p->current_pri,pending);
  if(cd!=self && cpu_signal(cd,SIGPast,NULL,NULL)==KERN_SUCCESS)++sent;
 }
 ml_set_interrupts_enabled(irq);
 return sent;
}
