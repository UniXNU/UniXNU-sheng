// SPDX-License-Identifier: BSD-2-Clause-Patent
// Process traversal can wait in proc_iterate despite the bounded thread scan.
// Run capture only in its separate worker. The display takes a nonblocking copy
// of the last complete result; never expose an in-progress row to the renderer.
#include <kern/task.h>
#include <kern/thread.h>
#include <kern/simple_lock.h>
#include <machine/machine_routines.h>
#include <arm/thread.h>
#include <string.h>
#include <pexpert/pexpert.h>
#include <kern/misc_protos.h>
// Exact private process ABI from this source revision's proc.h/proc_internal.h.
// Do not mix BSD and Mach-private header type universes in one translation unit.
struct proc;typedef struct proc *proc_t;
char *proc_best_name(proc_t);
int proc_pid(proc_t);
task_t proc_task(proc_t);
void proc_iterate(unsigned,int (*)(proc_t,void *),void *,int (*)(proc_t,void *),void *);
#define PROC_RETURNED 0
#define PROC_ALLPROCLIST (1U<<0)
#define PROC_NOWAITTRANS (1U<<2)
char boot_process_rows[10][120];
extern void boot_ui_trace_begin(void) __attribute__((weak_import));
extern void boot_ui_trace_task(task_t,unsigned,int,const char *,unsigned) __attribute__((weak_import));
extern void boot_ui_trace_publish(void) __attribute__((weak_import));
extern int boot_ui_trace_role_peer(unsigned) __attribute__((weak_import));

volatile unsigned boot_process_passes,boot_process_seen;
volatile unsigned boot_process_attempts,boot_process_busy;
static char capture_rows[10][120],published_rows[10][120];
static unsigned capture_seen,capture_passes,published_passes,published_seen,publish_lock;
static unsigned chrono_focus_initialized;
static char focus_name[32],focus2_name[32],focus3_name[32],focus4_name[32];
#if XNU_TARGET_OS_OSX
static unsigned trace_slot[10]={0,1,1,1,1};
static const char *targets[]={"launchd","WindowServer","Dock","Finder","loginwindow","lsd","keybagd","cfprefsd","containermanagerd","kernel_task"};
#else
static unsigned trace_slot[10]={0,1,1};
static const char *targets[]={"launchd","backboardd","SpringBoard","PreBoard","runningboardd","lsd","keybagd","cfprefsd","containermanagerd","kernel_task"};
#endif
// boot-ui-follow=1: follow a wait chain by pid. follow_pid[0] is the process the SpringBoard slot sends to (traced in slot 4, role U),
// follow_pid[1] the one that process sends to (traced in slot 1, role B); 0 = nothing to follow.
static unsigned follow_enabled;
static int follow_pid[2];
// SpringBoard and backboardd keep their own slots (2, 3) even when the chain leads to them.
static int followed_slot(proc_t p) {
 int pid=proc_pid(p);const char *name=proc_best_name(p);
 if(!strcmp(name,targets[2]) || !strcmp(name,targets[3]))return 10;
 if(follow_pid[0]>0 && pid==follow_pid[0])return 4;
 if(follow_pid[1]>0 && pid==follow_pid[1])return 1;
 return 10;
}
static int selected(proc_t p,void *arg) {
 (void)arg;
 const char *name=proc_best_name(p);
 for(unsigned i=0;i<10;++i)if(!strcmp(name,targets[i]))return 1;
 return proc_pid(p)==0 || followed_slot(p)<10;
}
static int capture(proc_t p,void *arg) {
 (void)arg;
 const char *name=proc_best_name(p);unsigned index=10;
 for(unsigned i=0;i<10;++i)if(!strcmp(name,targets[i])){index=i;break;}
 if(proc_pid(p)==0)index=9;
 if(followed_slot(p)<10)index=followed_slot(p);
 // A slot the chain uses belongs to the followed process only (runningboardd/PreBoard would mix their threads into its trace).
 else if((index==4 && follow_pid[0]>0) || (index==1 && follow_pid[1]>0))index=10;
 if(index==10)return PROC_RETURNED;
 task_t task=proc_task(p);
 if(!task || !task_lock_try(task)) {
  snprintf(capture_rows[index],120,"%.18s p%d task unavailable/busy",name,proc_pid(p));return PROC_RETURNED;
 }
 unsigned count=0,running=0,waiting=0,busy=0,state=0,sampled=0;int priority=0;
 uint64_t wait=0,continuation=0,userPC=0;
 thread_t thread;
 queue_iterate(&task->threads,thread,thread_t,task_threads){
  if(count++>=128)break;
  bool irq=ml_set_interrupts_enabled(false);
  if(!simple_lock_try(&thread->sched_lock,&thread_lck_grp)){++busy;ml_set_interrupts_enabled(irq);continue;}
  unsigned flags=(unsigned)thread->state;
  running+=(flags&TH_RUN)!=0;waiting+=(flags&TH_WAIT)!=0;
  bool take=count==1;
  if(index==9){char tn[64]={};thread_get_thread_name(thread,tn);take=!strcmp(tn,"VM_pageout_scan");}
  if(take){
   sampled=1;state=flags;priority=thread->sched_pri;wait=thread->wait_event;
   continuation=(uint64_t)thread->continuation;
   if(thread->machine.upcb)userPC=(uint64_t)get_saved_state_pc(thread->machine.upcb);
  }
  thread_unlock(thread);ml_set_interrupts_enabled(irq);
 }
 unsigned total=(unsigned)task->thread_count,slide=task->task_shared_region_slide;task_unlock(task);
 snprintf(capture_rows[index],120,"%.15s p%d n%u r%u w%u b%u t%u s%x/%d W%llx C%llx U%llx +%x",
  name,proc_pid(p),total,running,waiting,busy,sampled,state,priority,
  (unsigned long long)wait,(unsigned long long)continuation,(unsigned long long)userPC,slide);
 if(trace_slot[index] && boot_ui_trace_task)boot_ui_trace_task(task,index,proc_pid(p),name,slide);
 capture_seen=capture_seen+1;return PROC_RETURNED;
}
void boot_process_capture(void) {
 // Exactly one capture worker. Refuse reentry without waiting or modifying its
 // partial results, including in host failure-injection checks.
 if(__atomic_exchange_n(&boot_process_busy,1,__ATOMIC_ACQUIRE))return;
 __atomic_fetch_add(&boot_process_attempts,1,__ATOMIC_RELAXED);
 if(!chrono_focus_initialized) {
  unsigned focus=0;PE_parse_boot_argn("boot-ui-chrono",&focus,sizeof(focus));
  if(focus==1){targets[1]="chronod";targets[3]="backboardd";}
  // boot-ui-focus=<process> replaces the sampled second slot; the chronod flag above stays as a fallback.
  if(PE_parse_boot_argn("boot-ui-focus",focus_name,sizeof(focus_name)) && focus_name[0]) {
   focus_name[sizeof(focus_name)-1]=0;targets[1]=focus_name;
#if !XNU_TARGET_OS_OSX
   targets[3]="backboardd";
#endif
  }
  // boot-ui-focus2=<process> likewise replaces the third slot (normally SpringBoard, role S).
  if(PE_parse_boot_argn("boot-ui-focus2",focus2_name,sizeof(focus2_name)) && focus2_name[0]) {
   focus2_name[sizeof(focus2_name)-1]=0;targets[2]=focus2_name;
  }
  // boot-ui-focus3/4 add two more traced slots (replacing the PreBoard/runningboardd rows).
  if(PE_parse_boot_argn("boot-ui-focus3",focus3_name,sizeof(focus3_name)) && focus3_name[0]) {
   focus3_name[sizeof(focus3_name)-1]=0;targets[3]=focus3_name;trace_slot[3]=1;
  }
  if(PE_parse_boot_argn("boot-ui-focus4",focus4_name,sizeof(focus4_name)) && focus4_name[0]) {
   focus4_name[sizeof(focus4_name)-1]=0;targets[4]=focus4_name;trace_slot[4]=1;
  }
  PE_parse_boot_argn("boot-ui-follow",&follow_enabled,sizeof(follow_enabled));
  if(follow_enabled==1)trace_slot[4]=trace_slot[1]=1;
  chrono_focus_initialized=1;
 }
 // The previous pass's traces name the next processes to follow (a busy trace keeps the previous choice).
 if(follow_enabled==1 && boot_ui_trace_role_peer) {
  int first=boot_ui_trace_role_peer(1),second=boot_ui_trace_role_peer(3);
  if(first>=0) {
   if(first!=follow_pid[0])second=0;   // slot 4's trace belongs to the previous choice
   follow_pid[0]=first;
   if(second>=0)follow_pid[1]=first>0 && second!=first?second:0;
  }
 }
 for(unsigned i=0;i<10;++i)snprintf(capture_rows[i],120,"%.17s: absent from this process pass",targets[i]);
 capture_seen=0;
 if(boot_ui_trace_begin)boot_ui_trace_begin();
 // No waiting for exec/exit transitions; callouts run outside proc_list_lock.
 proc_iterate(PROC_ALLPROCLIST|PROC_NOWAITTRANS,capture,NULL,selected,NULL);
 if(boot_ui_trace_publish)boot_ui_trace_publish();
 ++capture_passes;
 if(!__atomic_exchange_n(&publish_lock,1,__ATOMIC_ACQUIRE)) {
  memcpy(published_rows,capture_rows,sizeof(published_rows));
  published_passes=capture_passes;published_seen=capture_seen;
  __atomic_store_n(&publish_lock,0,__ATOMIC_RELEASE);
 }
 __atomic_store_n(&boot_process_busy,0,__ATOMIC_RELEASE);
}
void boot_process_snapshot(void) {
 // One reader: the diagnostic monitor. Never allocate, walk processes or wait
 // for the worker; leave the previous complete display snapshot if it is busy.
 if(__atomic_exchange_n(&publish_lock,1,__ATOMIC_ACQUIRE))return;
 memcpy(boot_process_rows,published_rows,sizeof(boot_process_rows));
 boot_process_passes=published_passes;boot_process_seen=published_seen;
 __atomic_store_n(&publish_lock,0,__ATOMIC_RELEASE);
}
