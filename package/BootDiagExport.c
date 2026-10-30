// SPDX-License-Identifier: MIT
// Diagnostic file export (int-217): the unified log that logd persisted (/private/var/db/diagnostics, with the format strings in
// /private/var/db/uuidtext) and the crash reports, read by the kernel itself, so neither a sandbox nor a launchd job that never runs
// (adrenoprobed) stands in the way. BootLog.c asks for the stream once (sheng-diag-export=<seconds>) and writes it into the half of the
// export region the pipeline-cache blob does not hold; scripts/read-sheng-log.py unpacks it into the run's diag/ directory.
// Stream: "SHEXR001", u32 stream bytes, u32 0, then the cache export's stream (drivers/AdrenoMetal/CacheExport.h, "M2VCACH1") with root
// "/" and every path relative to it. The scan lists every file first and spends the budget in the order of worth (the log newest file
// first, then its format strings, then the crash reports), so an older boot's large log files never push out this boot's. The format
// strings come last among the log's files because they only change with the binaries: the host keeps those of earlier runs.
// screen.bgrx is the panel's scanout at half resolution (16-byte header "SHOT", u32 width, height, stride, then BGRX rows), so the
// run shows what was on the screen. Files that do not fit the budget are named in diag-skipped.txt, the stream's last file.
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/proc.h>
#include <sys/vnode.h>
#include <sys/uio.h>
#include <sys/dirent.h>
#include <sys/fcntl.h>
#include <sys/kauth.h>
#include <IOKit/IOLib.h>
#include <pexpert/pexpert.h>

enum {DiagMaxFiles = 6144, DiagPath = 192, DiagDepth = 6, DiagChunk = 256 * 1024, DiagNote = 32768};
struct diag_file {char path[DiagPath]; uint64_t size; uint32_t mode, uid, gid, root, keep;};
static struct diag_file *diag_files;
static uint32_t diag_count, diag_skipped_count;
static char *diag_note;   // diag-skipped.txt
static size_t diag_note_used;
volatile uint32_t boot_diag_files, boot_diag_skipped, boot_diag_errors;
volatile uint32_t boot_diag_alloc;   // the scan's allocations that failed: 1 the file table, 2 the note

// In the order of their worth: the format strings of earlier runs are on the host already, and the crash reports are rarely needed.
// A root may be a single file: the render server's shader priming state (PrimeShaders.h) in its persistent cache directory, small and
// the only record of how far priming got (backboardd runs as mobile; the root home is the fallback).
static const char *const diag_roots[] = {
  "/private/var/mobile/Library/Caches/com.apple.backboardd/m2v/prime.log",
  "/private/var/mobile/Library/Caches/com.apple.backboardd/m2v/prime-bad",
  "/private/var/root/Library/Caches/com.apple.backboardd/m2v/prime.log",
  "/private/var/root/Library/Caches/com.apple.backboardd/m2v/prime-bad",
  "/private/var/mobile/Library/Caches/com.apple.backboardd/m2v/shadow.log",
  "/private/var/root/Library/Caches/com.apple.backboardd/m2v/shadow.log",
  "/private/var/mobile/Library/Caches/com.apple.backboardd/m2v/frames.trace",
  "/private/var/root/Library/Caches/com.apple.backboardd/m2v/frames.trace",
  "/private/var/mobile/Library/Caches/com.apple.backboardd/m2v/cpu.profile",
  "/private/var/root/Library/Caches/com.apple.backboardd/m2v/cpu.profile",
  // Safari's built-in data lists (int-269: a nil built-in list URL aborted MobileSafari): what a directory read of the framework
  // returns next to what a lookup of each file finds. Small, so they go before the log.
  "/System/Library/PrivateFrameworks/SafariShared.framework/UserAgentQuirks.plist",
  "/System/Library/PrivateFrameworks/SafariShared.framework/SitesAllowedToAutoplay.plist",
  "/System/Library/PrivateFrameworks/SafariShared.framework/SitesAllowedToUseAutoplayQuirks.plist",
  "/System/Library/PrivateFrameworks/SafariShared.framework/WBSOfflineSearchRemoteDisablementDefaultConfig.plist",
  "/System/Library/PrivateFrameworks/SafariShared.framework/WBSPasswordBreachConfigurationBag.plist",
  "/System/Library/PrivateFrameworks/SafariCore.framework/WBSAutoFillQuirks.plist",
  "/System/Library/PrivateFrameworks/SafariCore.framework/WBSPasswordWarningTopFraudTargets.plist",
  "/System/Library/PrivateFrameworks/SafariShared.framework",
  "/System/Library/PrivateFrameworks/SafariCore.framework",
  "/private/var/db/diagnostics",
  "/private/var/db/uuidtext",
  "/private/var/mobile/Library/Logs/CrashReporter",
};
extern volatile uint64_t native_gop_virtual __attribute__((weak_import));
extern void native_legacy_direct_snapshot(void) __attribute__((weak_import));   // direct scanout: copies the surface on screen into the boot memory
extern void qsort(void *, size_t, size_t, int (*)(const void *, const void *));
static uint32_t shot_width, shot_height;   // the half-resolution screenshot; 0 when there is no scanout
// Scene screenshots taken while the run goes (boot_diag_shot_tick): up to DiagShots of the panel at quarter resolution, kept in memory and
// exported as screen-<seconds of log time>.bgrx, so a scene that is gone by the export is still on file. They are analysed by code only.
enum {DiagShots = 24};   // every 8 s from 90 s to 274 s at quarter resolution: the scene of each moment of the user's session
static uint8_t *shot_data[DiagShots];
static uint32_t shot_time[DiagShots], shot_w, shot_h, shot_taken;
static const uint32_t shot_at[DiagShots] = {90, 98, 106, 114, 122, 130, 138, 146, 154, 162, 170, 178, 186, 194, 202, 210, 218, 226, 234, 242, 250, 258, 266, 274};
// The kernel's rings, as text (kernel.rings.txt), formatted at the scan so its length is known and emitted as it was then.
extern volatile uint64_t (*native_legacy_ring)[2] __attribute__((weak_import));
extern volatile uint32_t native_legacy_ring_cap __attribute__((weak_import));
extern volatile uint32_t native_legacy_ring_count __attribute__((weak_import));
extern volatile uint64_t native_legacy_snap[64][2] __attribute__((weak_import));
extern volatile uint32_t native_legacy_snap_count __attribute__((weak_import));
extern volatile uint32_t native_legacy_swapstat[16] __attribute__((weak_import));
extern volatile uint64_t (*native_adreno_tl)[6] __attribute__((weak_import));
extern volatile uint32_t native_adreno_tl_cap __attribute__((weak_import));
extern volatile uint32_t native_adreno_tl_count __attribute__((weak_import));
extern volatile uint64_t native_adreno_stall_log[32][8] __attribute__((weak_import));
extern volatile uint32_t native_adreno_stall_log_count __attribute__((weak_import));
extern volatile uint64_t native_gpu_dcvs_log[256][2] __attribute__((weak_import));
extern volatile uint32_t native_gpu_dcvs_log_count __attribute__((weak_import));
extern volatile uint32_t native_gop_wait_ring_count __attribute__((weak_import));
extern volatile uint64_t native_gop_wait_ring[64][3] __attribute__((weak_import));
extern volatile uint32_t native_gop_wait_reasons[4] __attribute__((weak_import));
extern char *PE_boot_args(void);
extern char boot_log_autoreset_text[200];
enum {DiagRings = 4194304};
static char *rings_text;
static size_t rings_len;
static uint32_t rings_keep, shots_kept, ipc_keep;

static void note(const char *text, const char *path, int error) {
  if (!diag_note) return;
  int n = snprintf(diag_note + diag_note_used, DiagNote - diag_note_used, "%s %s %d\n", text, path, error);
  if (n > 0 && diag_note_used + (size_t)n < DiagNote) diag_note_used += (size_t)n;
}

static void add(const char *path, uint32_t root, vfs_context_t ctx) {
  vnode_t file = NULLVP;
  int error = vnode_lookup(path, 0, &file, ctx);
  if (error) {note("lookup", path, error); ++boot_diag_errors; return;}
  struct vnode_attr va;
  VATTR_INIT(&va);
  VATTR_WANTED(&va, va_data_size);
  VATTR_WANTED(&va, va_mode);
  VATTR_WANTED(&va, va_uid);
  VATTR_WANTED(&va, va_gid);
  error = vnode_getattr(file, &va, ctx);
  vnode_put(file);
  if (error) {note("getattr", path, error); ++boot_diag_errors; return;}
  if (diag_count >= DiagMaxFiles) {note("unlisted", path, (int)(va.va_data_size >> 10)); ++diag_skipped_count; return;}
  struct diag_file *f = &diag_files[diag_count++];
  strlcpy(f->path, path, sizeof f->path);
  f->size = va.va_data_size; f->mode = va.va_mode; f->uid = va.va_uid; f->gid = va.va_gid; f->root = root; f->keep = 0;
}

static void scan(const char *path, unsigned depth, uint32_t root, vfs_context_t ctx) {
  vnode_t dir = NULLVP;
  int error = vnode_lookup(path, 0, &dir, ctx);
  if (error) {note("lookup", path, error); ++boot_diag_errors; return;}
  if (vnode_vtype(dir) == VREG) {vnode_put(dir); add(path, root, ctx); return;}
  if (vnode_vtype(dir) != VDIR) {vnode_put(dir); return;}
  char *buffer = IOMallocData(8192);
  char *child = IOMallocData(DiagPath);
  if (!buffer || !child) {vnode_put(dir); if (buffer) IOFreeData(buffer, 8192); if (child) IOFreeData(child, DiagPath); return;}
  off_t offset = 0;
  for (int eof = 0; !eof;) {
    uio_t uio = uio_create(1, offset, UIO_SYSSPACE, UIO_READ);
    if (!uio) break;
    uio_addiov(uio, CAST_USER_ADDR_T(buffer), 8192);
    int count = 0;
    error = VNOP_READDIR(dir, uio, 0, &eof, &count, ctx);
    const size_t got = 8192 - (size_t)uio_resid(uio);
    offset = uio_offset(uio);
    uio_free(uio);
    if (error) {note("readdir", path, error); ++boot_diag_errors; break;}
    if (!got) break;
    for (size_t at = 0; at + offsetof(struct dirent, d_name) <= got;) {
      const struct dirent *d = (const struct dirent *)(buffer + at);
      if (!d->d_reclen) {eof = 1; break;}
      at += d->d_reclen;
      if (!d->d_ino || (d->d_namlen == 1 && d->d_name[0] == '.') || (d->d_namlen == 2 && d->d_name[0] == '.' && d->d_name[1] == '.')) continue;
      if ((size_t)snprintf(child, DiagPath, "%s/%.*s", path, (int)d->d_namlen, d->d_name) >= DiagPath) {note("long", path, 0); continue;}
      if (d->d_type == DT_DIR) {
        if (depth < DiagDepth) scan(child, depth + 1, root, ctx);
        continue;
      }
      if (d->d_type != DT_REG) continue;
      add(child, root, ctx);
    }
  }
  IOFreeData(child, DiagPath);
  IOFreeData(buffer, 8192);
  vnode_put(dir);
}

// Root order first; inside a root the later name first (log files are numbered upward, so the newest comes first).
static int worth(const void *a, const void *b) {
  const struct diag_file *x = a, *y = b;
  if (x->root != y->root) return x->root < y->root ? -1 : 1;
  return -strcmp(x->path, y->path);
}

// IPC trace: every message sent to a port of backboardd (the render server's process) or of SpringBoard (which gets the touch events
// from backboardd), the question being what wakes the render thread and the client before a slow frame. The kernel calls
// sheng_ipc_send_hook (the xnu patch) with the destination port locked for the messages to the two traced tasks; this records the time,
// the message id, the sender's pid and the receiver in a ring. kernel.ipc.bin is the ring in time order (16-byte header "IPC2", u32 total,
// u32 kept, u32 0; then {u64 microseconds, u32 message id, u32 receiver << 24 | sender pid (24 bits, 0 kernel, 0xffffff unknown)};
// receiver 1 backboardd, 2 SpringBoard), and kernel.rings.txt names the pids. The hook goes in only once the home screen draws (50
// flips), so it cannot touch the boot's startup (the first run with it, i7d94e5078, stopped at the logo; the cause is not known).
// Touch reports: kernel.touch.bin, the time of every report the touch driver handed to the HID event driver (HidInputProbe.cpp).
extern uint64_t mach_absolute_time(void);
extern void absolutetime_to_nanoseconds(uint64_t, uint64_t *);
extern void *sheng_ipc_trace_task __attribute__((weak_import));
extern void *sheng_ipc_trace_task2 __attribute__((weak_import));
extern void (*sheng_ipc_send_hook)(uint32_t, void *, uint32_t) __attribute__((weak_import));
extern struct task *proc_task(proc_t);
extern volatile uint64_t *native_touch_report_ring __attribute__((weak_import));
extern volatile uint32_t native_touch_report_count __attribute__((weak_import));
enum {IpcRing = 524288, TouchRing = 65536};
// The client sampler (BootUiTraceProbe.c): kernel.client.bin is its ring in time order (16-byte header "CLI1", u32 total, u32 kept, u32 0;
// then 128-byte records {u64 microseconds, i32 pid, u16 state, i16 priority, u32 context switches, i32 mach_msg peer pid, u64 CPU time
// (mach units), u64 wait event, u64 frames[11] (pc, lr, nine callers)}); kernel.rings.txt has a C row per target (pid, shared cache
// slide, main image address, name). A record lasts until the next one of the same pid (unchanged blocked threads are not repeated).
struct cs_rec {uint64_t abs; int32_t pid; uint16_t state; int16_t pri; uint32_t cs; int32_t peer; uint64_t cpu, wait, frames[11];};
struct cs_target {int pid; unsigned slide; uint64_t image, seen; char name[32];};
// CPU occupancy (BootUiTraceProbe.c), kernel.cpus.bin: header "CPU1", total, kept, 0; then 40-byte records {u64 microseconds, i16 priority
// of what runs on CPU 0..7, u8 idle thread on CPU 0..7, u8 processor state 0..7}.
struct cs_cpu {uint64_t abs; int16_t pri[8]; uint8_t idle[8], state[8];};
extern struct cs_cpu *boot_cpu_ring __attribute__((weak_import));
extern volatile uint32_t boot_cpu_count __attribute__((weak_import));
enum {CpuRing = 65536};
static uint32_t cpu_kept, cpu_keep;
// User client calls (HidInputProbe.cpp), kernel.ioc.bin: header "IOC1", total, kept, 0; then 32-byte records {u64 microseconds entered,
// u32 microseconds waiting for the client's lock, u32 microseconds in the method, u32 selector, i32 pid, u64 class name address}; the
// class names are the K rows of kernel.rings.txt.
struct ioc_rec {uint64_t entered; uint32_t lock_us, call_us, selector; int32_t pid; uint64_t class_name;};
extern struct ioc_rec *native_ioc_ring __attribute__((weak_import));
extern volatile uint32_t native_ioc_count __attribute__((weak_import));
extern void native_ioc_trace_start(const int *, unsigned) __attribute__((weak_import));
enum {IocRing = 262144};
static uint32_t ioc_kept, ioc_keep;
extern void boot_client_sampler_start(void) __attribute__((weak_import));
extern struct cs_rec *boot_client_ring __attribute__((weak_import));
extern volatile uint32_t boot_client_count __attribute__((weak_import));
extern struct cs_target boot_client_targets[32] __attribute__((weak_import));
extern volatile unsigned boot_client_target_count __attribute__((weak_import));
enum {ClientRing = 131072};
static uint32_t client_kept, client_keep;
struct ipc_rec {uint64_t abs; uint32_t id; uint32_t from;};
static struct ipc_rec *ipc_ring;
static volatile uint32_t ipc_count;
static uint32_t ipc_kept, touch_kept, touch_keep;   // the counts the export measured, so each file has the length it announced
static int ipc_pid[2];
static unsigned ipc_scans;

static void ipc_hook(uint32_t id, void *sender_task, uint32_t receiver) {
  const uint32_t n = __atomic_fetch_add(&ipc_count, 1, __ATOMIC_RELAXED);
  struct ipc_rec *r = &ipc_ring[n & (IpcRing - 1)];
  proc_t proc = sender_task ? (proc_t)get_bsdtask_info((task_t)sender_task) : NULL;
  const uint32_t pid = sender_task ? (proc ? (uint32_t)proc_pid(proc) & 0xffffff : 0xffffff) : 0;
  r->abs = mach_absolute_time(); r->id = id; r->from = receiver << 24 | pid;
}

static void *ipc_task_of(const char *want, int *pid_out) {
  for (int pid = 1; pid < 512; ++pid) {
    char name[32] = {0};
    proc_name(pid, name, sizeof name);
    if (strcmp(name, want)) continue;
    proc_t proc = proc_find(pid);
    if (!proc) return NULL;
    void *task = proc_task(proc);
    proc_rele(proc);
    *pid_out = pid;
    return task;   // only compared, never followed
  }
  return NULL;
}

static void ipc_trace_tick(void) {
  if (!&native_legacy_swapstat || native_legacy_swapstat[9] < 50) return;   // the home screen draws
  if (&boot_client_sampler_start) boot_client_sampler_start();
  if (&native_ioc_trace_start && &boot_client_target_count) {
    int pids[32]; unsigned n = 0;
    for (unsigned i = 0; i < boot_client_target_count && i < 32; ++i) pids[n++] = boot_client_targets[i].pid;
    native_ioc_trace_start(pids, n);
  }
  if (!&sheng_ipc_send_hook || !&sheng_ipc_trace_task || !&sheng_ipc_trace_task2) return;
  if (!ipc_ring) {
    ipc_ring = IOMallocAligned(sizeof(struct ipc_rec) * IpcRing, PAGE_SIZE);
    if (!ipc_ring) return;
    memset(ipc_ring, 0, sizeof(struct ipc_rec) * IpcRing);
  }
  if (ipc_pid[0] > 0 && ipc_pid[1] > 0 && (++ipc_scans % 8) != 0) return;   // a restarted process has a new pid and task
  int pid = 0;
  void *task = ipc_task_of("backboardd", &pid);
  if (task) {ipc_pid[0] = pid; if (sheng_ipc_trace_task != task) sheng_ipc_trace_task = task;}
  task = ipc_task_of("SpringBoard", &pid);
  if (task) {ipc_pid[1] = pid; if (sheng_ipc_trace_task2 != task) sheng_ipc_trace_task2 = task;}
  if (ipc_pid[0] > 0 && !sheng_ipc_send_hook) sheng_ipc_send_hook = ipc_hook;
}

// Called once per log round with the seconds of log time: takes the next scene screenshot when it is due.
void boot_diag_shot_tick(unsigned now) {
  ipc_trace_tick();
  if (shot_taken >= DiagShots || now < shot_at[shot_taken]) return;
  PE_Video video = {};
  if (!(&native_gop_virtual && native_gop_virtual && !PE_current_console(&video) && video.v_depth == 32 && video.v_width >= 2 && video.v_height >= 2 &&
        video.v_width <= 16384 && video.v_height <= 16384 && video.v_rowBytes >= video.v_width * 4)) {++shot_taken; return;}
  const uint32_t w = (uint32_t)video.v_width / 4, h = (uint32_t)video.v_height / 4;
  const unsigned n = shot_taken++;
  shot_data[n] = IOMallocAligned((size_t)w * h * 4, PAGE_SIZE);
  if (!shot_data[n]) return;
  if (&native_legacy_direct_snapshot) native_legacy_direct_snapshot();
  for (uint32_t y = 0; y < h; ++y) {
    uint32_t *out = (uint32_t *)(shot_data[n] + (size_t)y * w * 4);
    const uint32_t *in = (const uint32_t *)(uintptr_t)(native_gop_virtual + (uint64_t)y * 4 * video.v_rowBytes);
    for (uint32_t x = 0; x < w; ++x) out[x] = in[x * 4];
  }
  shot_w = w; shot_h = h; shot_time[n] = now;
}

#define RT(...) do {if (rings_len + 160 < DiagRings) {int n_ = snprintf(rings_text + rings_len, DiagRings - rings_len, __VA_ARGS__); if (n_ > 0) rings_len += (size_t)n_;}} while (0)
// One text file with every kernel ring of the batch: lines are "<tag> fields", 64-bit microseconds on the uptime clock the render
// server's traces use (CLOCK_UPTIME_RAW), so rows join by time.
static void rings_build(void) {
  if (!rings_text) rings_text = IOMallocAligned(DiagRings, PAGE_SIZE);
  rings_len = 0;
  if (!rings_text) return;
  const char *args = PE_boot_args();
  const size_t alen = args ? strlen(args) : 0;
  RT("# boot-args seen by the kernel: length %lu first32 [%.32s] last32 [%s]\n", (unsigned long)alen, args ? args : "", alen > 32 ? args + alen - 32 : (args ? args : ""));
  RT("# autoreset: %s\n", boot_log_autoreset_text);
  if (&native_legacy_ring_count && &native_legacy_ring_cap && native_legacy_ring_cap) {
    const uint32_t n = native_legacy_ring_count;
    RT("# swap ring: S <us> <event 1 validated 2 fence wait 3 fence passed 4 flip 5 latch 6 retire 7 copy offer 8 blank> <slot or -1> <validated depth> <fences out> <tag> (total %u)\n", n);
    const uint32_t cap = native_legacy_ring_cap;
    for (uint32_t i = n > cap ? n - cap : 0; i < n; ++i) {
      const uint64_t t = native_legacy_ring[i & (cap - 1)][0], v = native_legacy_ring[i & (cap - 1)][1];
      RT("S %llu %u %d %u %u %x\n", t, (unsigned)(v >> 56), (int)((v >> 48) & 0xff) - 1, (unsigned)((v >> 40) & 0xff), (unsigned)((v >> 24) & 0xff), (unsigned)(v & 0xffffff));
    }
    if (&native_legacy_snap_count && native_legacy_snap_count) {
      RT("# swap snapshot: the 64 events before the first validation to flip over 100 ms\n");
      for (unsigned i = 0; i < 64; ++i) {
        const uint64_t t = native_legacy_snap[i][0], v = native_legacy_snap[i][1];
        RT("N %llu %u %d %u %u %x\n", t, (unsigned)(v >> 56), (int)((v >> 48) & 0xff) - 1, (unsigned)((v >> 40) & 0xff), (unsigned)((v >> 24) & 0xff), (unsigned)(v & 0xffffff));
      }
    }
    RT("# swap stats: validated %u kicks %u refused(transferred %u not ready %u no head %u latch in flight %u) fence waits %u copy %u blank %u flips %u latches %u retires %u slow(>100ms) %u longest %uus total %uus max depth %u\n",
       native_legacy_swapstat[0], native_legacy_swapstat[1], native_legacy_swapstat[2], native_legacy_swapstat[3], native_legacy_swapstat[4], native_legacy_swapstat[5],
       native_legacy_swapstat[6], native_legacy_swapstat[7], native_legacy_swapstat[8], native_legacy_swapstat[9], native_legacy_swapstat[10], native_legacy_swapstat[11],
       native_legacy_swapstat[12], native_legacy_swapstat[13], native_legacy_swapstat[14], native_legacy_swapstat[15]);
  }
  if (&native_adreno_tl_count && &native_adreno_tl_cap && native_adreno_tl_cap) {
    const uint32_t n = native_adreno_tl_count;
    RT("# gpu timeline: T <fence> <buffers> <submit us> <cp start ticks> <cp end ticks> (19.2 MHz) <level at submit> <ring status 1 attached 2 stamps 4 page 8 acquired 16 read> (total %u; raw words, zero when the CP wrote nothing)\n", n);
    const uint32_t cap = native_adreno_tl_cap;
    for (uint32_t i = n > cap ? n - cap : 0; i < n; ++i) {
      const volatile uint64_t *r = native_adreno_tl[i & (cap - 1)];
      RT("T %u %u %llu %llu %llu %u %u\n", (unsigned)(r[0] & 0xffffffffu), (unsigned)((r[0] >> 32) & 0x03ffffffu), r[1], r[2], r[3], (unsigned)r[4], (unsigned)(r[0] >> 58));
    }
  }
  if (&native_adreno_stall_log_count) {
    const uint32_t n = native_adreno_stall_log_count;
    RT("# gpu stall ring: X <us> <fence> <event 0 ended 1 kick 3 drain 4 snapshot> <timed-out waits> <pending us> <rptr w before, rptr w after (hex)> <client queue> <first handle> <buffers> (total %u)\n", n);
    for (uint32_t i = n > 32 ? n - 32 : 0; i < n; ++i) {
      const volatile uint64_t *r = native_adreno_stall_log[i & 31];
      RT("X %llu %u %u %u %llu %llx %llx %llx %u\n", r[0], (unsigned)r[1], (unsigned)(r[2] & 0xff), (unsigned)(r[2] >> 8), r[3], r[4], r[5], r[6] & 0xffffffffffffull, (unsigned)(r[6] >> 48));
    }
  }
  if (&native_gpu_dcvs_log_count) {
    const uint32_t n = native_gpu_dcvs_log_count;
    RT("# gpu dcvs ring: D <us> <from level> <to level> <reason 1 busy 2 need 3 down 4 idle 5 touch 6 wake 7 floor 8 vote failed> <window busy %%> <needed MHz> (total %u)\n", n);
    for (uint32_t i = n > 256 ? n - 256 : 0; i < n; ++i) {
      const uint64_t t = native_gpu_dcvs_log[i & 255][0], v = native_gpu_dcvs_log[i & 255][1];
      RT("D %llu %u %u %u %u %u\n", t, (unsigned)(v >> 56), (unsigned)((v >> 48) & 0xff), (unsigned)((v >> 40) & 0xff), (unsigned)((v >> 32) & 0xff), (unsigned)(v & 0xffffffffu));
    }
  }
  if (&boot_client_target_count && boot_client_target_count) {
    RT("# client sampler targets: C <pid> <shared cache slide hex> <main image address hex> <name> <first seen us> (samples %u, kernel.client.bin)\n", client_kept);
    for (unsigned i = 0; i < boot_client_target_count && i < 32; ++i) {
      uint64_t seen = 0;
      absolutetime_to_nanoseconds(boot_client_targets[i].seen, &seen);
      RT("C %d %x %llx %s %llu\n", boot_client_targets[i].pid, boot_client_targets[i].slide, boot_client_targets[i].image, boot_client_targets[i].name, seen / 1000);
    }
  }
  if (ioc_kept) {
    const uint32_t kept = ioc_kept > IocRing ? IocRing : ioc_kept;
    RT("# user client calls: %u (kernel.ioc.bin); K <class name address hex> <class>\n", ioc_kept);
    uint64_t seen[48]; unsigned seen_n = 0;
    for (uint32_t i = 0; i < kept && seen_n < 48; ++i) {
      const uint64_t name = native_ioc_ring[(ioc_kept - kept + i) & (IocRing - 1)].class_name;
      unsigned k = 0;
      while (k < seen_n && seen[k] != name) ++k;
      if (k < seen_n || !name) continue;
      seen[seen_n++] = name;
      RT("K %llx %.60s\n", name, (const char *)(uintptr_t)name);
    }
  }
  if (ipc_ring) {
    const uint32_t n = ipc_kept ? ipc_kept : ipc_count, kept = n > IpcRing ? IpcRing : n;
    RT("# ipc to backboardd (receiver 1, pid %d) and SpringBoard (receiver 2, pid %d): total %u kept %u (kernel.ipc.bin); P <pid> <name> for every sender in it\n",
       ipc_pid[0], ipc_pid[1], n, kept);
    int seen[64]; unsigned seen_n = 0;
    for (uint32_t i = 0; i < kept && seen_n < 64; ++i) {
      const uint32_t raw = ipc_ring[(n - kept + i) & (IpcRing - 1)].from & 0xffffff;
      const int pid = raw == 0xffffff ? -1 : (int)raw;
      unsigned k = 0;
      while (k < seen_n && seen[k] != pid) ++k;
      if (k < seen_n) continue;
      seen[seen_n++] = pid;
      char name[32] = {0};
      if (pid > 0) proc_name(pid, name, sizeof name); else snprintf(name, sizeof name, "kernel");
      RT("P %d %s\n", pid, name);
    }
  }
  if (&native_gop_wait_ring_count) {
    const uint32_t n = native_gop_wait_ring_count;
    RT("# scanout waits that ended without a swap after 100 ms or more (the object waited on is the presenter's swap slot): W <us> <waited ms> <reason 0 deadline 1 interrupted 2 other> <pid> <swap offered> <delivered> <online> <requested ms> (total %u; reasons: deadline %u interrupted %u other %u woken with a swap %u)\n",
       n, native_gop_wait_reasons[0], native_gop_wait_reasons[1], native_gop_wait_reasons[2], native_gop_wait_reasons[3]);
    for (uint32_t i = n > 64 ? n - 64 : 0; i < n; ++i) {
      const volatile uint64_t *r = native_gop_wait_ring[i & 63];
      RT("W %llu %llu %u %u %u %u %u %u\n", r[0], (r[1] & 0xffffffffffffffull) / 1000000, (unsigned)(r[1] >> 56), (unsigned)(r[2] & 0xffffffffu), (unsigned)((r[2] >> 32) & 1),
         (unsigned)((r[2] >> 33) & 1), (unsigned)((r[2] >> 34) & 1), (unsigned)(r[2] >> 40));
    }
  }
}

// The stream's length (without the 16-byte blob header) for at most budget bytes, after scanning the roots; 0 when nothing could be read.
uint64_t boot_diag_scan(uint64_t budget) {
  // The scan's only zero return is a failed allocation ([diag] status=-8 in i84edf75a1 and, with the table from page memory,
  // in id02fe1633): both come from page allocations, as the export's staging does (IOMallocAligned, 256 KiB, which works), and
  // boot_diag_alloc names the one that failed.
  if (!diag_files) diag_files = IOMallocAligned(sizeof(struct diag_file) * DiagMaxFiles, PAGE_SIZE);
  if (!diag_note) diag_note = IOMallocAligned(DiagNote, PAGE_SIZE);
  boot_diag_alloc = (diag_files ? 0 : 1) | (diag_note ? 0 : 2);
  if (!diag_files || !diag_note) return 0;
  diag_count = diag_skipped_count = 0; diag_note_used = 0; diag_note[0] = 0;
  const char *note_name = "diag-skipped.txt";
  uint64_t used = 8 + 4 + 1 + 4 + 24 + strlen(note_name) + DiagNote;   // head with root "/", the note file at its full size
  PE_Video video = {};
  shot_width = shot_height = 0;
  if (&native_gop_virtual && native_gop_virtual && !PE_current_console(&video) && video.v_depth == 32 && video.v_width >= 2 && video.v_height >= 2 &&
      video.v_width <= 16384 && video.v_height <= 16384 && video.v_rowBytes >= video.v_width * 4) {
    const uint64_t cost = 24 + strlen("screen.bgrx") + 16 + (uint64_t)(video.v_width / 2) * (video.v_height / 2) * 4;
    if (used + cost <= budget) {shot_width = (uint32_t)video.v_width / 2; shot_height = (uint32_t)video.v_height / 2; used += cost;}
  }
  // The kernel's rings and the scene screenshots come before every file: they are this run's own record.
  rings_keep = shots_kept = 0;
  ipc_kept = ipc_count;
  client_kept = (&boot_client_count && &boot_client_ring && boot_client_ring) ? boot_client_count : 0;
  cpu_kept = (&boot_cpu_count && &boot_cpu_ring && boot_cpu_ring) ? boot_cpu_count : 0;
  ioc_kept = (&native_ioc_count && &native_ioc_ring && native_ioc_ring) ? native_ioc_count : 0;
  rings_build();
  if (rings_text && rings_len) {
    const uint64_t cost = 24 + strlen("kernel.rings.txt") + rings_len;
    if (used + cost <= budget) {rings_keep = 1; used += cost;}
  }
  ipc_keep = 0;
  client_keep = 0;
  touch_keep = 0;
  cpu_keep = 0;
  if (cpu_kept) {
    const uint32_t kept = cpu_kept > CpuRing ? CpuRing : cpu_kept;
    const uint64_t cost = 24 + strlen("kernel.cpus.bin") + 16 + (uint64_t)kept * sizeof(struct cs_cpu);
    if (used + cost <= budget) {cpu_keep = 1; used += cost;}
  }
  ioc_keep = 0;
  if (ioc_kept) {
    const uint32_t kept = ioc_kept > IocRing ? IocRing : ioc_kept;
    const uint64_t cost = 24 + strlen("kernel.ioc.bin") + 16 + (uint64_t)kept * sizeof(struct ioc_rec);
    if (used + cost <= budget) {ioc_keep = 1; used += cost;}
  }
  touch_kept = (&native_touch_report_count && &native_touch_report_ring && native_touch_report_ring) ? native_touch_report_count : 0;
  if (touch_kept) {
    const uint32_t kept = touch_kept > TouchRing ? TouchRing : touch_kept;
    const uint64_t cost = 24 + strlen("kernel.touch.bin") + 16 + (uint64_t)kept * 8;
    if (used + cost <= budget) {touch_keep = 1; used += cost;}
  }
  if (client_kept) {
    const uint32_t kept = client_kept > ClientRing ? ClientRing : client_kept;
    const uint64_t cost = 24 + strlen("kernel.client.bin") + 16 + (uint64_t)kept * sizeof(struct cs_rec);
    if (used + cost <= budget) {client_keep = 1; used += cost;}
  }
  if (ipc_ring && ipc_kept) {
    const uint32_t kept = ipc_kept > IpcRing ? IpcRing : ipc_kept;
    const uint64_t cost = 24 + strlen("kernel.ipc.bin") + 16 + (uint64_t)kept * sizeof(struct ipc_rec);
    if (used + cost <= budget) {ipc_keep = 1; used += cost;}
  }
  for (unsigned i = 0; i < DiagShots; ++i) {
    if (!shot_data[i]) continue;
    const uint64_t cost = 24 + strlen("screen-000.bgrx") + 16 + (uint64_t)shot_w * shot_h * 4;
    if (used + cost <= budget) {++shots_kept; used += cost;} else shot_time[i] = 0xffffffffu;
  }
  vfs_context_t ctx = vfs_context_kernel();
  for (unsigned i = 0; i < sizeof diag_roots / sizeof diag_roots[0]; ++i) scan(diag_roots[i], 0, i, ctx);
  qsort(diag_files, diag_count, sizeof diag_files[0], worth);
  uint32_t kept = 0;
  for (uint32_t i = 0; i < diag_count; ++i) {
    struct diag_file *f = &diag_files[i];
    const uint64_t cost = 24 + strlen(f->path) - 1 + f->size;
    if (used + cost > budget) {note("skipped", f->path, (int)(f->size >> 10)); ++diag_skipped_count; continue;}
    f->keep = 1; used += cost; ++kept;
  }
  boot_diag_files = kept; boot_diag_skipped = diag_skipped_count;
  return used;
}

typedef int (*boot_diag_emit_t)(const void *, uint32_t, void *);
static int put_head(boot_diag_emit_t emit, void *arg, const char *path, uint32_t mode, uint32_t uid, uint32_t gid, uint64_t size) {
  uint8_t head[24];
  const uint32_t plen = (uint32_t)strlen(path), fields[4] = {plen, mode, uid, gid};
  memcpy(head, fields, 16); memcpy(head + 16, &size, 8);
  return emit(head, sizeof head, arg) || emit(path, plen, arg);
}

// Emits the stream the last scan measured; each file goes out at its scanned size (zero-filled when it shrank or could not be read, cut
// when it grew), so the length always equals boot_diag_scan's. Returns the first nonzero emit result.
int boot_diag_stream(boot_diag_emit_t emit, void *arg) {
  uint8_t *chunk = IOMallocAligned(DiagChunk, PAGE_SIZE);
  if (!chunk) return -1;
  vfs_context_t ctx = vfs_context_kernel();
  int rc = emit("M2VCACH1", 8, arg);
  const uint32_t root_len = 1, count = boot_diag_files + 1 + (shot_width ? 1 : 0) + rings_keep + shots_kept + ipc_keep + client_keep + touch_keep + cpu_keep + ioc_keep;
  if (!rc) rc = emit(&root_len, 4, arg);
  if (!rc) rc = emit("/", 1, arg);
  if (!rc) rc = emit(&count, 4, arg);
  if (!rc && shot_width) {
    // Every second pixel of every second row, read from the scanout mapping the display presenter set up.
    PE_Video video = {};
    if (&native_legacy_direct_snapshot) native_legacy_direct_snapshot();
    const uint32_t row = shot_width * 4, head[4] = {'S' | 'H' << 8 | 'O' << 16 | (uint32_t)'T' << 24, shot_width, shot_height, row};
    const bool live = !PE_current_console(&video) && video.v_width / 2 == shot_width && video.v_height / 2 == shot_height;
    rc = put_head(emit, arg, "screen.bgrx", 0100644, 0, 0, 16 + (uint64_t)row * shot_height);
    if (!rc) rc = emit(head, sizeof head, arg);
    for (uint32_t y = 0; !rc && y < shot_height; ++y) {
      uint32_t *out = (uint32_t *)chunk;
      const uint32_t *in = (const uint32_t *)(uintptr_t)(native_gop_virtual + (uint64_t)y * 2 * video.v_rowBytes);
      for (uint32_t x = 0; x < shot_width; ++x) out[x] = live ? in[x * 2] : 0;
      rc = emit(chunk, row, arg);
    }
  }
  if (!rc && rings_keep) {
    rc = put_head(emit, arg, "kernel.rings.txt", 0100644, 0, 0, rings_len);
    for (size_t at = 0; !rc && at < rings_len;) {
      const uint32_t n = rings_len - at < 65536 ? (uint32_t)(rings_len - at) : 65536;
      rc = emit(rings_text + at, n, arg);
      at += n;
    }
  }
  if (!rc && ipc_keep) {
    const uint32_t ipc_total = ipc_kept, kept = ipc_total > IpcRing ? IpcRing : ipc_total;   // the measured count: newer records wait for the next export
    const uint32_t head[4] = {'I' | 'P' << 8 | 'C' << 16 | (uint32_t)'2' << 24, ipc_total, kept, 0};
    rc = put_head(emit, arg, "kernel.ipc.bin", 0100644, 0, 0, 16 + (uint64_t)kept * sizeof(struct ipc_rec));
    if (!rc) rc = emit(head, sizeof head, arg);
    for (uint32_t at = 0; !rc && at < kept;) {   // oldest first, microseconds since boot
      uint32_t n = kept - at < 4096 ? kept - at : 4096;
      for (uint32_t k = 0; k < n; ++k) {
        const struct ipc_rec *r = &ipc_ring[(ipc_total - kept + at + k) & (IpcRing - 1)];
        uint64_t ns = 0;
        absolutetime_to_nanoseconds(r->abs, &ns);
        struct ipc_rec out = {ns / 1000, r->id, r->from};
        memcpy(chunk + (size_t)k * sizeof out, &out, sizeof out);
      }
      rc = emit(chunk, n * sizeof(struct ipc_rec), arg);
      at += n;
    }
  }
  if (!rc && cpu_keep) {
    const uint32_t cpu_total = cpu_kept, kept = cpu_total > CpuRing ? CpuRing : cpu_total;
    const uint32_t head[4] = {'C' | 'P' << 8 | 'U' << 16 | (uint32_t)'1' << 24, cpu_total, kept, 0};
    rc = put_head(emit, arg, "kernel.cpus.bin", 0100644, 0, 0, 16 + (uint64_t)kept * sizeof(struct cs_cpu));
    if (!rc) rc = emit(head, sizeof head, arg);
    for (uint32_t at = 0; !rc && at < kept;) {
      const uint32_t n = kept - at < 4096 ? kept - at : 4096;
      for (uint32_t k = 0; k < n; ++k) {
        struct cs_cpu out = boot_cpu_ring[(cpu_total - kept + at + k) & (CpuRing - 1)];
        uint64_t ns = 0;
        absolutetime_to_nanoseconds(out.abs, &ns);
        out.abs = ns / 1000;
        memcpy(chunk + (size_t)k * sizeof out, &out, sizeof out);
      }
      rc = emit(chunk, n * sizeof(struct cs_cpu), arg);
      at += n;
    }
  }
  if (!rc && ioc_keep) {
    const uint32_t ioc_total = ioc_kept, kept = ioc_total > IocRing ? IocRing : ioc_total;
    const uint32_t head[4] = {'I' | 'O' << 8 | 'C' << 16 | (uint32_t)'1' << 24, ioc_total, kept, 0};
    rc = put_head(emit, arg, "kernel.ioc.bin", 0100644, 0, 0, 16 + (uint64_t)kept * sizeof(struct ioc_rec));
    if (!rc) rc = emit(head, sizeof head, arg);
    for (uint32_t at = 0; !rc && at < kept;) {
      const uint32_t n = kept - at < 8192 ? kept - at : 8192;
      for (uint32_t k = 0; k < n; ++k) {
        struct ioc_rec out = native_ioc_ring[(ioc_total - kept + at + k) & (IocRing - 1)];
        uint64_t ns = 0;
        absolutetime_to_nanoseconds(out.entered, &ns);
        out.entered = ns / 1000;
        memcpy(chunk + (size_t)k * sizeof out, &out, sizeof out);
      }
      rc = emit(chunk, n * sizeof(struct ioc_rec), arg);
      at += n;
    }
  }
  if (!rc && touch_keep) {
    const uint32_t touch_total = touch_kept, kept = touch_total > TouchRing ? TouchRing : touch_total;
    const uint32_t head[4] = {'T' | 'C' << 8 | 'H' << 16 | (uint32_t)'1' << 24, touch_total, kept, 0};
    rc = put_head(emit, arg, "kernel.touch.bin", 0100644, 0, 0, 16 + (uint64_t)kept * 8);
    if (!rc) rc = emit(head, sizeof head, arg);
    for (uint32_t at = 0; !rc && at < kept;) {   // oldest first, microseconds since boot
      const uint32_t n = kept - at < 8192 ? kept - at : 8192;
      for (uint32_t k = 0; k < n; ++k) {
        uint64_t ns = 0;
        absolutetime_to_nanoseconds(native_touch_report_ring[(touch_total - kept + at + k) & (TouchRing - 1)], &ns);
        const uint64_t us = ns / 1000;
        memcpy(chunk + (size_t)k * 8, &us, 8);
      }
      rc = emit(chunk, n * 8, arg);
      at += n;
    }
  }
  if (!rc && client_keep) {
    const uint32_t client_total = client_kept, kept = client_total > ClientRing ? ClientRing : client_total;
    const uint32_t head[4] = {'C' | 'L' << 8 | 'I' << 16 | (uint32_t)'1' << 24, client_total, kept, 0};
    rc = put_head(emit, arg, "kernel.client.bin", 0100644, 0, 0, 16 + (uint64_t)kept * sizeof(struct cs_rec));
    if (!rc) rc = emit(head, sizeof head, arg);
    for (uint32_t at = 0; !rc && at < kept;) {   // oldest first, microseconds since boot
      const uint32_t n = kept - at < 1024 ? kept - at : 1024;
      for (uint32_t k = 0; k < n; ++k) {
        struct cs_rec out = boot_client_ring[(client_total - kept + at + k) & (ClientRing - 1)];
        uint64_t ns = 0;
        absolutetime_to_nanoseconds(out.abs, &ns);
        out.abs = ns / 1000;
        memcpy(chunk + (size_t)k * sizeof out, &out, sizeof out);
      }
      rc = emit(chunk, n * sizeof(struct cs_rec), arg);
      at += n;
    }
  }
  for (unsigned i = 0; !rc && i < DiagShots; ++i) {
    if (!shot_data[i] || shot_time[i] == 0xffffffffu) continue;
    char name[24];
    snprintf(name, sizeof name, "screen-%03u.bgrx", shot_time[i] % 1000);
    const uint32_t row = shot_w * 4, head[4] = {'S' | 'H' << 8 | 'O' << 16 | (uint32_t)'T' << 24, shot_w, shot_h, row};
    rc = put_head(emit, arg, name, 0100644, 0, 0, 16 + (uint64_t)row * shot_h);
    if (!rc) rc = emit(head, sizeof head, arg);
    for (uint32_t y = 0; !rc && y < shot_h; ++y) rc = emit(shot_data[i] + (size_t)y * row, row, arg);
  }
  for (uint32_t i = 0; !rc && i < diag_count; ++i) {
    const struct diag_file *f = &diag_files[i];
    if (!f->keep) continue;
    rc = put_head(emit, arg, f->path + 1, f->mode, f->uid, f->gid, f->size);
    vnode_t vp = NULLVP;
    int error = rc ? 0 : vnode_open(f->path, FREAD, 0, 0, &vp, ctx);
    if (error) {note("open", f->path, error); ++boot_diag_errors; vp = NULLVP;}
    for (uint64_t at = 0; !rc && at < f->size;) {
      const uint32_t want = f->size - at < DiagChunk ? (uint32_t)(f->size - at) : DiagChunk;
      int resid = (int)want;
      memset(chunk, 0, want);
      if (vp && (error = vn_rdwr(UIO_READ, vp, (caddr_t)chunk, (int)want, (off_t)at, UIO_SYSSPACE, IO_NOAUTH, vfs_context_ucred(ctx), &resid,
                                 vfs_context_proc(ctx)))) {
        note("read", f->path, error); ++boot_diag_errors;
        vnode_close(vp, FREAD, ctx); vp = NULLVP;
      }
      rc = emit(chunk, want, arg);
      at += want;
    }
    if (vp) vnode_close(vp, FREAD, ctx);
  }
  // The note last, at the size the scan reserved.
  if (!rc) rc = put_head(emit, arg, "diag-skipped.txt", 0100644, 0, 0, DiagNote);
  if (!rc) {memset(diag_note + diag_note_used, 0, DiagNote - diag_note_used); rc = emit(diag_note, DiagNote, arg);}
  IOFreeAligned(chunk, DiagChunk);
  return rc;
}
