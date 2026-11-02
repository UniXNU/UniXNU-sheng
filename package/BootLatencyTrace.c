/* SPDX-License-Identifier: MIT */
#include <kern/thread.h>
#include <kern/task.h>
#include <kern/clock.h>
#include <arm/thread.h>
#include <mach/mach_types.h>
extern int proc_selfpid(void);
struct proc;
extern int proc_pid(struct proc *);
#include <IOKit/IOLib.h>
#include "BootLatencyStore.h"
#include "BootLatencyTrace.h"
extern void *sheng_ipc_trace_task __attribute__((weak_import));
extern void *sheng_ipc_trace_task2 __attribute__((weak_import));
extern void (*sheng_ipc_flow_hook)(const void *,uint32_t,void *,uint32_t,uint32_t) __attribute__((weak_import));
extern void (*sheng_user_fault_hook)(int,const int64_t *,unsigned) __attribute__((weak_import));
enum {BlCapacity=524288};
static struct bl_store store;
void boot_latency_record(uint32_t kind,uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f) {
 if(!__atomic_load_n(&store.slots,__ATOMIC_ACQUIRE))return;
 uint64_t ns=0;absolutetime_to_nanoseconds(mach_absolute_time(),&ns);
 struct bl_record row={ns/1000,thread_tid(current_thread()),kind,(uint32_t)proc_selfpid(),{a,b,c,d,e,f}};
 bl_push(&store,&row);
}
static void ipc_flow(const void *cookie,uint32_t id,void *task,uint32_t receiver,uint32_t phase) {
 void *p=task ? get_bsdtask_info((task_t)task):NULL;
 boot_latency_record(phase,(uintptr_t)cookie,id,receiver,p?(uint32_t)proc_pid(p):0,0,0);
}
static void user_fault(int exception,const int64_t *codes,unsigned count) {
 arm_saved_state_t *state=current_thread()->machine.upcb;
 if(!state || !is_saved_state64(state))return;
 boot_latency_record(3,(uint32_t)exception,count?(uint64_t)codes[0]:0,count>1?(uint64_t)codes[1]:0,
  (uint64_t)get_saved_state_pc(state),(uint64_t)get_saved_state_lr(state),(uint64_t)get_saved_state_sp(state));
}
void boot_latency_start(void) {
 if(store.slots)return;
 struct bl_slot *slots=IOMallocAligned(sizeof(*slots)*BlCapacity,PAGE_SIZE);if(!slots)return;
 memset(slots,0,sizeof(*slots)*BlCapacity);store.capacity=BlCapacity;
 __atomic_store_n(&store.slots,slots,__ATOMIC_RELEASE);
 if(&sheng_ipc_flow_hook)sheng_ipc_flow_hook=ipc_flow;
 if(&sheng_user_fault_hook)sheng_user_fault_hook=user_fault;
}
uint32_t boot_latency_snapshot(void *out,uint32_t bytes) {
 if(!store.slots || bytes<16)return 0;
 uint64_t issued=__atomic_load_n(&store.issued,__ATOMIC_ACQUIRE);
 uint64_t first=issued>BlCapacity?issued-BlCapacity+1:1;
 uint32_t count=0,capacity=(bytes-16)/sizeof(struct bl_record);
 struct bl_record *rows=(struct bl_record *)((char *)out+16);
 for(uint64_t seq=first;seq<=issued && count<capacity;++seq)if(bl_read(&store,seq,&rows[count]))++count;
 uint32_t head[4]={0x3154414c,(uint32_t)issued,count,(uint32_t)__atomic_load_n(&store.dropped,__ATOMIC_RELAXED)};
 memcpy(out,head,16);return 16+count*sizeof(struct bl_record);
}
