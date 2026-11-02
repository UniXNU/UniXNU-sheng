/* SPDX-License-Identifier: MIT */
#include <kern/thread.h>
#include <kern/task.h>
#include <kern/clock.h>
#include <arm/thread.h>
#include <mach/mach_types.h>
#include <vm/vm_map.h>
#if __has_include(<vm/vm_map_xnu.h>)
#include <vm/vm_map_xnu.h>
#endif
#include <kdp/kdp_dyld.h>
extern int proc_selfpid(void);
struct proc;
extern int proc_pid(struct proc *);
#include <IOKit/IOLib.h>
#include "BootLatencyStore.h"
#include "BootLatencyUnwind.h"
#include "BootLatencyTrace.h"
extern void *sheng_ipc_trace_task __attribute__((weak_import));
extern void *sheng_ipc_trace_task2 __attribute__((weak_import));
extern void (*sheng_ipc_flow_hook)(const void *,uint32_t,void *,uint32_t,uint32_t,void *,void *,void *) __attribute__((weak_import));
extern void (*sheng_user_fault_hook)(int,const int64_t *,unsigned) __attribute__((weak_import));
extern void (*native_gpu_dispatch_hook)(uint32_t,const uint64_t *,uint32_t) __attribute__((weak_import));
extern void (*sheng_user_exit_hook)(int,int,uint32_t,uint64_t,const char *,const char *) __attribute__((weak_import));
enum {BlCapacity=524288,FaultSlots=32};
struct fault_slot {uint32_t state,pid;uint64_t id,us,tid,fp,sp,pc,lr,slide;task_t task;};
static struct fault_slot *faults;
static uint64_t fault_sequence;
static struct bl_store store;
static void record_as(uint32_t kind,uint32_t pid,uint64_t tid,uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f) {
 if(!__atomic_load_n(&store.slots,__ATOMIC_ACQUIRE))return;
 uint64_t ns=0;absolutetime_to_nanoseconds(mach_absolute_time(),&ns);
 struct bl_record row={ns/1000,tid,kind,pid,{a,b,c,d,e,f}};bl_push(&store,&row);
}
void boot_latency_record(uint32_t kind,uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f) {
 if(!__atomic_load_n(&store.slots,__ATOMIC_ACQUIRE))return;
 record_as(kind,(uint32_t)proc_selfpid(),thread_tid(current_thread()),a,b,c,d,e,f);
}
int boot_latency_enabled(void) {return __atomic_load_n(&store.slots,__ATOMIC_ACQUIRE)!=NULL;}
static void ipc_flow(const void *cookie,uint32_t id,void *task,uint32_t receiver,uint32_t phase,
                     void *destination,void *port,void *reply) {
 void *p=task ? get_bsdtask_info((task_t)task):NULL;
 boot_latency_record(phase,(uintptr_t)cookie,id,receiver,p?(uint32_t)proc_pid(p):0,(uintptr_t)destination,(uintptr_t)port);
 if(phase==1 && reply)boot_latency_record(13,(uintptr_t)cookie,phase,(uintptr_t)task,(uintptr_t)destination,(uintptr_t)port,(uintptr_t)reply);
}
void boot_latency_waiter(int pid,uint64_t tid,unsigned state,uint64_t wait,unsigned switches,int peer,unsigned peer_code,unsigned error,const uint64_t frames[12],const uint64_t kernel[12]) {
 static uint64_t sequence;const uint64_t id=++sequence;
 record_as(28,(uint32_t)pid,tid,id,state,wait,frames[0],frames[1],switches);
 record_as(30,(uint32_t)pid,tid,id,(uint32_t)peer,peer_code,error,0,0);
 for(unsigned first=0;first<12;first+=4) {
  record_as(29,(uint32_t)pid,tid,id,first,frames[first],frames[first+1],frames[first+2],frames[first+3]);
  record_as(32,(uint32_t)pid,tid,id,first,kernel[first],kernel[first+1],kernel[first+2],kernel[first+3]);
 }
}
static void dispatch_point(uint32_t pid,const uint64_t *v,uint32_t count) {
 if(count==6)record_as(20,pid,thread_tid(current_thread()),v[0],v[1],v[2],v[3],v[4],v[5]);
}
static void user_fault(int exception,const int64_t *codes,unsigned count) {
 arm_saved_state_t *state=current_thread()->machine.upcb;
 if(!state || !is_saved_state64(state))return;
 const uint64_t id=__atomic_add_fetch(&fault_sequence,1,__ATOMIC_RELAXED);
 const uint64_t pc=(uint64_t)get_saved_state_pc(state),lr=(uint64_t)get_saved_state_lr(state),sp=(uint64_t)get_saved_state_sp(state);
 boot_latency_record(3,(uint32_t)exception,count?(uint64_t)codes[0]:0,count>1?(uint64_t)codes[1]:0,pc,lr,sp);
 boot_latency_record(11,id,(uint32_t)exception,count?(uint64_t)codes[0]:0,count>1?(uint64_t)codes[1]:0,pc,lr);
 const uint64_t fp=(uint64_t)get_saved_state_fp(state),slide=current_task()->task_shared_region_slide;
 boot_latency_record(6,id,fp,sp,(uint64_t)get_saved_state_far(state),(uint64_t)get_saved_state_esr(state),slide);
 for(unsigned first=0;first<29;first+=4) {
  uint64_t x[4]={0};for(unsigned n=0;n<4 && first+n<29;++n)x[n]=(uint64_t)get_saved_state_reg(state,first+n);
  boot_latency_record(7,id,first,x[0],x[1],x[2],x[3]);
 }
 /* Retain only the task here. Faultable stack reads run on the capture worker. */
 for(unsigned i=0;faults && i<FaultSlots;++i) {
  struct fault_slot *slot=&faults[i];uint32_t empty=0;
  if(!__atomic_compare_exchange_n(&slot->state,&empty,1,0,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED))continue;
  uint64_t ns=0;absolutetime_to_nanoseconds(mach_absolute_time(),&ns);
  slot->id=id;slot->us=ns/1000;slot->pid=(uint32_t)proc_selfpid();slot->tid=thread_tid(current_thread());
  slot->fp=fp;slot->sp=sp;slot->pc=pc;slot->lr=lr;slot->slide=slide;slot->task=current_task();task_reference(slot->task);
  __atomic_store_n(&slot->state,2,__ATOMIC_RELEASE);return;
 }
 boot_latency_record(12,id,16,0,0,0,0); /* bounded queue full or unavailable */
}
static void user_exit(int pid,int status,uint32_t reason_namespace,uint64_t reason_code,const char *name,const char *description) {
 char text[96]={0};if(description)strlcpy(text,description,sizeof text);
 for(unsigned part=0;part<4;++part){uint64_t words[3]={0};memcpy(words,text+part*24,24);boot_latency_record(23,(uint32_t)pid,(uint32_t)status,part,words[0],words[1],words[2]);}
 uint64_t name_words[3]={0};if(name)memcpy(name_words,name,strnlen(name,23));
 boot_latency_record(24,(uint32_t)pid,0,0,name_words[0],name_words[1],name_words[2]);
 boot_latency_record(22,(uint32_t)pid,(uint32_t)status,reason_namespace,reason_code,(uint32_t)proc_selfpid()==(uint32_t)pid,0);
 if((status&127) && (uint32_t)proc_selfpid()==(uint32_t)pid) {
  int64_t codes[2]={(uint32_t)status,(int64_t)reason_code};user_fault(0x100|(status&127),codes,2);
 }
}
static int read_fault_frame(void *context,uint64_t fp,uint64_t pair[2]) {return vm_map_read_user((vm_map_t)context,fp,pair,16);}
void boot_latency_fault_drain(void) {
 unsigned processed=0;
 for(unsigned i=0;faults && i<FaultSlots && processed<2;++i) {
  struct fault_slot *s=&faults[i];uint32_t ready=2;
  if(!__atomic_compare_exchange_n(&s->state,&ready,3,0,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED))continue;
  ++processed;
  vm_map_t map=get_task_map_reference(s->task);uint64_t frames[32]={0};
  struct bl_unwind_result walk={2,8,0};frames[0]=s->pc;frames[1]=s->lr;
  if(map)walk=bl_unwind(s->fp,s->sp,s->pc,s->lr,frames,32,read_fault_frame,map);
  unsigned depth=walk.count,status=walk.flags;
  kern_return_t error=walk.error,stack_error=KERN_SUCCESS;
  for(unsigned first=0;first<depth;first+=4) {
   uint64_t v[4]={0};for(unsigned n=0;n<4 && first+n<depth;++n)v[n]=frames[first+n];
   record_as(8,s->pid,s->tid,s->id,first,v[0],v[1],v[2],v[3]);
  }
  uint64_t words[16]={0};
  if(map)stack_error=vm_map_read_user(map,s->sp&0x00007fffffffffffULL,words,sizeof words);else stack_error=KERN_INVALID_ADDRESS;
  if(stack_error==KERN_SUCCESS)for(unsigned first=0;first<16;first+=4)
   record_as(10,s->pid,s->tid,s->id,first,words[first],words[first+1],words[first+2],words[first+3]);
  uint64_t info_addr=task_get_all_image_info_addr(s->task),main=0;struct user64_dyld_all_image_infos info={0};
  if(map && info_addr && vm_map_read_user(map,info_addr,&info,sizeof info)==KERN_SUCCESS && info.infoArrayCount) {
   struct user64_dyld_image_info image={0};
   if(vm_map_read_user(map,info.infoArray,&image,sizeof image)==KERN_SUCCESS)main=image.imageLoadAddress;
  }
  record_as(9,s->pid,s->tid,s->id,s->slide,info_addr,info.version,info.infoArrayCount,main);
  uint64_t uuid[2]={0};memcpy(uuid,info.sharedCacheUUID,16);
  record_as(25,s->pid,s->tid,s->id,info.sharedCacheBaseAddress,uuid[0],uuid[1],main,0);
  uint64_t uuid_count=info.uuidArrayCount<64?info.uuidArrayCount:64,captured=0;int uuid_error=0;
  for(uint64_t i=0;map && info.uuidArray && i<uuid_count;++i) {
   struct user64_dyld_uuid_info image={0};
   uuid_error=vm_map_read_user(map,info.uuidArray+i*sizeof image,&image,sizeof image);if(uuid_error)break;
   memcpy(uuid,image.imageUUID,16);record_as(26,s->pid,s->tid,s->id,image.imageLoadAddress,uuid[0],uuid[1],i,0);++captured;
  }
  record_as(27,s->pid,s->tid,s->id,info.uuidArrayCount,captured,(uint32_t)uuid_error,64,0);
  uint64_t now=0;absolutetime_to_nanoseconds(mach_absolute_time(),&now);
  record_as(12,s->pid,s->tid,s->id,status,(uint32_t)error,depth,now/1000-s->us,(uint32_t)stack_error);
  if(map)vm_map_deallocate(map);task_deallocate(s->task);s->task=NULL;__atomic_store_n(&s->state,0,__ATOMIC_RELEASE);
 }
}
void boot_latency_start(void) {
 if(store.slots)return;
 struct bl_slot *slots=IOMallocAligned(sizeof(*slots)*BlCapacity,PAGE_SIZE);if(!slots)return;
 memset(slots,0,sizeof(*slots)*BlCapacity);store.capacity=BlCapacity;
 faults=IOMallocAligned(sizeof(*faults)*FaultSlots,PAGE_SIZE);if(faults)memset(faults,0,sizeof(*faults)*FaultSlots);
 __atomic_store_n(&store.slots,slots,__ATOMIC_RELEASE);
 if(&sheng_ipc_flow_hook)sheng_ipc_flow_hook=ipc_flow;
 if(&sheng_user_fault_hook)sheng_user_fault_hook=user_fault;
 if(&sheng_user_exit_hook)sheng_user_exit_hook=user_exit;
 if(&native_gpu_dispatch_hook)native_gpu_dispatch_hook=dispatch_point;
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
