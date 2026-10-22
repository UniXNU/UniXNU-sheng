// SPDX-License-Identifier: MIT
// Observe real EL0 breakpoint state before exception delivery changes it.
// No allocation, process traversal, user-memory access or blocking in the trap.
#include <kern/task.h>
#include <kern/thread.h>
#include <arm/thread.h>
#include <kern/misc_protos.h>
#include <kdp/kdp_dyld.h>
#include <string.h>
#include <vm/vm_map.h>
extern int proc_selfpid(void);
extern void proc_selfname(char *, int);

struct boot_fault_record {
 unsigned valid;
 int pid;
 char name[32];
 uint64_t pc,lr,sp,fp,esr,far,slide,x[29];
};
static struct boot_fault_record faults[8];
static unsigned fault_lock,fault_count;

static void capture_exception(void *saved,unsigned esr,uint64_t far) {
 arm_saved_state_t *state=saved;
 if(__atomic_exchange_n(&fault_lock,1,__ATOMIC_ACQUIRE))return;
 struct boot_fault_record *f=&faults[fault_count++%8];
 f->valid=0;f->pid=proc_selfpid();proc_selfname(f->name,sizeof(f->name));
 f->pc=(uint64_t)get_saved_state_pc(state);f->lr=(uint64_t)get_saved_state_lr(state);
 f->sp=(uint64_t)get_saved_state_sp(state);f->fp=(uint64_t)get_saved_state_fp(state);f->esr=esr;f->far=far;
 f->slide=current_task()->task_shared_region_slide;
 for(unsigned i=0;i<29;++i)f->x[i]=(uint64_t)get_saved_state_reg(state,i);
 f->valid=1;
 __atomic_store_n(&fault_lock,0,__ATOMIC_RELEASE);
}

void boot_progress_user_breakpoint(void *saved,unsigned esr) {capture_exception(saved,esr,0);}
void boot_progress_user_abort(void *saved,unsigned esr,uint64_t far) {capture_exception(saved,esr,far);}
// No user memory access on the ordinary noncritical exit reporting path.
// A captured exception can have a handler; correlate only with a real exit.
void boot_progress_exit_fault(int pid,unsigned signal,char *out,unsigned long capacity) {
 struct boot_fault_record f={0};
 if(__atomic_exchange_n(&fault_lock,1,__ATOMIC_ACQUIRE))return;
 for(unsigned n=0;n<8 && n<fault_count;++n) {
  struct boot_fault_record *candidate=&faults[(fault_count-1-n)%8];
  if(candidate->valid && candidate->pid==pid){f=*candidate;break;}
 }
 __atomic_store_n(&fault_lock,0,__ATOMIC_RELEASE);
 // Signals without a recorded hardware exception (abort(), pthread_kill) take the registers the thread had when it entered the
 // kernel: the exiting thread is the one that died, as in a crash report on the console of other bring-ups.
 if(!f.valid && pid==proc_selfpid()) {
  arm_saved_state_t *state=find_user_regs(current_thread());
  if(state && is_saved_state64(state)) {
   f.valid=1;f.pid=pid;proc_selfname(f.name,sizeof(f.name));
   f.pc=(uint64_t)get_saved_state_pc(state);f.lr=(uint64_t)get_saved_state_lr(state);
   f.sp=(uint64_t)get_saved_state_sp(state);f.fp=(uint64_t)get_saved_state_fp(state);
   f.slide=current_task()->task_shared_region_slide;
  }
 }
 if(!f.valid)return;
 unsigned long n=snprintf(out,capacity,"p%d %.20s sig%u PC%llx LR%llx E%llx F%llx +%llx bt",
  f.pid,f.name,signal,f.pc,f.lr,f.esr,f.far,f.slide);
 if(pid==proc_selfpid()) {
  uint64_t frame=f.fp;
  for(unsigned depth=0;depth<12 && frame && !(frame&15) && n+16<capacity;++depth) {
   uint64_t pair[2];
   if(copyin(frame,pair,sizeof(pair)))break;
   n+=snprintf(out+n,capacity-n," %llx",pair[1]);
   if(pair[0]<=frame || pair[0]-frame>1048576)break;
   frame=pair[0];
  }
 }
}

// Called on the ordinary exit path before coredump/sync, never from panic or
// IRQ context. Only read user memory if the recorded process owns this task.
// Failure to copy is evidence, never a reason to suppress the original panic.
void boot_progress_critical_exit(int pid, char *out, unsigned long capacity) {
 struct boot_fault_record f={0};
 if(__atomic_exchange_n(&fault_lock,1,__ATOMIC_ACQUIRE))return;
 for(unsigned n=0;n<8 && n<fault_count;++n) {
  struct boot_fault_record *candidate=&faults[(fault_count-1-n)%8];
  if(candidate->valid && candidate->pid==pid){f=*candidate;break;}
 }
 __atomic_store_n(&fault_lock,0,__ATOMIC_RELEASE);
 unsigned long used=strlen(out);
 if(used>=capacity)return;
 if(!f.valid){snprintf(out+used,capacity-used,"\nEL0 breakpoint record unavailable");return;}
 uint32_t instruction=0;
 uint64_t frames[6]={0},frame=f.fp;
 char message[161]={0};unsigned long copied=0;
 int instruction_error=-1,message_error=-1,image_error=-1;unsigned depth=0;
 struct user64_dyld_all_image_infos info={0};
 struct user64_dyld_image_info image={0};char image_path[81]={0};
 if(pid==proc_selfpid()) {
  instruction_error=copyin(f.pc,&instruction,sizeof(instruction));
  image_error=copyin(task_get_all_image_info_addr(current_task()),&info,sizeof(info));
  if(!image_error && info.infoArrayCount && info.infoArray) {
   image_error=copyin(info.infoArray,&image,sizeof(image));
   if(!image_error && copyinstr(image.imageFilePath,image_path,sizeof(image_path),&copied))image_path[0]=0;
  }
  for(;depth<3 && frame && !(frame&15);++depth) {
   uint64_t pair[2];
   if(copyin(frame,pair,sizeof(pair)))break;
   frames[depth*2]=pair[0];frames[depth*2+1]=pair[1];
   if(pair[0]<=frame || pair[0]-frame>1048576){++depth;break;}
   frame=pair[0];
  }
  message_error=copyinstr(f.x[0],message,sizeof(message),&copied);
  if(message_error)message[0]=0;
  for(unsigned i=0;message[i];++i)if((unsigned char)message[i]<32 || (unsigned char)message[i]>126)message[i]='?';
 }
 snprintf(out+used,capacity-used,
  "\nEL0 %.31s PC=%llx LR=%llx SP=%llx FP=%llx ESR=%llx cache+=%llx"
  "\nINSN=%x err=%d x0=%llx x1=%llx x2=%llx x3=%llx"
  "\nx19=%llx x20=%llx x21=%llx x22=%llx x23=%llx"
  "\nframes=%u %llx/%llx %llx/%llx %llx/%llx"
  "\nx0str err=%d %.160s\nimage0=%llx err=%d %.80s",
  f.name,f.pc,f.lr,f.sp,f.fp,f.esr,f.slide,instruction,instruction_error,
  f.x[0],f.x[1],f.x[2],f.x[3],f.x[19],f.x[20],f.x[21],f.x[22],f.x[23],
  depth,frames[0],frames[1],frames[2],frames[3],frames[4],frames[5],message_error,message,image.imageLoadAddress,image_error,image_path);
}

// REASON-SCAN (kernel-only; excluded from the host formatting test)
// An aborting process that died of an uncaught exception holds the exception text in its malloc heap ("*** Terminating app due
// to uncaught exception 'X', reason: 'Y'"); the abort itself carries nothing. Scan the writable malloc regions of the exiting
// process (the calling thread owns the map) for the printable run that holds it, as the ios-sweet crash log does.
#define REASON_CHUNK 65536
static char reason_chunk[REASON_CHUNK+1];
static volatile int reason_busy;
// The throw site: the Metal plugin's uncaught-exception handler (drivers/NativeMetal/ExceptionDiag.h) leaves "SHENGTHROW <name> | <reason> | <return
// addresses>" in the malloc heap before the process aborts. The abort itself loses the frames between the dispatch callout and the throw, so this is
// where the exception came from. Filled by the same scan; BootProgress.c reads it right after.
char boot_progress_throw_text[330];
void boot_progress_exit_reason(int pid,unsigned signal,char *out,unsigned long capacity) {
 if(!capacity)return;
 out[0]=0;
 if(signal!=6 || pid!=proc_selfpid() || __atomic_exchange_n(&reason_busy,1,__ATOMIC_ACQUIRE))return;
 vm_map_t map=current_map();
 vm_map_offset_t addr=0;uint64_t scanned=0;int best=0;
 int thrown=0;
 while(scanned<(160ull<<20) && (best<2 || !thrown)) {
  vm_map_size_t size=0;natural_t depth=0;vm_region_submap_info_data_64_t info;
  mach_msg_type_number_t count=VM_REGION_SUBMAP_INFO_V2_COUNT_64;
  if(vm_map_region_recurse_64(map,&addr,&size,&depth,(vm_region_submap_info_64_t)&info,&count)!=KERN_SUCCESS)break;
  vm_map_offset_t base=addr;addr+=size;
  if(info.is_submap || (info.protection&(VM_PROT_READ|VM_PROT_WRITE))!=(VM_PROT_READ|VM_PROT_WRITE) ||
     info.user_tag<1 || info.user_tag>11)continue;
  for(vm_map_offset_t off=0;off<size && (best<2 || !thrown);off+=REASON_CHUNK) {
   unsigned long len=(size-off)<REASON_CHUNK?(unsigned long)(size-off):REASON_CHUNK;
   if(copyin((user_addr_t)(base+off),reason_chunk,len))continue;
   scanned+=len;
   unsigned long start=0;
   for(unsigned long i=0;i<=len;++i) {
    int ok=i<len && (unsigned char)reason_chunk[i]>=0x20 && (unsigned char)reason_chunk[i]<0x7f;
    if(ok)continue;
    unsigned long run=i-start;
    if(run>=24) {
     // Rank 2: the full uncaught-exception line; rank 1: any run with a reason clause.
     char saved=reason_chunk[i];reason_chunk[i]=0;
     if(!thrown && run>=32 && !strncmp(reason_chunk+start,"SHENGTHROW ",11)) {
      unsigned long m=run<sizeof boot_progress_throw_text-1?run:sizeof boot_progress_throw_text-1;
      memcpy(boot_progress_throw_text,reason_chunk+start,m);boot_progress_throw_text[m]=0;thrown=1;
     }
     int rank=strnstr(reason_chunk+start,"uncaught exception",run)?2:strnstr(reason_chunk+start,"reason: ",run)?1:0;
     if(rank>best) {
      unsigned long m=run<capacity-1?run:capacity-1;
      memcpy(out,reason_chunk+start,m);out[m]=0;best=rank;
     }
     reason_chunk[i]=saved;
     if(best>=2 && thrown)break;
    }
    start=i+1;
   }
  }
 }
 __atomic_store_n(&reason_busy,0,__ATOMIC_RELEASE);
}
