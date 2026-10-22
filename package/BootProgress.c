// SPDX-License-Identifier: MIT
// Opt-in diagnostic screen for this board's bring-up. Uses the existing XNU
// framebuffer mapping and architectural counter; never changes translation,
// storage, signatures, device registers, power, or display mode.
#include <stdint.h>
#include <stddef.h>
#include <pexpert/pexpert.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <kern/sched_prim.h>
#include <kern/sched.h>
#include <kern/host.h>
#include <mach/host_info.h>
extern kern_return_t host_statistics64(host_t host, host_flavor_t flavor, host_info_t info, mach_msg_type_number_t *count);
#include <string.h>
extern void proc_selfname(char *, int);
extern int proc_selfpid(void);
#include <machine/machine_routines.h>
#include <kern/misc_protos.h>
#include <kern/boot_progress.h>
#include <arm/cpu_data_internal.h>
#include <arm/thread.h>
#include <console/video_console.h>
#include "BootProgressCore.h"
#include "BootProgressDisplay.h"
#include "BootProgressPulse.h"
#include "BootProgressLifecycle.h"
static struct bp_exits exit_records,exit_snapshot;
static unsigned exit_lock,exit_dropped;
// Every process name that was exec'd, with its starts, its exits and the second of its first start. The exit records keep only failed exits,
// so a service that starts, works and ends quietly (or never starts) was invisible; this says which of the system's helpers ran at all (int-201:
// the wallpaper's extension, Safari's and the widgets' processes).
static unsigned seconds(void);
enum {BpCensus = 448};   // every process name of a boot (int-213: 389 names, 56 kept)
static struct {char name[24];unsigned short started,exited,first;} census[BpCensus];
static unsigned census_count,census_lock,census_dropped;
static void census_note(const char *name,int exited) {
  if(!name || !name[0])return;
  if(__atomic_exchange_n(&census_lock,1,__ATOMIC_ACQUIRE)) {__atomic_fetch_add(&census_dropped,1,__ATOMIC_RELAXED);return;}
  unsigned i=0;
  while(i<census_count && strncmp(census[i].name,name,sizeof census[i].name-1))++i;
  if(i==census_count) {
    if(census_count>=BpCensus) {__atomic_fetch_add(&census_dropped,1,__ATOMIC_RELAXED);__atomic_store_n(&census_lock,0,__ATOMIC_RELEASE);return;}
    strlcpy(census[i].name,name,sizeof census[i].name);census[i].first=(unsigned short)seconds();++census_count;
  }
  if(exited) {if(census[i].exited<65535)++census[i].exited;} else if(census[i].started<65535)++census[i].started;
  __atomic_store_n(&census_lock,0,__ATOMIC_RELEASE);
}

// Every xpcproxy that ended without exec'ing its job (a job launch that failed in its spawn setup): the job's label from argv, how often, and the
// last exit status and exit-reason namespace/code (int-214: 219 such exits in 4 minutes, launchd the busiest process; the wallpaper's and the
// widgets' extensions are launched this way).
extern int proc_selfexecutableargs(uint8_t *buf,size_t *buflen);
enum {BpXpcFails = 32};
static struct {char label[64];unsigned short count;int status;unsigned ns;uint64_t code;} xpc_fails[BpXpcFails];
static unsigned xpc_fail_count,xpc_fail_lock,xpc_fail_dropped;
static uint8_t xpc_args[512];
static void xpc_fail_note(int status,unsigned ns,uint64_t code) {
  char label[64]="?";
  if(!__atomic_exchange_n(&xpc_fail_lock,1,__ATOMIC_ACQUIRE)) {
    // argc, then the exec path and argv strings: the label is the last of the first three strings (path, "xpcproxy", label).
    size_t length=sizeof xpc_args-1;
    if(!proc_selfexecutableargs(xpc_args,&length) && length>4) {
      xpc_args[length<sizeof xpc_args?length:sizeof xpc_args-1]=0;
      unsigned found=0;
      for(size_t at=4;at<length && found<3;) {
        if(!xpc_args[at]){++at;continue;}
        const char *text=(const char *)xpc_args+at;
        if(++found>1)strlcpy(label,text,sizeof label);
        at+=strnlen(text,length-at)+1;
      }
    }
    unsigned i=0;
    while(i<xpc_fail_count && strcmp(xpc_fails[i].label,label))++i;
    if(i==xpc_fail_count && xpc_fail_count<BpXpcFails) {strlcpy(xpc_fails[i].label,label,sizeof xpc_fails[i].label);++xpc_fail_count;}
    if(i<xpc_fail_count) {
      if(xpc_fails[i].count<65535)++xpc_fails[i].count;
      xpc_fails[i].status=status;xpc_fails[i].ns=ns;xpc_fails[i].code=code;
    } else ++xpc_fail_dropped;
    __atomic_store_n(&xpc_fail_lock,0,__ATOMIC_RELEASE);
  } else __atomic_fetch_add(&xpc_fail_dropped,1,__ATOMIC_RELAXED);
}

static struct bp_display display;
static struct bp_core state, render_snapshot;
static unsigned render_busy;
// boot-diag-resume=<seconds>: after the OS stops scanning out frames for that long, take the panel back for
// the diagnostic page. Any new validated scanout yields it again through boot_progress_prepare_scanout.
static unsigned resume_after,last_scanout_at;
static char build_tag[32];
const char *boot_progress_tag(void) { return build_tag; }
static struct bp_pulse pulse;
struct bp_panic_info boot_progress_panic_vinfo;
// The panic screen (scripts/xnu-panic-screen.c) also copies the formatted panic message here (first panic only, NUL-terminated):
// a panic on a secondary core parks just that core, the next swap paints over the panic screen, and the live banner keeps the text.
char boot_progress_panic_text[256];
_Static_assert(offsetof(struct bp_panic_info,height)==offsetof(struct vc_info,v_height),"panic height ABI");
_Static_assert(offsetof(struct bp_panic_info,width)==offsetof(struct vc_info,v_width),"panic width ABI");
_Static_assert(offsetof(struct bp_panic_info,depth)==offsetof(struct vc_info,v_depth),"panic depth ABI");
_Static_assert(offsetof(struct bp_panic_info,stride)==offsetof(struct vc_info,v_rowbytes),"panic stride ABI");
_Static_assert(offsetof(struct bp_panic_info,frame)==offsetof(struct vc_info,v_baseaddr),"panic address ABI");
static unsigned pulse_worker_status,bind_count,bind_result=3,bind_depth;
static uintptr_t bind_base;
static uint64_t irq_entries,irq_entry_pc;
static unsigned enabled, busy, started, last_draw = ~0U;
uint64_t boot_progress_epoch, boot_progress_frequency;
static uint64_t irq_count, thread_count;
static uint64_t last_pc, last_psr, last_sp;
struct pc_sample { uint64_t pc, psr; unsigned second; int pid; char name[64]; };
static struct pc_sample samples[8];
static unsigned sample_count, sample_second = ~0U;
volatile unsigned boot_progress_frozen, boot_progress_reason;
volatile unsigned boot_progress_display_owned;
volatile uint64_t boot_progress_last_pc;
volatile uint64_t boot_progress_observation[24];
__attribute__((noinline)) void boot_progress_stop_checkpoint(void) { __asm__ volatile("" ::: "memory"); }

extern volatile uint64_t native_ufs_block_reads, native_ufs_verified_blocks;
extern volatile uint64_t native_ufs_verify_ticks __attribute__((weak_import)), native_ufs_read_ticks __attribute__((weak_import));
extern volatile uint64_t native_ufs_block_failures;
extern volatile uint32_t native_ufs_integrity_failures, native_ufs_blocks_published;
extern volatile uint32_t native_ufs_probe_calls __attribute__((weak_import)), native_ufs_matched __attribute__((weak_import));
extern volatile uint32_t native_ufs_root_step __attribute__((weak_import));
extern const char *volatile native_ufs_root_failure __attribute__((weak_import));
extern volatile uint32_t native_virtio_ready, native_virtio_read_requests, native_virtio_read_failures;
extern volatile uint64_t native_virtio_verified_blocks;
extern void native_ufs_progress_text(char *, size_t);
extern unsigned int vm_page_free_count;
extern unsigned int vm_page_active_count,vm_page_inactive_count,vm_page_wire_count,vm_page_speculative_count;
extern uint64_t max_mem;
extern volatile uint32_t native_ram_block_sparse __attribute__((weak_import));
extern volatile uint64_t native_ram_block_bytes,native_ram_block_resident_bytes __attribute__((weak_import));
extern unsigned int vm_page_free_min,vm_page_free_reserved,vm_page_free_target;
extern void boot_progress_exit_fault(int,unsigned,char *,unsigned long) __attribute__((weak_import));
extern void boot_progress_exit_reason(int,unsigned,char *,unsigned long) __attribute__((weak_import));
extern char boot_progress_throw_text[330] __attribute__((weak_import));
extern void boot_ui_trace_snapshot(unsigned) __attribute__((weak_import));
extern char boot_ui_trace_rows[14][120] __attribute__((weak_import));
extern char boot_ui_trace_label[120] __attribute__((weak_import));
extern unsigned boot_ui_trace_ready __attribute__((weak_import));
extern volatile unsigned boot_ui_trace_phase __attribute__((weak_import));
extern volatile unsigned boot_ui_trace_index __attribute__((weak_import));
extern void boot_process_capture(void) __attribute__((weak_import));
extern void boot_process_snapshot(void) __attribute__((weak_import));
extern void boot_log_tick(unsigned) __attribute__((weak_import));
extern void sheng_cpufreq_poll(void) __attribute__((weak_import));
extern char boot_process_rows[10][120] __attribute__((weak_import));
extern unsigned boot_ui_trace_role_rows(unsigned,char [][120],unsigned,unsigned) __attribute__((weak_import));
extern char native_touch_rows[4][120] __attribute__((weak_import));
extern char native_cpufreq_row[120] __attribute__((weak_import));
extern volatile uint32_t native_hid_input_events[8] __attribute__((weak_import));
extern volatile int native_hid_input_results[8] __attribute__((weak_import));
extern volatile uint32_t native_touch_loop_step __attribute__((weak_import));
extern volatile uint32_t native_touch_seq,native_touch_count __attribute__((weak_import));
extern volatile int native_touch_xs[12],native_touch_ys[12],native_touch_ids[12] __attribute__((weak_import));
extern char *debug_buf_base __attribute__((weak_import));
extern unsigned int kernel_debugger_entry_count __attribute__((weak_import));
extern volatile uint32_t native_cpufreq_stage __attribute__((weak_import));
extern volatile unsigned native_touch_stage_req __attribute__((weak_import));
extern volatile unsigned boot_process_passes,boot_process_seen __attribute__((weak_import));
extern volatile unsigned boot_process_attempts __attribute__((weak_import));
extern volatile unsigned boot_process_busy __attribute__((weak_import));
static unsigned process_worker_status,process_capture_age;
extern volatile uint32_t __attribute__((weak_import)) armcpu_smp_requested,armcpu_smp_running,armcpu_smp_conduit,armcpu_boot_mpidr;
extern volatile uint32_t __attribute__((weak_import)) armcpu_smp_step[8],armcpu_smp_seen_mpidr[8],armcpu_smp_fail[8],armcpu_smp_sys[8][5],armcpu_boot_sys[5],armcpu_gic_stride,armcpu_gic_typer_probe[2];
extern volatile int32_t __attribute__((weak_import)) armcpu_smp_psci_rc[8];
extern volatile uint32_t __attribute__((weak_import)) armcpu_irq_phase,armcpu_irq_id,armcpu_quiesce_calls,armcpu_halt_calls;
extern int nprocs;
extern volatile int native_owner_root_accepted;

extern volatile uint32_t native_legacy_stage __attribute__((weak_import));
extern volatile uint32_t native_legacy_pm_stage __attribute__((weak_import));
extern volatile uint32_t native_legacy_pm_calls __attribute__((weak_import));
extern volatile uint32_t native_legacy_pm_callbacks __attribute__((weak_import));
extern volatile uint32_t native_legacy_swap_validation_calls __attribute__((weak_import));
extern volatile uint32_t native_legacy_swap_validation_result __attribute__((weak_import));
extern volatile uint32_t native_legacy_swap_validation_reason __attribute__((weak_import));
extern volatile uint32_t native_legacy_swap_accepted_count __attribute__((weak_import));
extern volatile uint32_t native_legacy_swap_rejected_count __attribute__((weak_import));
extern volatile uint64_t native_legacy_rej_surface[7] __attribute__((weak_import));
extern volatile uint32_t native_legacy_rej_reason __attribute__((weak_import));
extern volatile uint64_t native_legacy_rej_surfaces[4] __attribute__((weak_import));
extern volatile uint32_t native_legacy_rej_state[3] __attribute__((weak_import));
extern volatile uint32_t native_legacy_rej_options[3] __attribute__((weak_import));
extern volatile uint32_t native_legacy_rej_size[2] __attribute__((weak_import));
extern volatile uint32_t native_legacy_rej_rects[2][4] __attribute__((weak_import));
extern char native_legacy_rej_class[32] __attribute__((weak_import));
extern volatile uint32_t native_legacy_frame_count __attribute__((weak_import));
extern volatile uint32_t native_legacy_frame_kind[32] __attribute__((weak_import));
extern volatile uint32_t native_legacy_frame_time[32] __attribute__((weak_import));
extern volatile uint64_t native_legacy_frame_stats[32][5] __attribute__((weak_import));
extern volatile uint32_t native_legacy_frame_colors[8][4] __attribute__((weak_import));
extern volatile uint64_t native_legacy_frame_color_counts[8][4] __attribute__((weak_import));
extern volatile uint64_t native_legacy_frame_color_other[8] __attribute__((weak_import));
extern volatile uint32_t native_legacy_transfer_calls __attribute__((weak_import));
extern volatile uint32_t native_legacy_retirement_calls __attribute__((weak_import));
extern volatile uint32_t native_legacy_guard_slot __attribute__((weak_import));
extern volatile uint32_t native_legacy_prepare_calls __attribute__((weak_import));
extern volatile uint32_t native_legacy_prepare_reason __attribute__((weak_import));
extern volatile uint32_t native_legacy_prepare_attempts __attribute__((weak_import));
extern volatile uint32_t native_legacy_client_calls __attribute__((weak_import));
extern volatile uint32_t native_legacy_client_type __attribute__((weak_import));
extern volatile int native_legacy_client_pid __attribute__((weak_import));
extern volatile int native_legacy_client_status __attribute__((weak_import));
extern char native_legacy_client_class[64] __attribute__((weak_import));
extern volatile uint64_t native_legacy_gop_virtual __attribute__((weak_import));
extern volatile uint64_t native_legacy_gop_physical __attribute__((weak_import));
extern volatile uint64_t native_legacy_gop_bytes __attribute__((weak_import));
extern volatile int native_legacy_transfer_status __attribute__((weak_import));
extern volatile uint64_t native_legacy_transfer_bytes __attribute__((weak_import));
static volatile unsigned touch_passes,touch_dots,touch_pad_w;
static uint64_t ticks(void) {
  uint64_t v; __asm__ volatile("isb\n\tmrs %0, cntpct_el0" : "=r"(v)); return v;
}
static unsigned seconds(void) {
  return boot_progress_frequency ? (unsigned)((ticks() - boot_progress_epoch) / boot_progress_frequency) : 0;
}
static int take(void) {
  if (__atomic_exchange_n(&busy, 1, __ATOMIC_ACQUIRE)) {
    __atomic_fetch_add(&state.dropped, 1, __ATOMIC_RELAXED); return 0;
  }
  return 1;
}
static void release(void) { __atomic_store_n(&busy, 0, __ATOMIC_RELEASE); }
// The text of every page row and of the last banner paint, for the UFS log (BootLog.c).
char boot_progress_page_text[48][BP_COLS];
char boot_progress_banner_text[49][120];
volatile unsigned boot_progress_banner_rows;
static void capture(unsigned row, const char *text) {
  if(row<48){strncpy(boot_progress_page_text[row],text,BP_COLS-1);boot_progress_page_text[row][BP_COLS-1]=0;}
}
static void line(unsigned row, const char *text, uint32_t color) {
  capture(row,text);
  // Reserve the last diagnostic row for a readable independent pulse.
  if(row==47 && __atomic_load_n(&pulse.ready,__ATOMIC_ACQUIRE))return;
  bp_line(&display,row,text,color);
}
// Title bar and section headings: same grid, a tinted row background.
#define BP_BAR_LIVE 0xff1f3a68
#define BP_BAR_STOP 0xff7a1f1f
#define BP_BAR_SECTION 0xff1a2029
static void bar(unsigned row, const char *text, uint32_t color, uint32_t background) {
  capture(row,text);
  if(row==47 && __atomic_load_n(&pulse.ready,__ATOMIC_ACQUIRE))return;
  bp_line_bg(&display,row,text,color,background);
}
static void draw(unsigned now) {
  const struct bp_core *visible=started ? &render_snapshot : &state;
  char b[BP_COLS];
  uint64_t tcr, ttbr0, ttbr1, sctlr;
  __asm__ volatile("mrs %0,tcr_el1" : "=r"(tcr));
  __asm__ volatile("mrs %0,ttbr0_el1" : "=r"(ttbr0));
  __asm__ volatile("mrs %0,ttbr1_el1" : "=r"(ttbr1));
  __asm__ volatile("mrs %0,sctlr_el1" : "=r"(sctlr));
  const uint64_t observation[24]={enabled,display.bound,now,visible->activity_at,
    irq_count,thread_count,visible->dropped,visible->reason,visible->reads,visible->verified,
    visible->failures,last_pc,last_psr,last_sp,tcr,ttbr0,ttbr1,sctlr,
    visible->history_count,visible->log_lines,display.width,display.height,display.stride,display.frame};
  for(unsigned i=0;i<24;++i) boot_progress_observation[i]=observation[i];
  const char *title = visible->continuous && visible->reason ? "LIVE: DEADLINE EXCEEDED - OS and diagnostic recording continue" :
          visible->reason == 1 ? "STOP: NO STAGE / IO ACTIVITY FOR 30s - diagnostic snapshot" :
          visible->reason == 2 ? "STOP: 120s OBSERVATION LIMIT - activity does not prove forward progress" :
          "UniXNU boot diagnostics - live XNU - native 4K - no automatic reboot";
  // Session/build label from the sheng-build-tag boot argument (e.g. ks-001, gpu-003), so a photograph says which image it is.
  if(build_tag[0]) snprintf(b,sizeof(b)," [%s] %s",build_tag,title); else snprintf(b,sizeof(b)," %s",title);
  bar(0, b, 0xffffffff, visible->reason && !visible->continuous ? BP_BAR_STOP : BP_BAR_LIVE);
  snprintf(b,sizeof(b),"time=%us idle=%us IRQ=%llu monitor=%llu lost=%u root=%d procs=%d free=%u",
    now, now-visible->activity_at, irq_count, thread_count, visible->dropped,
    native_owner_root_accepted, nprocs, vm_page_free_count); line(1,b,0xffffffff);
  snprintf(b,sizeof(b),"%s requests=%llu verified4K=%llu failures=%llu integrity=%u published=%u",
    native_virtio_ready && !native_ufs_blocks_published ? "OFFLINE VIRTIO" : "UFS",
    visible->reads,visible->verified,visible->failures,native_ufs_integrity_failures,native_ufs_blocks_published);
  if(&native_ufs_root_step && !native_ufs_blocks_published) {
    // Nothing published yet: how far the root start got (probe calls, match, step, last soft-failure reason).
    size_t n=strlen(b);const char *why=native_ufs_root_failure;
    snprintf(b+n,sizeof(b)-n," | probe=%u match=%u root=%u %.40s",native_ufs_probe_calls,native_ufs_matched,
      native_ufs_root_step,why?why:"-");
  }
  line(2,b,0xffffffff);
  snprintf(b,sizeof(b),"TCR=%llx TTBR0=%llx TTBR1=%llx SCTLR=%llx",tcr,ttbr0,ttbr1,sctlr); line(3,b,0xffffffff);
  snprintf(b,sizeof(b),"last IRQ PC=%llx PSR=%llx SP=%llx (sample, not a fault diagnosis)",last_pc,last_psr,last_sp); line(4,b,0xffffffff);
  unsigned latest = visible->history_count ? (visible->history_count-1)%BP_HISTORY : 0;
  snprintf(b,sizeof(b),"stage: %.106s",visible->history[latest].text); line(5,b,0xff00ffff);
  if(!__atomic_exchange_n(&exit_lock,1,__ATOMIC_ACQUIRE)) {
    exit_snapshot=exit_records;
    __atomic_store_n(&exit_lock,0,__ATOMIC_RELEASE);
  }
  const int trace_visible=&boot_ui_trace_ready && boot_ui_trace_ready;
  if(!trace_visible) {
  unsigned first = visible->history_count > BP_HISTORY ? visible->history_count-BP_HISTORY : 0;
  for (unsigned i=0;i<BP_HISTORY;++i) {
    b[0]=0;
    if (first+i < visible->history_count) {
      const struct bp_history *h=&visible->history[(first+i)%BP_HISTORY];
      snprintf(b,sizeof(b),"%3us %.110s",h->seconds,h->text);
    }
    line(6+i,b,0xff00ffff);
  }
  first = sample_count > 8 ? sample_count-8 : 0;
  for (unsigned i=0;i<4;++i) {
    struct pc_sample *a=&samples[(first+i*2)%8],*c=&samples[(first+i*2+1)%8];
    snprintf(b,sizeof(b),"PC %us p%d %.10s %llx/%x | %us p%d %.10s %llx/%x",a->second,a->pid,a->name,a->pc,(unsigned)a->psr,c->second,c->pid,c->name,c->pc,(unsigned)c->psr);
    line(12+i,b,0xffaaaaaa);
  }
  bar(16," Kernel console (latest lines; an IRQ-off hard lock stops refresh)",0xffffff00,BP_BAR_SECTION);
  {
    unsigned last=visible->logs[visible->log_row][0] ? visible->log_row : (visible->log_row+BP_LOGS-1)%BP_LOGS;
    for (unsigned i=0;i<7;++i) line(17+i,visible->logs[(last+BP_LOGS-6+i)%BP_LOGS],0xffffffff);
  }
  if(exit_snapshot.total) {
    snprintf(b,sizeof(b)," Process exits: total=%u nonzero-status-or-reason=%u dropped=%u; FAIL rows persist across normal exits",
      exit_snapshot.total,exit_snapshot.failed,__atomic_load_n(&exit_dropped,__ATOMIC_RELAXED));
    bar(16,b,0xffffff00,BP_BAR_SECTION);
    for(unsigned i=0;i<3;++i) {
      unsigned n=exit_snapshot.failed>3?exit_snapshot.failed-3:0;
      line(17+i,n+i<exit_snapshot.failed?exit_snapshot.failures[(n+i)%3]:"",0xffffaa00);
    }
    for(unsigned i=0;i<4;++i) {
      unsigned n=exit_snapshot.total>4?exit_snapshot.total-4:0;
      line(20+i,n+i<exit_snapshot.total?exit_snapshot.recent[(n+i)%4]:"",0xffffffff);
    }
  }
  }
  if(trace_visible) {
    for(unsigned i=0;i<14;++i)line(6+i,boot_ui_trace_rows[i],0xffaaffff);
    line(20,boot_ui_trace_label,0xffffff00);
    unsigned retained=exit_snapshot.crashes?exit_snapshot.crashes:exit_snapshot.failed;
    for(unsigned i=0;i<3;++i) {
      unsigned n=retained>3?retained-3:0;
      line(21+i,n+i<retained?(exit_snapshot.crashes?exit_snapshot.crash[(n+i)%3]:exit_snapshot.failures[(n+i)%3]):"No additional nonzero exit status/reason recorded",0xffffaa00);
    }
    if(&native_legacy_frame_count && native_legacy_frame_count>1) {
      // Dominant pixel values (BGRA word:count) of presented frames 2..5, two frames per row.
      for(unsigned r=0;r<2;++r) {
        char *w=b;size_t left=sizeof(b);b[0]=0;
        for(unsigned c=0;c<2;++c) {
          unsigned i=1+r*2+c;if(i>=native_legacy_frame_count || i>=8)break;
          int n=snprintf(w,left,"F%u %08x:%llu %08x:%llu o=%llu | ",i+1,(unsigned)native_legacy_frame_colors[i][0],
            native_legacy_frame_color_counts[i][0],(unsigned)native_legacy_frame_colors[i][1],
            native_legacy_frame_color_counts[i][1],native_legacy_frame_color_other[i]);
          if(n<0 || (size_t)n>=left)break;w+=n;left-=(size_t)n;
        }
        line(21+r,b,0xff00ff88);
      }
    }
  }
  if(&boot_process_rows){
    for(unsigned i=0;i<10;++i)line(i<7?24+i:38+i-7,boot_process_rows[i],0xff00ffff);
    snprintf(b,sizeof(b),"%s passes=%u seen=%u worker=%u attempt=%u busy=%u age=%us; t=sample s=state/pri W=event C=cont U=user PC +=slide",
      process_capture_age>=5?"PROC DELAY":"PROC",boot_process_passes,boot_process_seen,__atomic_load_n(&process_worker_status,__ATOMIC_ACQUIRE),
      &boot_process_attempts?__atomic_load_n(&boot_process_attempts,__ATOMIC_RELAXED):0,
      &boot_process_busy?__atomic_load_n(&boot_process_busy,__ATOMIC_ACQUIRE):0,process_capture_age);
    if(&boot_ui_trace_phase)snprintf(b,sizeof(b),
      "PROC pass%u seen%u worker%u attempt%u busy%u age%us; trace phase%u index%u; EXIT %u nonzero%u crashsig%u drop%u",
      boot_process_passes,boot_process_seen,process_worker_status,boot_process_attempts,boot_process_busy,process_capture_age,
      boot_ui_trace_phase,boot_ui_trace_index,exit_snapshot.total,exit_snapshot.failed,exit_snapshot.crashes,exit_dropped);
    line(41,b,process_capture_age>=5?0xffff6600:0xffaaaaaa);
  }
  if(&armcpu_irq_phase){
    uint64_t vct,cval,ctl,daif;
    __asm__ volatile("mrs %0,cntvct_el0" : "=r"(vct));
    __asm__ volatile("mrs %0,cntv_cval_el0" : "=r"(cval));
    __asm__ volatile("mrs %0,cntv_ctl_el0" : "=r"(ctl));
    __asm__ volatile("mrs %0,daif" : "=r"(daif));
    snprintf(b,sizeof(b),"TIMER vct=%llx cval=%llx ctl=%llx DAIF=%llx (read-only monitor sample)",vct,cval,ctl,daif);
    if(!(&native_touch_stage_req && native_touch_stage_req))line(42,b,0xffaaaaaa);
    snprintf(b,sizeof(b),"CPU IRQ phase=%u id=%u quiesce=%u halt=%u; phase 0=exit 2=clock 4=IPI 6=EOI; sleep-disabled=1",
      armcpu_irq_phase,armcpu_irq_id,armcpu_quiesce_calls,armcpu_halt_calls);line(37,b,0xffffff00);
    if(&armcpu_smp_requested){
      unsigned n=(unsigned)snprintf(b,sizeof(b),"SMP req=%u run=%u cond=%u boot=%x |",
        armcpu_smp_requested,armcpu_smp_running,armcpu_smp_conduit,armcpu_boot_mpidr&0xffffff);
      for(unsigned c=1;c<8&&n<sizeof(b)-14;++c)if(armcpu_smp_step[c]||armcpu_smp_psci_rc[c])
      {
        n+=(unsigned)snprintf(b+n,sizeof(b)-n," c%u=step%u",c,armcpu_smp_step[c]);
        if(armcpu_smp_fail[c]&&n<sizeof(b)-14)n+=(unsigned)snprintf(b+n,sizeof(b)-n,"!fail%u",armcpu_smp_fail[c]);
      }
      line(38,b,0xff00ff88);
      snprintf(b,sizeof(b),"GIC stride=%x last redistributor read: offset=%x typer.lo=%x",
        armcpu_gic_stride,armcpu_gic_typer_probe[0],armcpu_gic_typer_probe[1]);
      line(30,b,0xff00ff88);
    }
    if(&native_cpufreq_stage && native_cpufreq_stage>=2)line(39,native_cpufreq_row,0xff00ccff);
    {
      // The same raw counter delta converted two ways: with the device-tree timebase (what the kernel and user space use)
      // and with CNTFRQ_EL0. They agree only if the device-tree timebase matches the counter. mach_absolute_time() is
      // avoided on purpose: this path runs very early, before per-CPU data exists.
      static uint64_t count0;
      uint64_t counter=ticks(),ns=0;
      if(!count0)count0=counter;
      absolutetime_to_nanoseconds(counter-count0,&ns);
      uint64_t kms=ns/1000000,cms=boot_progress_frequency?(counter-count0)*1000/boot_progress_frequency:0;
      snprintf(b,sizeof(b),"CLK cntfrq=%llu dt-timebase=%llu dt-converted=%llums cntfrq-converted=%llums (equal if the timebase is right)",
        (unsigned long long)boot_progress_frequency,(unsigned long long)gPEClockFrequencyInfo.timebase_frequency_hz,kms,cms);
      line(40,b,0xff00ccff);
    }
    // A panic on a secondary core flashes the panic screen while this monitor keeps running and redraws over it. Keep the
    // first lines of the formatted panic log visible (XNU's debug buffer), so the cause can be read from a later photograph.
    if(&kernel_debugger_entry_count && kernel_debugger_entry_count && &debug_buf_base && debug_buf_base){
      char pb[240];unsigned k=0;
      for(unsigned i=0;i<sizeof(pb)-1&&i<400;++i){char ch=debug_buf_base[i];if(!ch)break;pb[k++]=(ch>=32&&ch<127)?ch:' ';}
      pb[k]=0;
      char l1[118],l2[118];
      unsigned n1=k<110?k:110;for(unsigned i=0;i<n1;++i)l1[i]=pb[i];l1[n1]=0;
      unsigned n2=k>110?k-110:0;if(n2>110)n2=110;for(unsigned i=0;i<n2;++i)l2[i]=pb[110+i];l2[n2]=0;
      bar(14," PANIC LOG (first bytes of the XNU debug buffer):",0xffffffff,BP_BAR_STOP);
      line(15,l1,0xffff8080);
      line(16,l2,0xffff8080);
    }
  }
  if (!visible->frozen && &native_legacy_stage) {
    snprintf(b,sizeof(b),"DISPLAY stage=%u PM=%u calls=%u callbacks=%u guard=%u owned=%u",
      native_legacy_stage,native_legacy_pm_stage,native_legacy_pm_calls,native_legacy_pm_callbacks,
      native_legacy_guard_slot,boot_progress_display_owned);line(31,b,0xffffff00);
    snprintf(b,sizeof(b),"SWAP calls=%u accepted=%u reason=%u COPY calls=%u status=%08x bytes=%llu retired=%u",
      native_legacy_swap_validation_calls,native_legacy_swap_validation_result,native_legacy_swap_validation_reason,
      native_legacy_transfer_calls,(unsigned)native_legacy_transfer_status,native_legacy_transfer_bytes,
      native_legacy_retirement_calls);line(32,b,0xffffff00);
    if(&native_legacy_swap_rejected_count && native_legacy_swap_rejected_count) {
      snprintf(b,sizeof(b),"REJ acc=%u rej=%u r=%u s0=%llx cls=%.24s s1=%llx s2=%llx s3=%llx",
        native_legacy_swap_accepted_count,native_legacy_swap_rejected_count,native_legacy_rej_reason,
        native_legacy_rej_surfaces[0],native_legacy_rej_class,native_legacy_rej_surfaces[1],native_legacy_rej_surfaces[2],native_legacy_rej_surfaces[3]);line(33,b,0xffff6600);
      // The request's two rectangles were the whole display when the surface was judged (reason 4); what matters then is the surface itself.
      snprintf(b,sizeof(b),"REJ st=%x/%x/%x wh=%ux%u SURF %llux%llu fmt=%08llx pl=%llu st=%llu al=%llu ml=%llu",
        native_legacy_rej_state[0],native_legacy_rej_state[1],native_legacy_rej_state[2],
        native_legacy_rej_size[0],native_legacy_rej_size[1],
        (unsigned long long)native_legacy_rej_surface[0],(unsigned long long)native_legacy_rej_surface[1],(unsigned long long)native_legacy_rej_surface[2],
        (unsigned long long)native_legacy_rej_surface[3],(unsigned long long)native_legacy_rej_surface[4],(unsigned long long)native_legacy_rej_surface[5],
        (unsigned long long)native_legacy_rej_surface[6]);line(34,b,0xffff6600);
    } else if(&native_legacy_frame_count && native_legacy_frame_count) {
      // FRAME rows: the last six presented frames (of the first 32 kept): c = copied OS surface, B = blank fill,
      // t = seconds since the first frame, n = nonblack pixels, y = last nonblack row. Total count is in the SWAP row.
      unsigned total=native_legacy_frame_count>32?32:native_legacy_frame_count;
      unsigned first=total>6?total-6:0;
      for(unsigned r=0;r<2;++r) {
        char *w=b;size_t left=sizeof(b);b[0]=0;
        for(unsigned c=0;c<3;++c) {
          unsigned i=first+r*3+c;if(i>=total)break;
          int n=snprintf(w,left,"F%u%c t%u n=%llu y%d | ",i+1,native_legacy_frame_kind[i]?'B':'c',native_legacy_frame_time[i],
            native_legacy_frame_stats[i][0],(int)native_legacy_frame_stats[i][4]);
          if(n<0 || (size_t)n>=left)break;w+=n;left-=(size_t)n;
        }
        line(33+r,b,0xff00ff88);
      }
    } else {
    snprintf(b,sizeof(b),"HANDOFF calls=%u reason=%u attempts=%u (0=OK 1=IRQ 2=masked 3=writer timeout)",
      native_legacy_prepare_calls,native_legacy_prepare_reason,native_legacy_prepare_attempts);line(33,b,0xffffff00);
    if(&native_legacy_client_calls) {
      snprintf(b,sizeof(b),"CLIENT calls=%u pid=%d type=%u status=%08x class=%.32s",
        native_legacy_client_calls,native_legacy_client_pid,native_legacy_client_type,
        (unsigned)native_legacy_client_status,native_legacy_client_class);line(34,b,0xffaaaaaa);
    }
    }
    if(&native_legacy_gop_virtual) {
      snprintf(b,sizeof(b),"GOP actual mapping virtual=%llx physical=%llx bytes=%llu",
        native_legacy_gop_virtual,native_legacy_gop_physical,native_legacy_gop_bytes);
      line(44,b,0xffaaaaaa);
    }
    // Existing passive formatter: cached software fields only, no MMIO/locks.
    char io[1300];native_ufs_progress_text(io,sizeof(io));
    const char *keys[]={"CMD ","QUEUE "};
    for(unsigned j=0;j<2;++j) {
      const char *p=strnstr(io,keys[j],sizeof(io));unsigned n=0;
      if(!p || (j==1 && &native_ufs_verify_ticks))continue;
      while(n<BP_COLS-1 && p[n] && p[n]!='\n'){b[n]=p[n];++n;}
      b[n]=0;line(45+j,b,0xffaaaaaa);
    }
  }
  snprintf(b,sizeof(b),"OBS pulse-worker=%u anchor=%u bind=%u result=%u depth=%u base=%llx (0=bound 1=invalid 2=UI owned)",
    __atomic_load_n(&pulse_worker_status,__ATOMIC_ACQUIRE),__atomic_load_n(&pulse.ready,__ATOMIC_ACQUIRE),
    bind_count,bind_result,bind_depth,(uint64_t)bind_base);
  if(&native_touch_stage_req && native_touch_stage_req && &native_touch_rows) {
    // Touch replaces the TIMER/OBS rows (42-43) while it is requested: its summary row and the HID report counters.
    line(42,native_touch_rows[0],0xff00ffcc);
    if(&native_hid_input_events) {
      snprintf(b,sizeof(b),"TOUCH HID started=%u driver=%u touch-reports=%u result=%d key-reports=%u | pad dots=%u",
        native_hid_input_events[2],native_hid_input_events[4],native_hid_input_events[6],native_hid_input_results[6],
        native_hid_input_events[5],touch_dots);
      line(43,b,0xff00ffcc);
    }
  } else line(43,b,0xffaaaaaa);
  if(&native_ufs_verify_ticks && &native_ufs_read_ticks) {
    uint64_t freq;__asm__ volatile("mrs %0,cntfrq_el0":"=r"(freq));if(!freq)freq=1;
    uint64_t vt=__atomic_load_n(&native_ufs_verify_ticks,__ATOMIC_RELAXED),rt=__atomic_load_n(&native_ufs_read_ticks,__ATOMIC_RELAXED);
    uint64_t blocks=__atomic_load_n(&native_ufs_verified_blocks,__ATOMIC_RELAXED);
    snprintf(b,sizeof(b),"UFS TIME verify=%llu ms read=%llu ms verified=%llu blocks (%llu MiB) requests=%llu",
      vt*1000/freq,rt*1000/freq,blocks,blocks*4096>>20,__atomic_load_n(&native_ufs_block_reads,__ATOMIC_RELAXED));
    line(46,b,0xffffaa00);
  }
  snprintf(b,sizeof(b),"MEM totalMiB=%llu free4K=%u active=%u inactive=%u wired=%u speculative=%u",
    max_mem>>20,vm_page_free_count,vm_page_active_count,vm_page_inactive_count,
    vm_page_wire_count,vm_page_speculative_count);line(35,b,0xffffff00);
  if(&native_ram_block_sparse){
    snprintf(b,sizeof(b),"RAMDISK sparse=%u capacityMiB=%llu residentKiB=%llu; free min/reserved/target=%u/%u/%u",
      native_ram_block_sparse,native_ram_block_bytes>>20,native_ram_block_resident_bytes>>10,
      vm_page_free_min,vm_page_free_reserved,vm_page_free_target);line(36,b,0xffffff00);
  }
  if (visible->frozen) {
    // Capture without touching the controller, including when an interrupted
    // transfer owns its DMA buffer. Keep cached vs live evidence distinct.
    char cached[2500]; native_ufs_progress_text(cached,sizeof(cached));
    unsigned row=31, col=0;
    for(unsigned i=0;cached[i] && row<48;++i) {
      if(cached[i]=='\n' || col==BP_COLS-1) {
        b[col]=0;line(row++,b,0xffaaaaaa);col=0;
        if(cached[i]=='\n') continue;
      }
      b[col++]=cached[i];
    }
    if(col && row<48) { b[col]=0;line(row,b,0xffaaaaaa); }
  }
  __asm__ volatile("dsb sy" ::: "memory");
  last_draw=now;
}

// A framebuffer write must never delay timer acknowledgement. The monitor
// renders a bounded snapshot without holding the recorder lock across pixels.
static void refresh(unsigned now) {
  if (__atomic_exchange_n(&render_busy,1,__ATOMIC_ACQUIRE)) return;
  if (resume_after && boot_progress_display_owned && !display.bound && display.frame &&
      now >= __atomic_load_n(&last_scanout_at,__ATOMIC_RELAXED)+resume_after) {
    display.bound=1;
    memset(display.previous,0xff,sizeof(display.previous));
    for(unsigned i=0;i<48;++i) display.previous[i][BP_COLS-1]=0;
    bp_clear(&display);
  }
  if (take()) {
    render_snapshot=state;
    release();
    // Rows redraw only their changed cells, so anything else painted over the page (the kernel's early stage bars) would stay;
    // every 15 s each row is repainted whole (a colour reset forces it).
    static unsigned repainted_at;
    if (now-repainted_at>=15) {
      for(unsigned i=0;i<48;++i) display.colors[i][0]=0;
      repainted_at=now;
    }
    draw(now);
  }
  __atomic_store_n(&render_busy,0,__ATOMIC_RELEASE);
}

int boot_progress_active(void) { return enabled != 0; }
int boot_progress_pulse_anchor(uintptr_t base,uint64_t bytes,unsigned w,unsigned h,unsigned rb) {
  int accepted=enabled && bp_pulse_register(&pulse,base,bytes,w,h,rb);
  if(accepted)bp_panic_bind(&boot_progress_panic_vinfo,base,w,h,rb);
  return accepted;
}
// Called before a validated scanout copy, in thread context. A busy writer
// must finish before the display is released. Recording remains active so
// console fallback cannot start drawing over the system frame.
int boot_progress_prepare_scanout(void) {
  if (!enabled) return 1;
  __atomic_store_n(&display.yield_requested,1,__ATOMIC_RELEASE);
  if (__atomic_exchange_n(&render_busy,1,__ATOMIC_ACQUIRE)) return 0;
  if(!bp_pulse_enter(&pulse)) {
    __atomic_store_n(&render_busy,0,__ATOMIC_RELEASE);return 0;
  }
  __atomic_store_n(&display.yield_requested,0,__ATOMIC_RELEASE);
  if (!boot_progress_display_owned) {
    line(0,"DISPLAY HANDOFF: CPU scanout copy pending; diagnostic renderer yielding",0xffffff00);
    __asm__ volatile("dsb sy" ::: "memory");
  }
  boot_progress_display_owned=1;
  __atomic_store_n(&last_scanout_at,seconds(),__ATOMIC_RELAXED);
  display.bound=0;
  bp_pulse_close(&pulse);
  bp_pulse_leave(&pulse);
  __atomic_store_n(&render_busy,0,__ATOMIC_RELEASE);
  return 1;
}
void boot_progress_cancel_scanout(void) {
  __atomic_store_n(&display.yield_requested,0,__ATOMIC_RELEASE);
}
void boot_progress_init(void) {
  unsigned opt=0;
  if (!PE_parse_boot_argn("boot-progress",&opt,sizeof(opt)) || (opt != 1 && opt != 2)) return;
  __asm__ volatile("mrs %0,cntfrq_el0" : "=r"(boot_progress_frequency));
  if (!boot_progress_frequency) return;
  boot_progress_epoch=ticks(); state.limit=120; state.stall=30;
  state.continuous=opt==2; enabled=opt;
  PE_parse_boot_argn("sheng-build-tag",build_tag,sizeof(build_tag));build_tag[sizeof(build_tag)-1]=0;
  if (PE_parse_boot_argn("boot-diag-resume",&resume_after,sizeof(resume_after)) && (resume_after<5 || resume_after>3600)) resume_after=0;
  boot_progress_stage("PE initialized; early native mappings");
}
void boot_progress_bind(uintptr_t base,unsigned w,unsigned h,unsigned rb,unsigned depth) {
  if (!enabled || !take()) return;
  ++bind_count;bind_base=base;bind_depth=depth;bind_result=1;
  if (boot_progress_display_owned) { bind_result=2;release(); return; }
  display.bound=0;
  // The caller passes vinfo only AFTER XNU established its own mapping.
  if (base >= 0xffff000000000000ULL && depth == 32 && w >= 720 && w <= 4096 &&
      h >= 560 && h <= 4096 && rb >= w*4 && rb <= 65536 &&
      base <= UINTPTR_MAX-(uint64_t)h*rb) {
    display.frame=base;display.width=w;display.height=h;display.stride=rb;display.scale=w>=2160 && h>=1400?3:1;
    memset(display.previous,0xff,sizeof(display.previous));
    // Terminate the sentinels before strcmp.
    for(unsigned i=0;i<48;++i) display.previous[i][BP_COLS-1]=0;
    display.bound=1;
    bind_result=0;
    if(!__atomic_load_n(&pulse.ready,__ATOMIC_ACQUIRE))
      bp_panic_bind(&boot_progress_panic_vinfo,base,w,h,rb);
    bp_clear(&display);
    draw(seconds());
  }
  release();
}
void boot_progress_putc(char c) {
  if (!enabled || !take()) return;
  bp_char(&state,c);
  unsigned now=seconds();
  if (!started && !state.frozen && c=='\n' && now!=last_draw) draw(now);
  release();
}
void boot_progress_stage(const char *text) {
  if (!enabled || !take()) return;
  unsigned now=seconds();
  char stamped[BP_COLS], name[32]={0};
  if (started && !ml_at_interrupt_context()) {
    proc_selfname(name,sizeof(name));
    snprintf(stamped,sizeof(stamped),"pid=%d %.31s %.70s",proc_selfpid(),name,text);
    text=stamped;
  }
  bp_stage(&state,text,now);
  if (!started && !state.frozen) draw(now);
  release();
}
void boot_progress_process_stage(int pid,const char *name,const char *text) {
  if(!enabled || !take())return;
  char stamped[BP_COLS];unsigned now=seconds();
  if(!strncmp(text,"exec process signature",22))census_note(name,0);
  snprintf(stamped,sizeof(stamped),"target pid=%d %.24s %.70s",pid,name,text);
  bp_stage(&state,stamped,now);
  if(!started && !state.frozen)draw(now);
  release();
}
void boot_progress_exit(int pid,const char *name,int status,unsigned ns,uint64_t code) {
  if(!enabled)return;
  census_note(name,1);
  if(name && !strcmp(name,"xpcproxy") && pid==proc_selfpid())xpc_fail_note(status,ns,code);
  if(__atomic_exchange_n(&exit_lock,1,__ATOMIC_ACQUIRE)) {
    __atomic_fetch_add(&exit_dropped,1,__ATOMIC_RELAXED);return;
  }
  bp_exit_add(&exit_records,pid,name,status,ns,code);
  unsigned signal=(unsigned)status&127;
  if(signal>=3 && signal<=12 && signal!=9 && boot_progress_exit_fault)
    boot_progress_exit_fault(pid,signal,exit_records.crash[(exit_records.crashes-1)%3],400);
  if(signal==6 && boot_progress_exit_reason) {
    if(&boot_progress_throw_text)boot_progress_throw_text[0]=0;
    boot_progress_exit_reason(pid,signal,exit_records.reason,sizeof(exit_records.reason));
    if(&boot_progress_throw_text && boot_progress_throw_text[0])strlcpy(exit_records.thrown,boot_progress_throw_text,sizeof(exit_records.thrown));
  }
  __atomic_store_n(&exit_lock,0,__ATOMIC_RELEASE);
}
// The log's [spawn] section (BootLog.c): the throw site of the last uncaught exception, then the census as name starts/exits@second of the first
// start, three to a row.
size_t boot_progress_spawn_rows(char *out,size_t cap) {
  size_t at=0;
#define SP_ROW(...) do{int w=snprintf(out+at,cap>at?cap-at:0,__VA_ARGS__);if(w>0)at+=(size_t)w;if(at>=cap)at=cap?cap-1:0;}while(0)
  if(cap<256)return 0;
  if(!__atomic_exchange_n(&exit_lock,1,__ATOMIC_ACQUIRE)) {
    if(exit_records.thrown[0])SP_ROW("THROW %.330s\n",exit_records.thrown);
    // The latest abort's reason text from the dying process's heap (the uncaught-exception line), which only the banner showed before.
    if(exit_records.reason[0])SP_ROW("REASON %.330s\n",exit_records.reason);
    // The crash records (fault registers, shared-cache slide, image base and backtrace of the signalled process), newest first: the log has no
    // other place for them while the banner is off, and the slide is what turns the THROW addresses into symbols.
    for(unsigned i=0;i<3 && i<exit_records.crashes;++i)SP_ROW("CRASH %.400s\n",exit_records.crash[(exit_records.crashes-1-i)%3]);
    __atomic_store_n(&exit_lock,0,__ATOMIC_RELEASE);
  }
  if(!__atomic_exchange_n(&xpc_fail_lock,1,__ATOMIC_ACQUIRE)) {
    if(xpc_fail_count)SP_ROW("xpcproxy exits: labels=%u dropped=%u\n",xpc_fail_count,xpc_fail_dropped);
    for(unsigned i=0;i<xpc_fail_count;++i)
      SP_ROW("XPC %.63s n=%u st=%x ns=%u code=%llx\n",xpc_fails[i].label,xpc_fails[i].count,(unsigned)xpc_fails[i].status,xpc_fails[i].ns,
             (unsigned long long)xpc_fails[i].code);
    __atomic_store_n(&xpc_fail_lock,0,__ATOMIC_RELEASE);
  }
  if(!__atomic_exchange_n(&census_lock,1,__ATOMIC_ACQUIRE)) {
    SP_ROW("names=%u dropped=%u\n",census_count,census_dropped);
    for(unsigned i=0;i<census_count;i+=3) {
      for(unsigned j=i;j<i+3 && j<census_count;++j)SP_ROW("%s%.23s %u/%u@%u",j==i?"":"  ",census[j].name,census[j].started,census[j].exited,census[j].first);
      SP_ROW("\n");
    }
    __atomic_store_n(&census_lock,0,__ATOMIC_RELEASE);
  }
#undef SP_ROW
  return at;
}
// Sampling profile (int-146): every interrupt that lands in a process records that process's interrupted PC, user (EL0) or kernel, in a
// ring of the last 64K samples; boot_progress_profile_rows ranks them per process for the UFS log. One packed word per sample:
// pid << 48 | kernel << 47 | pc & (2^47 - 1) (a kernel PC is rebuilt by setting the top 17 bits).
enum {BpProfSamples = 65536};
static uint64_t bp_prof[BpProfSamples];
static uint32_t bp_prof_head;
// Kernel samples also keep the interrupted frame chain (int-147): int-146's hottest kernel PC was the instruction after DAIFClr in
// ml_set_interrupts_enabled, i.e. timer interrupts held pending by an interrupts-masked section, which only its callers can name.
// Per sample: pc and three return addresses, each as its low 32 bits (kernel text lies in one 4 GiB window); 0 = frame not inside the
// interrupted thread's kernel stack.
enum {BpKernSamples = 32768};
static uint32_t bp_kern[BpKernSamples][4];
static uint32_t bp_kern_head;
static void bp_kernel_chain(uint64_t pc) {
  uint32_t *e=bp_kern[__atomic_fetch_add(&bp_kern_head,1,__ATOMIC_RELAXED)%BpKernSamples];
  e[0]=(uint32_t)pc;e[1]=e[2]=e[3]=0;
  const arm_saved_state_t *s=getCpuDatap()->cpu_int_state;thread_t thread=current_thread();
  if(!s || !thread || !thread->kernel_stack)return;
  const uint64_t low=(uint64_t)thread->kernel_stack,high=low+(uint64_t)kernel_stack_size;
  uint64_t fp=(uint64_t)get_saved_state_fp(s);
  for(unsigned i=1;i<4 && fp>=low && fp+16<=high && !(fp&15);++i) {
    const uint64_t *frame=(const uint64_t *)fp;e[i]=(uint32_t)frame[1];
    const uint64_t next=frame[0];if(next<=fp)break;fp=next;
  }
}
void boot_progress_irq(uint64_t pc,uint64_t psr,uint64_t sp) {
  // Independent of the recorder lock. IRQ still performs no pixel stores.
  if(enabled) {
    __atomic_fetch_add(&irq_entries,1,__ATOMIC_RELAXED);
    __atomic_store_n(&irq_entry_pc,pc,__ATOMIC_RELAXED);
    if(started && nprocs>0) {
      const int pid=proc_selfpid();
      if(pid>0) {
        const uint64_t kernel=(psr&0x1f)!=0;
        const uint32_t at=__atomic_fetch_add(&bp_prof_head,1,__ATOMIC_RELAXED);
        bp_prof[at%BpProfSamples]=((uint64_t)(pid&0xffff)<<48)|(kernel<<47)|(pc&((1ull<<47)-1));
        if(kernel)bp_kernel_chain(pc);
      }
    }
  }
  if (!enabled || state.frozen || !take()) return;
  ++irq_count;last_pc=pc;last_psr=psr;last_sp=sp;boot_progress_last_pc=pc;
  unsigned now=seconds();
  if(now!=sample_second) {
    struct pc_sample *sample=&samples[sample_count++%8];
    sample->pc=pc;sample->psr=psr;sample->second=now;sample->pid=0;sample->name[0]=0;
    if(started && nprocs>0){
      sample->pid=proc_selfpid();
      if(sample->pid)proc_selfname(sample->name,sizeof(sample->name));
      else thread_get_thread_name(current_thread(),sample->name);
      sample->name[sizeof(sample->name)-1]=0;
    }
    sample_second=now;
  }
  release();
  boot_progress_poll();
}
void boot_progress_poll(void) {
  if (!enabled || state.frozen || !take()) return;
  unsigned now=seconds();
  unsigned offline=native_virtio_ready && !native_ufs_blocks_published;
  unsigned reason=bp_poll(&state,now,
    offline ? native_virtio_read_requests : __atomic_load_n(&native_ufs_block_reads,__ATOMIC_RELAXED),
    offline ? native_virtio_verified_blocks : __atomic_load_n(&native_ufs_verified_blocks,__ATOMIC_RELAXED),
    offline ? native_virtio_read_failures : __atomic_load_n(&native_ufs_block_failures,__ATOMIC_RELAXED));
  if (state.frozen) { render_snapshot=state;draw(now); }
  boot_progress_reason=reason;
  if(state.frozen) {
    boot_progress_reason=reason;boot_progress_frozen=1;
    boot_progress_stop_checkpoint();
    // Intentionally retain the complete screen. No reset, storage writes,
    // unsafe MMIO snapshot, panic lock, or resubmission of in-flight I/O.
    __asm__ volatile("msr daifset,#15\n\tdsb sy" ::: "memory");
    for (;;) __asm__ volatile("wfe");
  }
  release();
}
static void pulse_monitor(void *arg, wait_result_t result) {
  (void)arg;(void)result;
  // No thread-name allocation or BSD dependency on this heartbeat path.
  __atomic_store_n(&pulse_worker_status,2,__ATOMIC_RELEASE);
  unsigned turns=0,last_bind=0,last_result=3,last_depth=0,seen_at=0;
  uint64_t last_monitor=~0ULL,last_base=0;
  for(;;) {
    unsigned now=seconds();uint64_t mon=__atomic_load_n(&thread_count,__ATOMIC_RELAXED);
    if(mon!=last_monitor){last_monitor=mon;seen_at=now;}
    if(take()) {
      last_bind=bind_count;last_result=bind_result;last_depth=bind_depth;last_base=bind_base;release();
    }
    if(bp_pulse_enter(&pulse)) {
      if(pulse.ready && !pulse.closed) {
        char text[BP_COLS];
        snprintf(text,sizeof(text),"P%u I%llu M%llu/%us B%u:%u d%u @%llx lock%u/%u PC%llx",
          ++turns,__atomic_load_n(&irq_entries,__ATOMIC_RELAXED),mon,now-seen_at,
          last_bind,last_result,last_depth,last_base,
          __atomic_load_n(&busy,__ATOMIC_RELAXED),__atomic_load_n(&render_busy,__ATOMIC_RELAXED),
          __atomic_load_n(&irq_entry_pc,__ATOMIC_RELAXED));
        bp_line(&pulse.screen,0,text,0xff00ff00);
      }
      bp_pulse_leave(&pulse);
    }
    assert_wait_timeout((event_t)&pulse_worker_status,THREAD_UNINT,1000,NSEC_PER_MSEC);
    thread_block(THREAD_CONTINUE_NULL);
  }
}
static void process_monitor(void *arg, wait_result_t result) {
  (void)arg;(void)result;
  unsigned named=0;
  __atomic_store_n(&process_worker_status,2,__ATOMIC_RELEASE);
  for (;;) {
    if(nprocs>0) {
      if(!named){thread_set_thread_name(current_thread(),"boot_process_probe");named=1;}
      boot_process_capture();
    }
    assert_wait_timeout((event_t)&process_worker_status,THREAD_UNINT,2000,NSEC_PER_MSEC);
    thread_block(THREAD_CONTINUE_NULL);
  }
}
// Visual touch check on the diagnostic page. The text renderer repaints whole rows up to column BP_COLS-1 about once a
// second, so anything drawn there is wiped again; the strip to the right of the text (x >= 120 columns) is never touched
// by it. A scaled copy of the panel (the 30480x20320 touch axes) is kept there: a frame, coloured corner markers inside
// it (top-left red, top-right green, bottom-left blue, bottom-right yellow, on the touch controller's own axes) and a
// persistent dot trail for every contact, so a photograph taken after the finger is gone still shows where it went.
static void touch_fill(unsigned x0,unsigned y0,unsigned x1,unsigned y1,uint32_t color) {
  if(!display.bound || !display.frame)return;
  if(x1>=display.width)x1=display.width-1;
  if(y1>=display.height)y1=display.height-1;
  for(unsigned y=y0;y<=y1;++y)
    for(unsigned x=x0;x<=x1;++x) {
      if(__atomic_load_n(&display.yield_requested,__ATOMIC_ACQUIRE))return;
      *(volatile uint32_t *)(display.frame+(uintptr_t)y*display.stride+x*4)=color;
    }
}
// Session status rows. The 47 text rows are full, so other kits publish short status strings into this array instead: slots 0-3
// belong to the keystore/Data work, slots 4-7 to the GPU work (docs/parallel-development.md). They are drawn right of the page text
// (47 columns, rows 13-20), 30 ms polling, only when the text changed; a redraw of the whole screen (diagnostic resume) repaints them.
char boot_progress_ext[8][48];
// Two wider GPU rows (GPU7, GPU8) for result lines that do not fit 47 columns; the live banner only.
char boot_progress_ext_wide[2][112];
// Data persistence counters (NativeRamBlock): ext slots 2-3 are formatted here from them. State: 0 off, 1 blank probe, 2 resumed,
// 3 fresh, 4 refused/failed, 5 degraded by an I/O error, 6 resumed read-only probe.
extern volatile uint32_t native_data_persist_state __attribute__((weak_import));
extern volatile uint32_t native_data_persist_header __attribute__((weak_import));
extern volatile uint32_t native_data_persist_gpt __attribute__((weak_import));
extern volatile uint32_t native_data_persist_gpt_after __attribute__((weak_import));
extern volatile uint64_t native_data_persist_pages __attribute__((weak_import));
extern volatile uint64_t native_data_persist_loaded __attribute__((weak_import));
extern volatile uint64_t native_data_persist_writes __attribute__((weak_import));
extern volatile uint64_t native_data_persist_syncs __attribute__((weak_import));
extern volatile uint64_t native_data_persist_errors __attribute__((weak_import));
extern volatile uint64_t native_data_persist_first_lba __attribute__((weak_import));
extern volatile uint64_t native_data_persist_present __attribute__((weak_import));
static void ext_data_rows(void) {
  if(!&native_data_persist_state || !native_data_persist_state)return;
  char a[48],b[48];
  snprintf(a,sizeof a,"DATA st=%u hdr=%u gpt=%x/%x lba=%llx",native_data_persist_state,native_data_persist_header,
    native_data_persist_gpt,native_data_persist_gpt_after,(unsigned long long)native_data_persist_first_lba);
  snprintf(b,sizeof b,"pres=%llu ld=%llu wr=%llu sy=%llu er=%llu",(unsigned long long)native_data_persist_present,
    (unsigned long long)native_data_persist_loaded,(unsigned long long)native_data_persist_writes,
    (unsigned long long)native_data_persist_syncs,(unsigned long long)native_data_persist_errors);
  for(unsigned i=0;i<47;++i){boot_progress_ext[2][i]=a[i]?a[i]:' ';boot_progress_ext[3][i]=b[i]?b[i]:' ';}
  boot_progress_ext[2][47]=boot_progress_ext[3][47]=0;
}
static char ext_drawn[8][48];
static unsigned ext_bound_seen;
static void ext_overlay_locked(void) {
  ext_data_rows();
  if(!display.bound){ext_bound_seen=0;return;}
  if(!ext_bound_seen){memset(ext_drawn,0,sizeof(ext_drawn));ext_bound_seen=1;}
  unsigned x0=BP_COLS*6*display.scale+8;
  if(display.width<x0+6*display.scale*8)return;
  unsigned cols=(display.width-x0-8)/(6*display.scale);if(cols>47)cols=47;
  for(unsigned slot=0;slot<8;++slot) {
    char text[48];
    for(unsigned i=0;i<47;++i)text[i]=boot_progress_ext[slot][i];text[47]=0;
    if(!strcmp(text,ext_drawn[slot]))continue;
    unsigned top=(13+slot)*14*display.scale;
    if(top+14*display.scale>=display.height)continue;
    uint32_t color=slot<4?0xff80ffff:0xffffa0ff;
    unsigned len=(unsigned)strlen(text);
    for(unsigned x=0;x<cols;++x) {
      unsigned c=x<len?(unsigned char)text[x]:' ';if(c<32||c>126)c='?';
      uint64_t glyph=font[c-32];
      for(unsigned y=0;y<14;++y) {
        unsigned bits=y<6?(unsigned)(glyph>>(32+y*5)):y<12?(unsigned)(glyph>>((y-6)*5)):0;
        for(unsigned dx=0;dx<6;++dx)
          for(unsigned sy=0;sy<display.scale;++sy)
            for(unsigned sx=0;sx<display.scale;++sx) {
              if(__atomic_load_n(&display.yield_requested,__ATOMIC_ACQUIRE))return;
              *(volatile uint32_t *)(display.frame+(uintptr_t)(top+y*display.scale+sy)*display.stride+(x0+(x*6+dx)*display.scale+sx)*4)=
                dx<5&&((bits>>dx)&1)?color:0;
            }
      }
    }
    memcpy(ext_drawn[slot],text,sizeof(text));
  }
}
// Crash banner: the last non-zero process exits and signal crashes (name, exit-reason namespace, subcode, and for signals the
// fault PC/LR/ESR/FAR) drawn at the bottom of the screen once the OS owns the display, repainted every second, so a restart of
// SpringBoard or backboardd leaves its cause readable on the live screen without waiting for the diagnostic page.
static void px_text(unsigned x0,unsigned top,const char *text,unsigned cols,uint32_t color,uint32_t background) {
  unsigned len=(unsigned)strlen(text);
  for(unsigned x=0;x<cols;++x) {
    unsigned c=x<len?(unsigned char)text[x]:' ';if(c<32||c>126)c='?';
    uint64_t glyph=font[c-32];
    for(unsigned y=0;y<14;++y) {
      unsigned bits=y<6?(unsigned)(glyph>>(32+y*5)):y<12?(unsigned)(glyph>>((y-6)*5)):0;
      for(unsigned dx=0;dx<6;++dx)
        for(unsigned sy=0;sy<display.scale;++sy)
          for(unsigned sx=0;sx<display.scale;++sx)
            *(volatile uint32_t *)(display.frame+(uintptr_t)(top+y*display.scale+sy)*display.stride+(x0+(x*6+dx)*display.scale+sx)*4)=
              dx<5&&((bits>>dx)&1)?color:background;
    }
  }
}
extern volatile uint64_t native_legacy_copy_n __attribute__((weak_import));
extern volatile uint64_t native_gop_gpu_offers __attribute__((weak_import)),native_gop_gpu_completions __attribute__((weak_import)),
  native_gop_gpu_failures __attribute__((weak_import)),native_gop_gpu_abandoned __attribute__((weak_import));
extern volatile uint32_t native_legacy_gpu_fallbacks __attribute__((weak_import)),native_legacy_gpu_last_status __attribute__((weak_import));
extern volatile uint64_t native_legacy_copy_last __attribute__((weak_import));
extern volatile uint64_t native_legacy_copy_max __attribute__((weak_import));
extern volatile uint64_t native_legacy_copy_sum __attribute__((weak_import));
extern volatile uint64_t native_legacy_copy_bytes_last __attribute__((weak_import));
extern volatile uint64_t native_legacy_copy_bytes_sum __attribute__((weak_import));
extern char native_cpufreq_row[120] __attribute__((weak_import));
extern char native_wi_log[4][100] __attribute__((weak_import));
extern volatile unsigned native_wi_count __attribute__((weak_import));
extern volatile uint64_t armcpu_timer_interrupts __attribute__((weak_import));
extern volatile uint64_t native_adreno_cache_stats[5] __attribute__((weak_import));
extern volatile uint32_t native_adreno_coherence[8] __attribute__((weak_import));
extern volatile uint32_t native_adreno_wc_distrusted __attribute__((weak_import));
extern volatile uint32_t native_adreno_wc_audit[6] __attribute__((weak_import));
extern volatile uint64_t native_adreno_reassert_ticks __attribute__((weak_import));
extern volatile uint64_t native_adreno_audit_ticks __attribute__((weak_import));
extern volatile uint32_t native_adreno_audit_attr __attribute__((weak_import));
static uint64_t banner_cache_ticks,banner_cache_bytes,banner_submits;
extern volatile uint64_t armcpu_ipi_interrupts __attribute__((weak_import));
static volatile uint64_t sleep_n,sleep_over_sum,sleep_over_max,sleep_late50;
static uint64_t banner_sleep_n,banner_sleep_sum,banner_sleep_late,banner_tint,banner_ipi;
static unsigned banner_second,banner_top;
static uint64_t banner_copy_n,banner_signature,banner_ufs_v,banner_ufs_r,banner_ufs_n,banner_ufs_b;
static uint32_t banner_load[4];
static void crash_banner(unsigned now) {
  if(!boot_progress_display_owned || display.bound || !display.frame || !display.scale || now-banner_second<2)return;
  // Each repaint is about a million device stores. A swap overwrites the whole frame, so repaint only when a swap has happened
  // since the last paint or the crash records changed; an idle screen keeps the banner.
  uint64_t gpu_sig=0;
  for(unsigned i=4;i<8;++i)for(unsigned k=0;k<47;++k)gpu_sig=gpu_sig*131ULL+(unsigned char)boot_progress_ext[i][k];
  for(unsigned i=0;i<2;++i)for(unsigned k=0;k<111;++k)gpu_sig=gpu_sig*131ULL+(unsigned char)boot_progress_ext_wide[i][k];
  for(unsigned k=0;k<255;++k)gpu_sig=gpu_sig*131ULL+(unsigned char)boot_progress_panic_text[k];
  uint64_t swaps=(&native_legacy_swap_validation_calls?native_legacy_swap_validation_calls:0)*31ULL+(&native_gop_gpu_offers?native_gop_gpu_offers+native_gop_gpu_completions*3:0);
  uint64_t signature=(&native_legacy_copy_n?native_legacy_copy_n:0)*1000003ULL+swaps*1009ULL+(now/10)*977ULL+(&native_wi_count?native_wi_count:0)*7919ULL+exit_records.crashes*131ULL+exit_records.failed+gpu_sig*17ULL;
  if(signature==banner_signature)return;
  banner_signature=signature;banner_second=now;
  char rows[48][120];unsigned n=0;
  if(__atomic_exchange_n(&exit_lock,1,__ATOMIC_ACQUIRE))return;
  unsigned crashes=exit_records.crashes,failed=exit_records.failed;
  // The two latest crash records, each wrapped over up to four rows (header, then the frame-pointer backtrace); then the last exits.
  for(unsigned i=0;i<2 && i<crashes;++i) {
    const char *text=exit_records.crash[(crashes-1-i)%3];
    for(unsigned part=0;part<4 && text[part*108] && n<10;++part)
      snprintf(rows[n++],120,"%s%.108s",part?"      ":"CRASH ",text+part*108);
  }
  for(unsigned part=0;part<3 && exit_records.reason[part*108] && n<12;++part)
    snprintf(rows[n++],120,"%s%.108s",part?"       ":"REASON ",exit_records.reason+part*108);
  for(unsigned i=0;i<3 && i<failed;++i)snprintf(rows[n++],120,"EXIT  %.100s",exit_records.failures[(failed-1-i)%3]);
  __atomic_store_n(&exit_lock,0,__ATOMIC_RELEASE);
  // Presentation cost: swaps in the last second and the time of the pixel copy (last / mean / maximum, microseconds).
  if(&native_legacy_copy_n && native_legacy_copy_n) {
    uint64_t f=0,c=native_legacy_copy_n;__asm__ volatile("mrs %0, cntfrq_el0":"=r"(f));if(!f)f=19200000;
    snprintf(rows[n++],120,"FB    swaps/s=%llu total=%llu copy_us last=%llu mean=%llu max=%llu stored_KiB last=%llu mean=%llu",(unsigned long long)(c-banner_copy_n),
      (unsigned long long)c,(unsigned long long)(native_legacy_copy_last*1000000/f),
      (unsigned long long)(native_legacy_copy_sum/c*1000000/f),(unsigned long long)(native_legacy_copy_max*1000000/f),
      (unsigned long long)(native_legacy_copy_bytes_last>>10),(unsigned long long)((native_legacy_copy_bytes_sum/c)>>10));
    banner_copy_n=c;
  }
  // The display path's counters (the GPU hand-off hides the CPU copy row): swaps the driver validated and how many it took, and the GPU
  // client's offers, completions, failures, abandoned frames, CPU fallbacks and the last fallback status.
  if(&native_legacy_swap_validation_calls && native_legacy_swap_validation_calls) {
    snprintf(rows[n++],120,"SWAP  calls=%u accepted=%u rejected=%u | gpu offers=%llu done=%llu failed=%llu abandoned=%llu fallback=%u last=%x",
      native_legacy_swap_validation_calls,&native_legacy_swap_accepted_count?native_legacy_swap_accepted_count:0,
      &native_legacy_swap_rejected_count?native_legacy_swap_rejected_count:0,
      (unsigned long long)(&native_gop_gpu_offers?native_gop_gpu_offers:0),(unsigned long long)(&native_gop_gpu_completions?native_gop_gpu_completions:0),
      (unsigned long long)(&native_gop_gpu_failures?native_gop_gpu_failures:0),(unsigned long long)(&native_gop_gpu_abandoned?native_gop_gpu_abandoned:0),
      &native_legacy_gpu_fallbacks?native_legacy_gpu_fallbacks:0,&native_legacy_gpu_last_status?native_legacy_gpu_last_status:0);
  }
  // GPU CPU-cache maintenance (all passes: submissions, completions, allocations) since the last paint and in total, per submission,
  // the longest single pass and how much memory user BOs hold write-combined.
  if(&native_adreno_cache_stats && &native_adreno_coherence && native_adreno_coherence[0]) {
    uint64_t f=0;__asm__ volatile("mrs %0, cntfrq_el0":"=r"(f));if(!f)f=19200000;
    uint64_t t=native_adreno_cache_stats[0],b=native_adreno_cache_stats[1],sub=native_adreno_coherence[0];
    uint64_t dt=t-banner_cache_ticks,db=b-banner_cache_bytes,ds=sub-banner_submits;
    snprintf(rows[n++],120,"GPUC  last: subs=%llu cache_ms=%llu MB=%llu per_sub_us=%llu | all: subs=%llu ms=%llu max_us=%llu wc_MB=%llu off@%u",
      (unsigned long long)ds,(unsigned long long)(dt*1000/f),(unsigned long long)(db>>20),(unsigned long long)(ds?dt*1000000/f/ds:0),
      (unsigned long long)sub,(unsigned long long)(t*1000/f),(unsigned long long)(native_adreno_cache_stats[2]*1000000/f),
      (unsigned long long)(native_adreno_cache_stats[4]>>8),&native_adreno_wc_distrusted?native_adreno_wc_distrusted:0);
    banner_cache_ticks=t;banner_cache_bytes=b;banner_submits=sub;
    // The write-combine audit (int-100): submissions and IB lines compared, lines a CPU cache held dirty or stale, the first dirty page's
    // XNU attribute (1xx; 106 = write-combined, 102 = cacheable) and the last failure; int-101: pages XNU had set back to cacheable and
    // that were write-combined again, and the cost of audit and re-assertion. off@ above ends the trust.
    if(&native_adreno_wc_audit && native_adreno_wc_audit[0])
      snprintf(rows[n++],120,"GPUW  audited=%u lines=%u dirty=%u stale=%u attr=%x fail=%x reset=%u audit_us=%llu reassert_us=%llu",
        native_adreno_wc_audit[0],native_adreno_wc_audit[1],native_adreno_wc_audit[2],native_adreno_wc_audit[3],
        &native_adreno_audit_attr?native_adreno_audit_attr:0,native_adreno_wc_audit[4],native_adreno_wc_audit[5],
        (unsigned long long)(&native_adreno_audit_ticks?native_adreno_audit_ticks*1000000/f:0),
        (unsigned long long)(&native_adreno_reassert_ticks?native_adreno_reassert_ticks*1000000/f:0));
  }
  // SpringBoard and backboardd as the process probe last saw them (threads, run state, wired/user counters), when they exist.
  if(&boot_process_rows)
    for(unsigned i=0;i<10 && n<22;++i)
      if((!strncmp(boot_process_rows[i],"SpringBoard p",13) || !strncmp(boot_process_rows[i],"backboardd p",12)))
        snprintf(rows[n++],120,"PROC  %.100s",boot_process_rows[i]);
  // SpringBoard's threads as the stack tracer last saw them (main thread registers and 24 return addresses, then the other threads), then
  // the wait chain boot-ui-follow traces: the process SpringBoard sends to (role U) and the one that process sends to (role B).
  // They take only what the panel's height leaves after the rows below (load, UFS, scheduler, CPU, eight GPU rows, three panic rows).
  unsigned stack_rows=0;
  if(&boot_ui_trace_role_rows) {
    unsigned fit=(display.height-8)/(14*display.scale),cap=fit>2?fit-2:0;if(cap>48)cap=48;
    static const unsigned roles[3]={1,3,0},limit[3]={8,7,6};
    for(unsigned k=0;k<3 && n+15+6<=cap;++k){unsigned room=cap-15-n;unsigned got=boot_ui_trace_role_rows(roles[k],rows+n,room<limit[k]?room:limit[k],k==0);n+=got;stack_rows+=got;}
  }
  // Where the time goes: scheduler ticks since the last paint (user/system/idle percent over all cores) and UFS work since then
  // (hashing/verification ms, transport read ms, 4 KiB blocks verified, read requests).
  {
    host_cpu_load_info_data_t load;mach_msg_type_number_t count=HOST_CPU_LOAD_INFO_COUNT;
    if(host_statistics64(&realhost,HOST_CPU_LOAD_INFO,(host_info_t)&load,&count)==KERN_SUCCESS) {
      uint32_t d[4]={load.cpu_ticks[CPU_STATE_USER]-banner_load[0],load.cpu_ticks[CPU_STATE_SYSTEM]-banner_load[1],
                     load.cpu_ticks[CPU_STATE_IDLE]-banner_load[2],load.cpu_ticks[CPU_STATE_NICE]-banner_load[3]};
      uint64_t sum=(uint64_t)d[0]+d[1]+d[2]+d[3];if(!sum)sum=1;
      for(unsigned i=0;i<4;++i)banner_load[i]=((uint32_t *)&load.cpu_ticks)[i];
      snprintf(rows[n++],120,"LOAD  user=%llu%% sys=%llu%% idle=%llu%% (ticks %llu)",(unsigned long long)(d[0]*100ULL/sum),
        (unsigned long long)(d[1]*100ULL/sum),(unsigned long long)(d[2]*100ULL/sum),(unsigned long long)sum);
    }
    if(&native_ufs_verify_ticks && &native_ufs_read_ticks) {
      uint64_t f=0;__asm__ volatile("mrs %0, cntfrq_el0":"=r"(f));if(!f)f=19200000;
      uint64_t v=native_ufs_verify_ticks,r=native_ufs_read_ticks,b=native_ufs_verified_blocks,q=native_ufs_block_reads;
      snprintf(rows[n++],120,"UFS   since last: verify=%llums read=%llums blocks=%llu reqs=%llu | total verify=%llums read=%llums reqs=%llu",
        (unsigned long long)((v-banner_ufs_v)*1000/f),(unsigned long long)((r-banner_ufs_r)*1000/f),(unsigned long long)(b-banner_ufs_b),
        (unsigned long long)(q-banner_ufs_n),(unsigned long long)(v*1000/f),(unsigned long long)(r*1000/f),(unsigned long long)q);
      banner_ufs_v=v;banner_ufs_r=r;banner_ufs_b=b;banner_ufs_n=q;
    }
  }
  {
    uint64_t sn=sleep_n,ss=sleep_over_sum,sl=sleep_late50,sm=__atomic_exchange_n(&sleep_over_max,0,__ATOMIC_RELAXED);
    uint64_t ti=&armcpu_timer_interrupts?armcpu_timer_interrupts:0,ip=&armcpu_ipi_interrupts?armcpu_ipi_interrupts:0;
    uint64_t dn=sn-banner_sleep_n;if(!dn)dn=1;
    snprintf(rows[n++],120,"SCHED 30ms sleeps since last: n=%llu mean_late_us=%llu max_late_us=%llu >50ms=%llu | irq timer=%llu ipi=%llu",
      (unsigned long long)(sn-banner_sleep_n),(unsigned long long)((ss-banner_sleep_sum)/dn),(unsigned long long)sm,
      (unsigned long long)(sl-banner_sleep_late),(unsigned long long)(ti-banner_tint),(unsigned long long)(ip-banner_ipi));
    banner_sleep_n=sn;banner_sleep_sum=ss;banner_sleep_late=sl;banner_tint=ti;banner_ipi=ip;
  }
  // The work-interval log gives its rows to the stack rows (the banner must fit the panel's height).
  if(!stack_rows && &native_wi_count && native_wi_count) {
    unsigned c=native_wi_count;
    for(unsigned i=0;i<3 && i<c;++i)snprintf(rows[n++],120,"%s",native_wi_log[(c-1-i)%4]);
  }
  if(&native_cpufreq_row && native_cpufreq_row[0])snprintf(rows[n++],120,"CPU   %.110s",native_cpufreq_row);
  // The extension slots are drawn only on the boot page (display bound); the GPU slots 2-7 also go to the live banner.
  // Slots 2 and 3 carry the IPI self-test and the exec observer from the GPU probe once it has run (before that they hold the early
  // zap/interrupt lines, which are worth seeing too), so the banner shows slots 2-7 as GPU1-GPU6.
  for(unsigned i=2;i<8 && n<48;++i)if(boot_progress_ext[i][0] && boot_progress_ext[i][0]!=' ')snprintf(rows[n++],120,"GPU%u  %.47s",i-1,boot_progress_ext[i]);
  for(unsigned i=0;i<2 && n<48;++i)if(boot_progress_ext_wide[i][0])snprintf(rows[n++],120,"GPU%u  %.111s",i+7,boot_progress_ext_wide[i]);
  // A panic that drew the panic screen and was then painted over: its message, wrapped over up to three rows.
  for(unsigned part=0;part<3 && boot_progress_panic_text[part*108] && n<48;++part) {
    char piece[109];unsigned k=0;
    for(;k<108 && part*108+k<255 && boot_progress_panic_text[part*108+k];++k){char ch=boot_progress_panic_text[part*108+k];piece[k]=(ch>=32&&ch<127)?ch:' ';}
    piece[k]=0;
    snprintf(rows[n++],120,"%s%s",part?"      ":"PANIC ",piece);
  }
  if(!n)return;
  unsigned cell=14*display.scale,cols=display.width/(6*display.scale);if(cols>119)cols=119;
  if(display.height<(n+2)*cell+8)return;
  // A title row above the panel; the panel grows upwards from the bottom edge. When it shrank since the last paint, the rows it
  // left behind are cleared so no stale line stays above the new title.
  unsigned top=display.height-(n+1)*cell-8,title=top-cell;
  if(banner_top && banner_top<title && banner_top<display.height)
    for(unsigned y=banner_top;y<title;++y) {
      if(__atomic_load_n(&display.yield_requested,__ATOMIC_ACQUIRE))return;
      for(unsigned x=0;x<cols*6*display.scale;++x)*(volatile uint32_t *)(display.frame+(uintptr_t)y*display.stride+x*4)=0xff000000;
    }
  banner_top=title;
  char head[120];
  snprintf(head,sizeof head," UniXNU live diagnostics%s%s%s  t=%us  crashes=%u exits=%u",build_tag[0]?"  [":"",build_tag,
    build_tag[0]?"]":"",now,crashes,failed);
  px_text(0,title,head,cols,0xffffffff,BP_BAR_LIVE);
  memcpy(boot_progress_banner_text[0],head,120);
  for(unsigned i=0;i<n;++i)memcpy(boot_progress_banner_text[i+1],rows[i],120);
  boot_progress_banner_rows=n+1;
  for(unsigned i=0;i<n;++i) {
    if(__atomic_load_n(&display.yield_requested,__ATOMIC_ACQUIRE))return;
    const char *r=rows[i];
    // Colour by row kind: failures red, exit records yellow, GPU results cyan, timing and load rows grey.
    uint32_t color=!strncmp(r,"CRASH",5)||!strncmp(r,"PANIC",5)?0xffff5a5a:!strncmp(r,"REASON",6)?0xffff9a70:
      !strncmp(r,"EXIT",4)?0xffffe066:!strncmp(r,"GPU",3)?0xff66e0ff:r[0]==' '?0xffff8a8a:0xffb4bcc6;
    px_text(0,top+i*cell,r,cols,color,0xff0d1117);
  }
}
static void touch_overlay_locked(void);
static void touch_overlay(void) {
  // sheng-banner=0 switches every on-screen diagnostic painting off once the system owns the display (the UFS log carries the same rows):
  // the painters hold render_busy for about a million uncached stores, which a swap's scanout hand-off has to wait for.
  static int banner_off=-1;
  if(banner_off<0){unsigned v=1;PE_parse_boot_argn("sheng-banner",&v,sizeof(v));banner_off=v==0;}
  if(banner_off)return;
  __atomic_fetch_add(&touch_passes,1,__ATOMIC_RELAXED);
  // Same rule as the text renderer: draw only while holding render_busy, so a display handoff (which takes it before
  // retiring the mapping) can never overlap these stores. A pass that finds it taken is simply skipped.
  if(__atomic_exchange_n(&render_busy,1,__ATOMIC_ACQUIRE))return;
  ext_overlay_locked();
  crash_banner(seconds());
  if(&native_touch_stage_req && native_touch_stage_req)touch_overlay_locked();
  __atomic_store_n(&render_busy,0,__ATOMIC_RELEASE);
}
static void touch_overlay_locked(void) {
  if(!display.bound || __atomic_load_n(&display.yield_requested,__ATOMIC_ACQUIRE))return;
  static const uint32_t palette[4]={0xffff4040,0xff40ff40,0xff4080ff,0xffffff40};
  static unsigned frame_drawn;
  unsigned textEnd=(BP_COLS)*6*display.scale+8;
  unsigned padX=textEnd,padW=display.width>textEnd+40?display.width-textEnd-24:0;
  touch_pad_w=padW;
  if(padW<200)return;
  unsigned padH=(unsigned)((uint64_t)padW*20320/30480),padY=display.height*45/100;
  if(padY+padH+8>=display.height)return;
  // Frame, corner markers and heartbeat about once a second (they are tens of thousands of uncached stores, far too much
  // for every 30 ms pass); dots are never erased, so the trail persists in between.
  static unsigned decor_pass;
  if((decor_pass++&31)==0) {
  touch_fill(padX-4,padY-4,padX+padW+3,padY-1,0xffffffff);touch_fill(padX-4,padY+padH,padX+padW+3,padY+padH+3,0xffffffff);
  touch_fill(padX-4,padY,padX-1,padY+padH-1,0xffffffff);touch_fill(padX+padW,padY,padX+padW+3,padY+padH-1,0xffffffff);
  touch_fill(padX,padY,padX+30,padY+30,palette[0]);touch_fill(padX+padW-31,padY,padX+padW-1,padY+30,palette[1]);
  touch_fill(padX,padY+padH-31,padX+30,padY+padH-1,palette[2]);touch_fill(padX+padW-31,padY+padH-31,padX+padW-1,padY+padH-1,palette[3]);
  (void)frame_drawn;
  touch_fill(padX,padY-30,padX+19,padY-11,(decor_pass>>5)&1?0xff00ff00:0xffff00ff);   // heartbeat: the overlay thread is running
  }
  uint32_t seq=__atomic_load_n(&native_touch_seq,__ATOMIC_ACQUIRE);
  if(seq&1)return;
  unsigned n=native_touch_count;if(n>12)n=12;
  int xs[12],ys[12];
  for(unsigned i=0;i<n;++i){xs[i]=native_touch_xs[i];ys[i]=native_touch_ys[i];}
  __asm__ volatile("dmb ish" ::: "memory");
  if(__atomic_load_n(&native_touch_seq,__ATOMIC_ACQUIRE)!=seq)return;
  for(unsigned i=0;i<n;++i) {
    if(xs[i]<0||ys[i]<0)continue;
    unsigned px=padX+(unsigned)((uint64_t)xs[i]*padW/30480),py=padY+(unsigned)((uint64_t)ys[i]*padH/20320);
    if(px<padX+4||py<padY+4||px+4>=padX+padW||py+4>=padY+padH)continue;
    // One colour per contact slot; the newest dot gets a white centre so the direction of a swipe is visible.
    touch_fill(px-4,py-4,px+4,py+4,i<4?palette[i]:0xffffffff);
    touch_fill(px-1,py-1,px+1,py+1,0xffffffff);
    __atomic_fetch_add(&touch_dots,1,__ATOMIC_RELAXED);
  }
}
// Scheduler/timer latency probe: the 30 ms sleep of this thread, measured with the architectural counter. A sleep that returns
// much later than asked means timers or wakeups reach an idle core late, which looks like a slow system with idle CPUs.
static void touch_monitor(void *arg, wait_result_t result) {
  (void)arg;(void)result;
  for(;;) {
    touch_overlay();
    uint64_t f,t0,t1;__asm__ volatile("mrs %0, cntfrq_el0":"=r"(f));if(!f)f=19200000;
    __asm__ volatile("isb\n\tmrs %0, cntpct_el0":"=r"(t0));
    assert_wait_timeout((event_t)&touch_monitor,THREAD_UNINT,30,NSEC_PER_MSEC);
    thread_block(THREAD_CONTINUE_NULL);
    __asm__ volatile("isb\n\tmrs %0, cntpct_el0":"=r"(t1));
    uint64_t want=f*30/1000,got=t1-t0,over=got>want?got-want:0;
    uint64_t over_us=over*1000000/f;
    __atomic_fetch_add(&sleep_n,1,__ATOMIC_RELAXED);__atomic_fetch_add(&sleep_over_sum,over_us,__ATOMIC_RELAXED);
    if(over_us>__atomic_load_n(&sleep_over_max,__ATOMIC_RELAXED))__atomic_store_n(&sleep_over_max,over_us,__ATOMIC_RELAXED);
    if(over_us>50000)__atomic_fetch_add(&sleep_late50,1,__ATOMIC_RELAXED);
  }
}
static void monitor(void *arg, wait_result_t result) {
  (void)arg;(void)result;
  unsigned capture_at=0,capture_attempt=~0U,named=0;
  for (;;) {
    __atomic_fetch_add(&thread_count,1,__ATOMIC_RELAXED);
    boot_progress_poll();
    unsigned now=seconds();
    if(nprocs>0 && !named){thread_set_thread_name(current_thread(),"boot_progress");named=1;}
    if(boot_process_snapshot)boot_process_snapshot();
    if(boot_log_tick)boot_log_tick(now);
    if(sheng_cpufreq_poll)sheng_cpufreq_poll();
    if(boot_ui_trace_snapshot)boot_ui_trace_snapshot(now);
    if(&boot_process_attempts && &boot_process_busy) {
      unsigned attempt=__atomic_load_n(&boot_process_attempts,__ATOMIC_RELAXED);
      unsigned capturing=__atomic_load_n(&boot_process_busy,__ATOMIC_ACQUIRE);
      if(attempt!=capture_attempt || !capturing)capture_at=now;
      capture_attempt=attempt;process_capture_age=capturing?now-capture_at:0;
    }
    refresh(seconds());
    assert_wait_timeout((event_t)&thread_count,THREAD_UNINT,1000,NSEC_PER_MSEC);
    thread_block(THREAD_CONTINUE_NULL);
  }
}
void boot_progress_start(void) {
  if (!enabled || started) return;
  started=1;thread_t thread;
  if(kernel_thread_start_priority(monitor,0,BASEPRI_PREEMPT,&thread)!=KERN_SUCCESS) {
    boot_progress_stage("monitor thread creation FAILED; IRQ sampling remains"); return;
  }
  thread_deallocate(thread);
  __atomic_store_n(&pulse_worker_status,1,__ATOMIC_RELEASE);
  if(kernel_thread_start_priority(pulse_monitor,0,BASEPRI_PREEMPT_HIGH,&thread)==KERN_SUCCESS)thread_deallocate(thread);
  else __atomic_store_n(&pulse_worker_status,3,__ATOMIC_RELEASE);
  if(boot_process_capture && boot_process_snapshot) {
    __atomic_store_n(&process_worker_status,1,__ATOMIC_RELEASE);
    if(kernel_thread_start_priority(process_monitor,0,BASEPRI_KERNEL,&thread)==KERN_SUCCESS)thread_deallocate(thread);
    else __atomic_store_n(&process_worker_status,3,__ATOMIC_RELEASE);
  }
  if(kernel_thread_start_priority(touch_monitor,0,BASEPRI_KERNEL,&thread)==KERN_SUCCESS)thread_deallocate(thread);
  boot_progress_stage(state.continuous ?
    "monitor continuous: deadlines warn only; OS remains running" :
    "monitor armed: idle 30s / total 120s; IRQs required for hard stalls");
}

// The profile rows (the log writer's thread, every round): sample count per process (top eight), then for the two busiest processes their
// hottest 1 MiB regions and hottest PCs (u = user, k = kernel PC, low 40 bits; offsets are resolved offline against the images' bases).
static void bp_heap_sort(uint64_t *v,size_t n) {
  for(size_t start=n/2;start-->0;)for(size_t root=start;;){size_t c=2*root+1;if(c>=n)break;if(c+1<n && v[c+1]>v[c])++c;if(v[root]>=v[c])break;uint64_t t=v[root];v[root]=v[c];v[c]=t;root=c;}
  for(size_t end=n;end>1;){--end;uint64_t t=v[0];v[0]=v[end];v[end]=t;
    for(size_t root=0;;){size_t c=2*root+1;if(c>=end)break;if(c+1<end && v[c+1]>v[c])++c;if(v[root]>=v[c])break;t=v[root];v[root]=v[c];v[c]=t;root=c;}}
}
static uint64_t bp_prof_snap[BpProfSamples],bp_prof_rank[BpProfSamples];
size_t boot_progress_profile_rows(char *out,size_t cap) {
  const uint32_t head=__atomic_load_n(&bp_prof_head,__ATOMIC_RELAXED);
  const size_t n=head<BpProfSamples?head:BpProfSamples;
  size_t at=0;
#define BP_ROW(...) do{int w=snprintf(out+at,cap>at?cap-at:0,__VA_ARGS__);if(w>0)at+=(size_t)w;if(at>=cap)at=cap?cap-1:0;}while(0)
  if(!n || cap<64)return 0;
  memcpy(bp_prof_snap,bp_prof,n*sizeof(uint64_t));
  bp_heap_sort(bp_prof_snap,n);
  // per process: (count << 16 | pid), ranked
  size_t procs=0;
  for(size_t i=0;i<n;){size_t j=i;const uint64_t pid=bp_prof_snap[i]>>48;while(j<n && bp_prof_snap[j]>>48==pid)++j;bp_prof_rank[procs++]=((uint64_t)(j-i)<<16)|pid;i=j;}
  bp_heap_sort(bp_prof_rank,procs);
  BP_ROW("PS samples=%u kept=%lu |",head,(unsigned long)n);
  for(size_t k=0;k<8 && k<procs;k++)BP_ROW(" p%llu:%llu",bp_prof_rank[procs-1-k]&0xffff,bp_prof_rank[procs-1-k]>>16);
  BP_ROW("\n");
  uint64_t top[2]={procs?bp_prof_rank[procs-1]&0xffff:0,procs>1?bp_prof_rank[procs-2]&0xffff:0};
  for(unsigned t=0;t<2 && t<procs;t++) {
    const uint64_t pid=top[t];
    size_t first=0;while(first<n && bp_prof_snap[first]>>48!=pid)++first;
    size_t last=first;while(last<n && bp_prof_snap[last]>>48==pid)++last;
    size_t kernel=0;for(size_t i=first;i<last;i++)kernel+=(bp_prof_snap[i]>>47)&1;
    BP_ROW("PF p%llu n=%lu kernel=%lu\n",pid,(unsigned long)(last-first),(unsigned long)kernel);
    // regions of 1 MiB: (count << 32 | kernel << 27 | pc >> 20 (27 bits)), ranked; a region prints as its MiB number (pc >> 20)
    size_t m=0;
    for(size_t i=first;i<last;){const uint64_t key=(bp_prof_snap[i]>>20)&0xfffffff;size_t j=i;while(j<last && ((bp_prof_snap[j]>>20)&0xfffffff)==key)++j;
      bp_prof_rank[m++]=((uint64_t)(j-i)<<32)|key;i=j;}
    bp_heap_sort(bp_prof_rank,m);
    BP_ROW("PR");
    for(size_t k=0;k<24 && k<m;k++){const uint64_t e=bp_prof_rank[m-1-k];BP_ROW(" %c%llx:%llu",(e>>27)&1?'k':'u',e&0x7ffffff,e>>32);if(k%8==7 && k+1<24 && k+1<m)BP_ROW("\nPR");}
    BP_ROW("\n");
    // PCs: (count << 40 | kernel << 39 | pc low 39 bits), ranked
    m=0;
    for(size_t i=first;i<last;){const uint64_t key=bp_prof_snap[i]&((1ull<<48)-1);size_t j=i;while(j<last && (bp_prof_snap[j]&((1ull<<48)-1))==key)++j;
      const uint64_t count=(j-i)>0xffffff?0xffffff:(j-i);bp_prof_rank[m++]=(count<<40)|(((key>>47)&1)<<39)|(key&((1ull<<39)-1));i=j;}
    bp_heap_sort(bp_prof_rank,m);
    const size_t want=t?40:80;
    for(size_t k=0;k<want && k<m;k++){const uint64_t e=bp_prof_rank[m-1-k];if(k%7==0)BP_ROW(k?"\nPP":"PP");
      BP_ROW(" %c%llx:%llu",(e>>39)&1?'k':'u',e&((1ull<<39)-1),e>>40);}
    BP_ROW("\n");
  }
  // Kernel callers: the four hottest kernel PCs of all processes, each with its six most frequent (return 1, return 2) pairs and the
  // third return address of one such sample. PK <pc low 32 bits>:<samples> then <r1>/<r2>/<r3>:<samples>.
  {
    const uint32_t khead=__atomic_load_n(&bp_kern_head,__ATOMIC_RELAXED);
    const size_t kn=khead<BpKernSamples?khead:BpKernSamples;
    for(size_t i=0;i<kn;i++)bp_prof_snap[i]=((uint64_t)bp_kern[i][0]<<32)|i;
    bp_heap_sort(bp_prof_snap,kn);
    size_t m=0;
    for(size_t i=0;i<kn;){size_t j=i;while(j<kn && bp_prof_snap[j]>>32==bp_prof_snap[i]>>32)++j;bp_prof_rank[m++]=((uint64_t)(j-i)<<32)|(uint64_t)i;i=j;}
    bp_heap_sort(bp_prof_rank,m);
    for(size_t k=0;k<4 && k<m;k++) {
      const uint64_t e=bp_prof_rank[m-1-k];const size_t first=(size_t)(e&0xffffffff),count=(size_t)(e>>32);
      BP_ROW("PK %x:%lu",(unsigned)(bp_prof_snap[first]>>32),(unsigned long)count);
      // chains of this PC into the free tail of bp_prof_rank (above the m ranked PCs): (r1 << 32 | r2), sorted, grouped
      uint64_t *chain=bp_prof_rank+m;size_t c=0;
      for(size_t i=first;i<first+count && m+c<BpProfSamples;i++){const uint32_t *x=bp_kern[bp_prof_snap[i]&0xffffffff];chain[c++]=((uint64_t)x[1]<<32)|x[2];}
      bp_heap_sort(chain,c);
      // groups as (count << 40 | first index); the 64 largest are kept
      uint64_t groups[64];size_t g=0;
      for(size_t i=0;i<c;){size_t j=i;while(j<c && chain[j]==chain[i])++j;
        const uint64_t key=((uint64_t)(j-i)<<40)|i;
        if(g<64)groups[g++]=key;else{size_t lo=0;for(size_t q=1;q<64;q++)if(groups[q]<groups[lo])lo=q;if(key>groups[lo])groups[lo]=key;}
        i=j;}
      bp_heap_sort(groups,g);
      for(size_t q=0;q<6 && q<g;q++){const uint64_t ge=groups[g-1-q];const uint64_t pair=chain[ge&0xffffffffff];
        uint32_t r3=0;for(size_t i=first;i<first+count;i++){const uint32_t *x=bp_kern[bp_prof_snap[i]&0xffffffff];if(x[1]==(uint32_t)(pair>>32) && x[2]==(uint32_t)pair){r3=x[3];break;}}
        BP_ROW(" %x/%x/%x:%llu",(unsigned)(pair>>32),(unsigned)pair,r3,ge>>40);}
      BP_ROW("\n");
    }
  }
#undef BP_ROW
  return at;
}

// BootLog.c reaches the diagnostic file export (BootDiagExport.c) only through weak references, which do not pull an archive member into
// the link; this table does.
extern uint64_t boot_diag_scan(uint64_t);
extern int boot_diag_stream(int (*)(const void *, uint32_t, void *), void *);
__attribute__((used)) void *const boot_diag_export_anchor[] = {(void *)boot_diag_scan, (void *)boot_diag_stream};
