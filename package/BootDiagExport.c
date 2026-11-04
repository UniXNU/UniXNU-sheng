// SPDX-License-Identifier: MIT
// Seven-issue latency export: command/pass phases, allocation/release, CPU,
// render thread waits, display arrival, fault and unnest evidence, and the
// touch report times that bound the analysed windows; with sheng-shots=<s>,
// scene screenshots for the colour checks.
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

extern void native_wifi_logf(int, const char *, ...) __attribute__((weak_import));
/* Opt-in capture of the cache loader's own error log. Runs on the log writer,
 * reads one fixed file and stops after two minutes; ordinary boots do no I/O. */
void boot_cache_loader_log_capture(unsigned now) {
  static int enabled = -1, last_error = -1;
  static unsigned next_read;
  static uint64_t last_size = UINT64_MAX;
  static struct timespec last_modified;
  static char *buffer;
  enum {capacity = 8192};
  if (enabled < 0) {
    uint32_t value = 0;
    enabled = PE_parse_boot_argn("sheng-cache-log", &value, sizeof value) && value == 1;
  }
  if (!enabled || !&native_wifi_logf) return;
  if (now > 120) { if (buffer) {IOFreeData(buffer, capacity); buffer = NULL;} return; }
  if (now < 10 || now < next_read) return;
  next_read = now + 4;
  vfs_context_t ctx = vfs_context_kernel();
  vnode_t vp = NULLVP;
  const char *path = "/private/var/log/com.apple.xpc.launchd/launchd_cache_loader.log";
  int error = vnode_open(path, FREAD, 0, 0, &vp, ctx);
  struct vnode_attr va;
  VATTR_INIT(&va); VATTR_WANTED(&va, va_data_size); VATTR_WANTED(&va, va_modify_time);
  if (!error) error = vnode_getattr(vp, &va, ctx);
  if (!error && (va.va_data_size != last_size ||
      va.va_modify_time.tv_sec != last_modified.tv_sec || va.va_modify_time.tv_nsec != last_modified.tv_nsec)) {
    if (!buffer) buffer = IOMallocData(capacity);
    if (!buffer) error = ENOMEM;
    else {
      uint64_t offset = va.va_data_size > capacity ? va.va_data_size - capacity : 0;
      int length = (int)(va.va_data_size - offset), resid = length;
      error = vn_rdwr(UIO_READ, vp, buffer, length, (off_t)offset, UIO_SYSSPACE,
          IO_NOAUTH, vfs_context_ucred(ctx), &resid, vfs_context_proc(ctx));
      if (!error && resid >= 0 && resid <= length) {
        length -= resid; last_size = va.va_data_size; last_modified = va.va_modify_time;
        native_wifi_logf(6, "cache-loader-file: size %llu offset %llu bytes %d mtime %llu.%09u", last_size, offset, length,
            (unsigned long long)last_modified.tv_sec, (unsigned)last_modified.tv_nsec);
        for (int at = 0; at < length;) {
          int n = 0;
          while (at + n < length && n < 76 && buffer[at + n] != '\n') ++n;
          if (n) native_wifi_logf(6, "cache-loader-file %u: %.*s", (unsigned)(offset + at), n, buffer + at);
          at += n;
          if (at < length && buffer[at] == '\n') ++at;
        }
      } else if (!error) error = EIO;
    }
  }
  if (vp) vnode_close(vp, FREAD, ctx);
  if (error != last_error) {last_error = error; native_wifi_logf(6, "cache-loader-file: read status %d", error);}
}


enum {DiagMaxFiles = 128, DiagPath = 192, DiagDepth = 0, DiagChunk = 256 * 1024, DiagNote = 32768};
struct diag_file {char path[DiagPath]; uint64_t size; uint32_t mode, uid, gid, root, keep;};
static struct diag_file *diag_files;
static uint32_t diag_count, diag_skipped_count;
static char *diag_note;   // diag-skipped.txt
static size_t diag_note_used;
volatile uint32_t boot_diag_files, boot_diag_skipped, boot_diag_errors;
volatile uint32_t boot_diag_alloc;   // the scan's allocations that failed: 1 the file table, 2 the note

#include "BootLatencyTrace.h"
#include "BootLatencyFiles.h"
static void *latency_data;static uint32_t latency_size;
enum {LatencyCapacity=16+524288*72};

static const char *const diag_roots[] = {
  "/private/var/mobile/Library/Caches/com.apple.backboardd/m2v/frames.trace",
  "/private/var/root/Library/Caches/com.apple.backboardd/m2v/frames.trace",
  "/private/var/mobile/Library/Logs/CrashReporter",
  "/private/var/root/Library/Logs/CrashReporter",
};
extern void qsort(void *, size_t, size_t, int (*)(const void *, const void *));
// The kernel's rings, as text (kernel.rings.txt), formatted at the scan so its length is known and emitted as it was then.
extern volatile uint64_t (*native_legacy_ring)[2] __attribute__((weak_import));
extern volatile uint32_t native_legacy_ring_cap __attribute__((weak_import));
extern volatile uint32_t native_legacy_ring_count __attribute__((weak_import));
extern volatile uint64_t native_legacy_snap[64][2] __attribute__((weak_import));
extern volatile uint32_t native_legacy_snap_count __attribute__((weak_import));
extern volatile uint32_t native_legacy_swapstat[16] __attribute__((weak_import));
extern volatile uint64_t (*native_adreno_tl)[8] __attribute__((weak_import));
extern volatile uint32_t native_adreno_tl_cap __attribute__((weak_import));
extern volatile uint32_t native_adreno_tl_count __attribute__((weak_import));
extern volatile uint32_t native_gop_wait_ring_count __attribute__((weak_import));
extern volatile uint64_t native_gop_wait_ring[64][3] __attribute__((weak_import));
extern volatile uint32_t native_gop_wait_reasons[4] __attribute__((weak_import));
extern char *PE_boot_args(void);
extern char boot_log_autoreset_text[200];
enum {DiagRings = 4194304};
static char *rings_text;
static size_t rings_len;
static uint32_t rings_keep;
extern uint32_t native_gpu_trace_snapshot(void *,uint32_t) __attribute__((weak_import));
static void *gpu_trace_data[1];
static uint32_t gpu_trace_size[1];
static const char *gpu_trace_name[1]={"gpu.trace.bin"};
static const uint32_t gpu_trace_capacity[1]={32+(65536+4096)*120};


static void note(const char *text, const char *path, int error) {
  if (!diag_note) return;
  int n = snprintf(diag_note + diag_note_used, DiagNote - diag_note_used, "%s %s %d\n", text, path, error);
  if (n > 0 && diag_note_used + (size_t)n < DiagNote) diag_note_used += (size_t)n;
}

static void add(const char *path, uint32_t root, vfs_context_t ctx) {
  if(!bl_latency_file(path))return;
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

// Touch reports: kernel.touch.bin, the time of every report the touch driver handed to the HID event driver (HidInputProbe.cpp).
extern uint64_t mach_absolute_time(void);
extern void absolutetime_to_nanoseconds(uint64_t, uint64_t *);
extern volatile uint64_t *native_touch_report_ring __attribute__((weak_import));
extern volatile uint32_t native_touch_report_count __attribute__((weak_import));
enum {TouchRing = 65536};
// Display refresh ticks (LegacyDisplayProbe.cpp), kernel.vsync.bin: header "VSY1", total, kept, 0; then u64 (microseconds << 3) | event
// (0 armed, 1 sent, 2 idle stop, 3 late, 4 started, 5 refresh reported), oldest first.
#include "TouchTrace.h"
_Static_assert(sizeof(struct touch_trace_row) == 48, "TPR1 row");
extern const struct touch_trace_row *sheng_touch_trace_freeze(uint32_t *) __attribute__((weak_import));
extern unsigned native_legacy_prepare_report(char *,unsigned) __attribute__((weak_import));
static const struct touch_trace_row *path_trace;
static uint32_t path_total, path_kept;
extern uint64_t *native_legacy_vsync_ring __attribute__((weak_import));
extern volatile uint32_t native_legacy_vsync_count __attribute__((weak_import));
enum {VsyncRing = 65536};
// CPU frequency governor windows (CpuFreqProbe.cpp), kernel.cpufreq.bin: header "CPF1", total, kept, 0; then 32-byte records
// {u64 microseconds, u16 voted MHz[3], u16 running MHz[3], u16 busiest CPU per mille[3], u16 flags (1 touch boost, 2 frame floor), u32 0},
// domains 0 the A510s, 1 the A715/A710s, 2 the X3.
struct cpufreq_rec {uint64_t abs; uint16_t vote_mhz[3], state_mhz[3], busy_pm[3]; uint16_t flags; uint32_t reserved;};
_Static_assert(sizeof(struct cpufreq_rec) == 32, "cpufreq record");
extern struct cpufreq_rec *native_cpufreq_ring __attribute__((weak_import));
extern volatile uint32_t native_cpufreq_count __attribute__((weak_import));
enum {CpuFreqRing = 32768};
// The client sampler (BootUiTraceProbe.c): kernel.client.bin is its ring in time order (16-byte header "CLI1", u32 total, u32 kept, u32 0;
// then 128-byte records {u64 microseconds, i32 pid, u16 state, i16 priority, u32 context switches, i32 mach_msg peer pid, u64 CPU time
// (mach units), u64 wait event, u64 frames[11] (pc, lr, nine callers)}); kernel.rings.txt has a C row per target (pid, shared cache
// slide, main image address, name). A record lasts until the next one of the same pid (unchanged blocked threads are not repeated).
struct cs_rec {uint64_t abs; int32_t pid; uint16_t state; int16_t pri; uint32_t cs; int32_t peer; uint64_t cpu, wait, frames[11];};
struct cs_target {int pid; unsigned slide; uint64_t image, seen; char name[32];};
// CPU occupancy (BootUiTraceProbe.c), kernel.cpus.bin: header "CPU1", total, kept, 0; then 40-byte records {u64 microseconds, i16 priority
// of what runs on CPU 0..7, u8 idle thread on CPU 0..7 (2: the sampler itself), u8 processor state 0..7}.
struct cs_cpu {uint64_t abs; int16_t pri[8]; uint8_t idle[8], state[8];};
extern struct cs_cpu *boot_cpu_ring __attribute__((weak_import));
extern volatile uint32_t boot_cpu_count __attribute__((weak_import));
enum {CpuRing = 65536};
static uint32_t cpu_kept, cpu_keep;
// Process CPU time (BootUiTraceProbe.c), kernel.ptime.bin: header "PTM1", total, kept, 0; then 24-byte records {u64 microseconds of the
// pass, i32 pid, u32 user microseconds, u32 system microseconds since the pid's last pass (100 ms apart), i16 base priority, u16 0}.
struct cs_ptime {uint64_t abs; int32_t pid; uint32_t user_us, system_us; int16_t priority; uint16_t reserved;};
extern struct cs_ptime *boot_ptime_ring __attribute__((weak_import));
extern const char *boot_ptime_name(int) __attribute__((weak_import));   // the name the CPU time pass saw: a process gone by the export has none
extern volatile uint32_t boot_ptime_count __attribute__((weak_import));
enum {PtimeRing = 131072};   // BootUiTraceProbe.c CS_PTIME_RING
static uint32_t ptime_kept, ptime_keep;
// Large GPU allocations (the driver's BoAlloc hook, NativeAdrenoPublish.cpp), kernel.boalloc.bin: header "BOA1", total, kept, 0; then
// 256-byte records {the driver's 56-byte phase record (u64 microseconds entered; u32 KiB, flags, then microseconds: limit lock, create,
// wire, clean, lock, dispatch, discard; u32 spans, i32 status, u32 0), i32 pid, u32 0, then the caller's user stack: u64 shared cache
// slide, u64 frames[16], two images {u64 Mach-O header address, u8 uuid[16]}, u32 frame count, u32 walk error}.
struct bo_trace {uint64_t entered; uint32_t kib, flags, limit_us, create_us, wire_us, clean_us, lock_us, dispatch_us, discard_us, spans;
  int32_t status; uint32_t reserved;};
struct boot_user_image {uint64_t base; uint8_t uuid[16];};
struct boot_user_stack {uint64_t slide, frames[16]; struct boot_user_image images[2]; uint32_t count, error;};
struct boalloc_rec {struct bo_trace trace; int32_t pid; uint32_t reserved; struct boot_user_stack stack;};
_Static_assert(sizeof(struct boalloc_rec) == 256, "boalloc record");
extern void (*native_adreno_bo_hook)(const struct bo_trace *) __attribute__((weak_import));
extern void boot_user_stack_here(struct boot_user_stack *) __attribute__((weak_import));
enum {BoAllocRing = 8192};
static struct boalloc_rec *boalloc_ring;
static volatile uint32_t boalloc_count;
static uint32_t boalloc_kept, boalloc_keep;
static void boalloc_hook(const struct bo_trace *trace) {
  struct boalloc_rec r;
  memset(&r, 0, sizeof r);
  r.trace = *trace; r.pid = proc_selfpid();
  if (&boot_user_stack_here) boot_user_stack_here(&r.stack);
  const uint32_t n = __atomic_fetch_add(&boalloc_count, 1, __ATOMIC_RELAXED);
  boalloc_ring[n & (BoAllocRing - 1)] = r;
}
extern void boot_client_sampler_start(void) __attribute__((weak_import));
extern struct cs_rec *boot_client_ring __attribute__((weak_import));
extern volatile uint32_t boot_client_count __attribute__((weak_import));
extern struct cs_target boot_client_targets[32] __attribute__((weak_import));
extern volatile unsigned boot_client_target_count __attribute__((weak_import));
enum {ClientRing = 262144};   // BootUiTraceProbe.c CS_RING
static uint32_t client_kept, client_keep;
static uint32_t touch_kept,touch_keep;
static uint32_t vsync_kept,vsync_keep,cpufreq_kept,cpufreq_keep;

static void latency_tick(void) {
  if (!&native_legacy_swapstat || native_legacy_swapstat[9] < 50) return;   // the home screen draws
  boot_latency_start();
  if (&boot_client_sampler_start) boot_client_sampler_start();
  // Everything below allocates only now, once the home screen draws: nothing of this trace runs during startup.
  if (&native_adreno_bo_hook && !boalloc_ring) {
    boalloc_ring = IOMallocAligned(sizeof(struct boalloc_rec) * BoAllocRing, PAGE_SIZE);
    if (boalloc_ring) {memset(boalloc_ring, 0, sizeof(struct boalloc_rec) * BoAllocRing); native_adreno_bo_hook = boalloc_hook;}
  }
  if (&native_legacy_vsync_ring && !native_legacy_vsync_ring) {
    uint64_t *ticks = IOMallocAligned(sizeof(uint64_t) * VsyncRing, PAGE_SIZE);
    if (ticks) {memset(ticks, 0, sizeof(uint64_t) * VsyncRing); native_legacy_vsync_ring = ticks;}
  }
  if (&native_cpufreq_ring && !native_cpufreq_ring) {
    struct cpufreq_rec *windows = IOMallocAligned(sizeof(struct cpufreq_rec) * CpuFreqRing, PAGE_SIZE);
    if (windows) {memset(windows, 0, sizeof(struct cpufreq_rec) * CpuFreqRing); native_cpufreq_ring = windows;}
  }
  if (&native_touch_report_ring && !native_touch_report_ring) {
    uint64_t *touches = IOMallocAligned(sizeof(uint64_t) * TouchRing, PAGE_SIZE);
    if (touches) {memset(touches, 0, sizeof(uint64_t) * TouchRing); native_touch_report_ring = touches;}
  }
}

// Scene screenshots (boot-arg sheng-shots=<first second of log time>, absent or 0: none): bursts of DiagBurst shots 50 ms apart (about six
// frames at 120 Hz), one burst every DiagBurstEvery seconds from that second on, up to DiagShots, of the panel at quarter resolution (every
// fourth pixel of every fourth row), kept in memory and exported as screen-<seconds>-<index in the burst>.bgrx (16-byte header "SHOT", u32
// width, height, stride, then BGRX rows). Shots of one burst show a picture that alternates between frames; a static scene repeats. They are
// analysed by code only.
enum {DiagShots = 24, DiagBurst = 6, DiagBurstEvery = 10};
extern volatile uint64_t native_gop_virtual __attribute__((weak_import));
extern void native_legacy_direct_snapshot(void) __attribute__((weak_import));   // direct scanout: the surface on screen into the boot memory
static uint8_t *shot_data[DiagShots];
static uint32_t shot_time[DiagShots], shot_index[DiagShots], shot_w, shot_h, shot_taken, shot_first, shot_first_read, shots_kept;
static void shot_tick(unsigned now) {
  if (!shot_first_read) {shot_first_read = 1; if (!PE_parse_boot_argn("sheng-shots", &shot_first, sizeof shot_first)) shot_first = 0;}
  if (!shot_first || shot_taken >= DiagShots || now < shot_first + (shot_taken / DiagBurst) * DiagBurstEvery) return;
  for (unsigned k = 0; k < DiagBurst && shot_taken < DiagShots; ++k) {
    if (k) IOSleep(50);
    PE_Video video = {};
    if (!(&native_gop_virtual && native_gop_virtual && !PE_current_console(&video) && video.v_depth == 32 && video.v_width >= 4 &&
          video.v_height >= 4 && video.v_width <= 16384 && video.v_height <= 16384 && video.v_rowBytes >= video.v_width * 4)) {
      ++shot_taken;
      continue;
    }
    const uint32_t w = (uint32_t)video.v_width / 4, h = (uint32_t)video.v_height / 4;
    const unsigned n = shot_taken++;
    shot_data[n] = IOMallocAligned((size_t)w * h * 4, PAGE_SIZE);
    if (!shot_data[n]) continue;
    if (&native_legacy_direct_snapshot) native_legacy_direct_snapshot();
    for (uint32_t y = 0; y < h; ++y) {
      uint32_t *out = (uint32_t *)(shot_data[n] + (size_t)y * w * 4);
      const uint32_t *in = (const uint32_t *)(uintptr_t)(native_gop_virtual + (uint64_t)y * 4 * video.v_rowBytes);
      for (uint32_t x = 0; x < w; ++x) out[x] = in[x * 4];
    }
    shot_w = w; shot_h = h; shot_time[n] = now; shot_index[n] = k;
  }
}

// Called once per diagnostic log round; starts the latency collectors and takes the scene screenshots.
void boot_diag_shot_tick(unsigned now) {latency_tick();shot_tick(now);}

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
  if (&native_legacy_prepare_report) {
    char status[256];
    native_legacy_prepare_report(status, sizeof status);
    RT("# display: %s; vsync ring allocated=%u recorded=%u\n", status,
       (unsigned)(&native_legacy_vsync_ring && native_legacy_vsync_ring != NULL),
       &native_legacy_vsync_count ? native_legacy_vsync_count : 0);
  }
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
    RT("# gpu timeline: T <fence> <buffers> <submit us> <cp start ticks> <cp end ticks> (19.2 MHz) <level at submit> <ring status 1 attached 2 stamps 4 page 8 acquired 16 read> <pid> <seen retired us> <ring> <priority+1> <gpu role> <ring the CP ran> <switch in flight> <restarts that replayed it> <replay us> (total %u; raw words, zero when the CP wrote nothing)\n", n);
    const uint32_t cap = native_adreno_tl_cap;
    for (uint32_t i = n > cap ? n - cap : 0; i < n; ++i) {
      const volatile uint64_t *r = native_adreno_tl[i & (cap - 1)];
      RT("T %u %u %llu %llu %llu %u %u %d %llu %u %u %u %u %u %u %llu\n", (unsigned)(r[0] & 0xffffffffu), (unsigned)((r[0] >> 32) & 0x03ffffffu), r[1], r[2], r[3], (unsigned)(r[4] & 0xffffffffu),
         (unsigned)(r[0] >> 58), (int)(r[4] >> 32), r[5], (unsigned)(r[6] & 15), (unsigned)((r[6] >> 4) & 15), (unsigned)((r[6] >> 8) & 255),
         (unsigned)((r[6] >> 16) & 15), (unsigned)((r[6] >> 20) & 15), (unsigned)((r[6] >> 24) & 255), r[7]);
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
  {
    // Names of every pid in the allocation and CPU time rings: N <pid> <name>.
    int seen[512]; unsigned seen_n = 0;   // every process that ran: the CPU time ring names them all
    #define NOTE_PID(p_) do {const int p__ = (p_); unsigned k_ = 0; while (k_ < seen_n && seen[k_] != p__) ++k_; \
      if (k_ == seen_n && seen_n < 512 && p__ > 0) {seen[seen_n++] = p__; char nm_[32] = {0}; proc_name(p__, nm_, sizeof nm_); \
        if (!nm_[0] && &boot_ptime_name && boot_ptime_name(p__)) strlcpy(nm_, boot_ptime_name(p__), sizeof nm_); RT("N %d %s\n", p__, nm_);}} while (0)
    RT("# pid names (allocation and CPU time rings): N <pid> <name>; kernel reference boot_diag_shot_tick=%llx (unslides unnest frames)\n",
       (unsigned long long)(uintptr_t)&boot_diag_shot_tick);
    if (boalloc_kept) {const uint32_t kept = boalloc_kept > BoAllocRing ? BoAllocRing : boalloc_kept; for (uint32_t i = 0; i < kept; ++i) NOTE_PID(boalloc_ring[(boalloc_kept - kept + i) & (BoAllocRing - 1)].pid);}
    if (ptime_kept) {const uint32_t kept = ptime_kept > PtimeRing ? PtimeRing : ptime_kept; for (uint32_t i = 0; i < kept; ++i) NOTE_PID(boot_ptime_ring[(ptime_kept - kept + i) & (PtimeRing - 1)].pid);}
    #undef NOTE_PID
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
  latency_size=0;
  if(!latency_data)latency_data=IOMallocAligned(LatencyCapacity,PAGE_SIZE);
  if(latency_data) {
    uint32_t n=boot_latency_snapshot(latency_data,LatencyCapacity);
    if(n && used+24+strlen("kernel.latency.bin")+n<=budget){latency_size=n;used+=24+strlen("kernel.latency.bin")+n;}
    else if(n)note("latency-budget","kernel.latency.bin",(int)n);
  }
  // Freeze the GPU data before scanning other files, and reserve its export budget first.
  for(unsigned i=0;i<1;++i) {
    gpu_trace_size[i]=0;
    if(!&native_gpu_trace_snapshot)continue;
    if(!gpu_trace_data[i])gpu_trace_data[i]=IOMallocAligned(gpu_trace_capacity[i],PAGE_SIZE);
    if(!gpu_trace_data[i]){note("gpu-allocate",gpu_trace_name[i],-1);continue;}
    uint32_t n=native_gpu_trace_snapshot(gpu_trace_data[i],gpu_trace_capacity[i]);
    if(n && used+24+strlen(gpu_trace_name[i])+n<=budget){gpu_trace_size[i]=n;used+=24+strlen(gpu_trace_name[i])+n;}
    else if(n)note("gpu-budget",gpu_trace_name[i],(int)n);
  }
  // The scene screenshots, which a run asks for by sheng-shots, come before the files: they are what such a run is for.
  shots_kept = 0;
  for (unsigned i = 0; i < DiagShots; ++i) {
    if (!shot_data[i]) continue;
    const uint64_t cost = 24 + strlen("screen-000-0.bgrx") + 16 + (uint64_t)shot_w * shot_h * 4;
    if (used + cost <= budget) {++shots_kept; used += cost;} else shot_time[i] = 0xffffffffu;
  }
  path_total = 0;
  path_trace = sheng_touch_trace_freeze ? sheng_touch_trace_freeze(&path_total) : NULL;
  path_kept = path_total > TouchTraceCapacity ? TouchTraceCapacity : path_total;
  if (path_trace) {
    const uint64_t cost = 24 + strlen("kernel.touchpath.bin") + 16 + (uint64_t)path_kept * sizeof(*path_trace);
    if (used + cost <= budget) used += cost;
    else {note("touch-budget", "kernel.touchpath.bin", (int)path_kept); path_trace = NULL;}
  }
  rings_keep=0;
  // Files are scanned before optional exports. Frame records have first claim
  // on the budget, so fast IPC and scene data cannot displace the causal trace.
  vfs_context_t ctx = vfs_context_kernel();
  // dasd aborts on a nil path for this bundle resource: whether the kernel sees it (diag-skipped.txt "resource <path> <errno>").
  {
    static const char dasd_model[]="/usr/libexec/freezer_app_ranking_model.mlmodelc";
    vnode_t model=NULLVP;const int error=vnode_lookup(dasd_model,0,&model,ctx);
    if(!error)vnode_put(model);
    note("resource",dasd_model,error);
  }
  for(unsigned i=0;i<sizeof diag_roots/sizeof diag_roots[0];++i)scan(diag_roots[i],0,i,ctx);
  qsort(diag_files,diag_count,sizeof diag_files[0],worth);
  unsigned crash_count[5]={0};uint64_t crash_bytes=0;uint32_t files_kept=0;
  for(uint32_t i=0;i<diag_count;++i) {
    struct diag_file *f=&diag_files[i];int process=bl_crash_process(f->path);
    uint64_t cost=24+strlen(f->path)-1+f->size;
    if(process>=0 && (crash_count[process]>=2 || f->size>4*1024*1024 || crash_bytes+f->size>8*1024*1024))continue;
    if(used+cost>budget){note("skipped",f->path,(int)(f->size>>10));++diag_skipped_count;continue;}
    f->keep=1;used+=cost;++files_kept;
    if(process>=0){++crash_count[process];crash_bytes+=f->size;}
  }
  boot_diag_files=files_kept;
  client_kept = (&boot_client_count && &boot_client_ring && boot_client_ring) ? boot_client_count : 0;
  cpu_kept = (&boot_cpu_count && &boot_cpu_ring && boot_cpu_ring) ? boot_cpu_count : 0;
  ptime_kept = (&boot_ptime_count && &boot_ptime_ring && boot_ptime_ring) ? boot_ptime_count : 0;
  boalloc_kept = boalloc_ring ? boalloc_count : 0;
  rings_build();
  if (rings_text && rings_len) {
    const uint64_t cost = 24 + strlen("kernel.rings.txt") + rings_len;
    if (used + cost <= budget) {rings_keep = 1; used += cost;}
  }
  client_keep = 0;
  touch_keep = 0;
  ptime_keep = 0;
  if (ptime_kept) {
    const uint32_t kept = ptime_kept > PtimeRing ? PtimeRing : ptime_kept;
    const uint64_t cost = 24 + strlen("kernel.ptime.bin") + 16 + (uint64_t)kept * sizeof(struct cs_ptime);
    if (used + cost <= budget) {ptime_keep = 1; used += cost;}
  }
  cpu_keep = 0;
  if (cpu_kept) {
    const uint32_t kept = cpu_kept > CpuRing ? CpuRing : cpu_kept;
    const uint64_t cost = 24 + strlen("kernel.cpus.bin") + 16 + (uint64_t)kept * sizeof(struct cs_cpu);
    if (used + cost <= budget) {cpu_keep = 1; used += cost;}
  }
  boalloc_keep = 0;
  if (boalloc_kept) {
    const uint32_t kept = boalloc_kept > BoAllocRing ? BoAllocRing : boalloc_kept;
    const uint64_t cost = 24 + strlen("kernel.boalloc.bin") + 16 + (uint64_t)kept * sizeof(struct boalloc_rec);
    if (used + cost <= budget) {boalloc_keep = 1; used += cost;}
  }
  vsync_keep = cpufreq_keep = 0;
  vsync_kept = (&native_legacy_vsync_count && &native_legacy_vsync_ring && native_legacy_vsync_ring) ? native_legacy_vsync_count : 0;
  if (&native_legacy_vsync_ring && native_legacy_vsync_ring) {
    const uint32_t kept = vsync_kept > VsyncRing ? VsyncRing : vsync_kept;
    const uint64_t cost = 24 + strlen("kernel.vsync.bin") + 16 + (uint64_t)kept * 8;
    if (used + cost <= budget) {vsync_keep = 1; used += cost;}
  }
  cpufreq_kept = (&native_cpufreq_count && &native_cpufreq_ring && native_cpufreq_ring) ? native_cpufreq_count : 0;
  if (cpufreq_kept) {
    const uint32_t kept = cpufreq_kept > CpuFreqRing ? CpuFreqRing : cpufreq_kept;
    const uint64_t cost = 24 + strlen("kernel.cpufreq.bin") + 16 + (uint64_t)kept * sizeof(struct cpufreq_rec);
    if (used + cost <= budget) {cpufreq_keep = 1; used += cost;}
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
  boot_diag_skipped = diag_skipped_count;
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
  const uint32_t root_len = 1, count = boot_diag_files + 1 + rings_keep + client_keep + touch_keep + !!path_trace + vsync_keep + cpufreq_keep + cpu_keep + boalloc_keep + ptime_keep + !!gpu_trace_size[0] + !!latency_size + shots_kept;
  if (!rc) rc = emit(&root_len, 4, arg);
  if (!rc) rc = emit("/", 1, arg);
  if (!rc) rc = emit(&count, 4, arg);
  for(unsigned i=0;i<1 && !rc;++i)if(gpu_trace_size[i]) {
    rc=put_head(emit,arg,gpu_trace_name[i],0100644,0,0,gpu_trace_size[i]);
    for(uint32_t at=0;at<gpu_trace_size[i] && !rc;) {
      uint32_t n=gpu_trace_size[i]-at;if(n>65536)n=65536;
      rc=emit((const char *)gpu_trace_data[i]+at,n,arg);at+=n;
    }
  }
  if(!rc && latency_size) {
    rc=put_head(emit,arg,"kernel.latency.bin",0100644,0,0,latency_size);
    for(uint32_t at=0;at<latency_size && !rc;) {
      uint32_t n=latency_size-at;if(n>65536)n=65536;
      rc=emit((const char *)latency_data+at,n,arg);at+=n;
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
  for (unsigned i = 0; !rc && i < DiagShots; ++i) {
    if (!shot_data[i] || shot_time[i] == 0xffffffffu) continue;
    char name[24];
    snprintf(name, sizeof name, "screen-%03u-%u.bgrx", shot_time[i] % 1000, shot_index[i] % 10);
    const uint32_t row = shot_w * 4, head[4] = {'S' | 'H' << 8 | 'O' << 16 | (uint32_t)'T' << 24, shot_w, shot_h, row};
    rc = put_head(emit, arg, name, 0100644, 0, 0, 16 + (uint64_t)row * shot_h);
    if (!rc) rc = emit(head, sizeof head, arg);
    for (uint32_t y = 0; !rc && y < shot_h; ++y) rc = emit(shot_data[i] + (size_t)y * row, row, arg);
  }
  if (!rc && boalloc_keep) {
    const uint32_t total = boalloc_kept, kept = total > BoAllocRing ? BoAllocRing : total;
    const uint32_t head[4] = {'B' | 'O' << 8 | 'A' << 16 | (uint32_t)'1' << 24, total, kept, 0};
    rc = put_head(emit, arg, "kernel.boalloc.bin", 0100644, 0, 0, 16 + (uint64_t)kept * sizeof(struct boalloc_rec));
    if (!rc) rc = emit(head, sizeof head, arg);
    for (uint32_t at = 0; !rc && at < kept;) {
      const uint32_t n = kept - at < 512 ? kept - at : 512;
      for (uint32_t k = 0; k < n; ++k) {
        struct boalloc_rec out = boalloc_ring[(total - kept + at + k) & (BoAllocRing - 1)];
        uint64_t ns = 0;
        absolutetime_to_nanoseconds(out.trace.entered, &ns);
        out.trace.entered = ns / 1000;
        memcpy(chunk + (size_t)k * sizeof out, &out, sizeof out);
      }
      rc = emit(chunk, n * sizeof(struct boalloc_rec), arg);
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
  if (!rc && ptime_keep) {
    const uint32_t total = ptime_kept, kept = total > PtimeRing ? PtimeRing : total;
    const uint32_t head[4] = {'P' | 'T' << 8 | 'M' << 16 | (uint32_t)'1' << 24, total, kept, 0};
    rc = put_head(emit, arg, "kernel.ptime.bin", 0100644, 0, 0, 16 + (uint64_t)kept * sizeof(struct cs_ptime));
    if (!rc) rc = emit(head, sizeof head, arg);
    for (uint32_t at = 0; !rc && at < kept;) {
      const uint32_t n = kept - at < 8192 ? kept - at : 8192;
      for (uint32_t k = 0; k < n; ++k) {
        struct cs_ptime out = boot_ptime_ring[(total - kept + at + k) & (PtimeRing - 1)];
        uint64_t ns = 0;
        absolutetime_to_nanoseconds(out.abs, &ns);
        out.abs = ns / 1000;
        memcpy(chunk + (size_t)k * sizeof out, &out, sizeof out);
      }
      rc = emit(chunk, n * sizeof(struct cs_ptime), arg);
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
  if (!rc && path_trace) {
    const uint32_t head[4] = {'T' | 'P' << 8 | 'R' << 16 | (uint32_t)'1' << 24,
                              path_total, path_kept, sizeof(*path_trace)};
    rc = put_head(emit, arg, "kernel.touchpath.bin", 0100644, 0, 0, 16 + (uint64_t)path_kept * sizeof(*path_trace));
    if (!rc) rc = emit(head, sizeof head, arg);
    for (uint32_t at = 0; !rc && at < path_kept;) {
      const uint32_t index = (path_total - path_kept + at) % TouchTraceCapacity;
      uint32_t n = path_kept - at;
      if (n > TouchTraceCapacity - index) n = TouchTraceCapacity - index;
      if (n > DiagChunk / sizeof(*path_trace)) n = DiagChunk / sizeof(*path_trace);
      rc = emit(path_trace + index, n * sizeof(*path_trace), arg);
      at += n;
    }
  }
  if (!rc && vsync_keep) {
    const uint32_t total = vsync_kept, kept = total > VsyncRing ? VsyncRing : total;
    const uint32_t head[4] = {'V' | 'S' << 8 | 'Y' << 16 | (uint32_t)'1' << 24, total, kept, 0};
    rc = put_head(emit, arg, "kernel.vsync.bin", 0100644, 0, 0, 16 + (uint64_t)kept * 8);
    if (!rc) rc = emit(head, sizeof head, arg);
    for (uint32_t at = 0; !rc && at < kept;) {
      const uint32_t n = kept - at < 8192 ? kept - at : 8192;
      for (uint32_t k = 0; k < n; ++k) {
        const uint64_t v = native_legacy_vsync_ring[(total - kept + at + k) & (VsyncRing - 1)];
        uint64_t ns = 0;
        absolutetime_to_nanoseconds(v >> 3, &ns);
        const uint64_t out = ((ns / 1000) << 3) | (v & 7);
        memcpy(chunk + (size_t)k * 8, &out, 8);
      }
      rc = emit(chunk, n * 8, arg);
      at += n;
    }
  }
  if (!rc && cpufreq_keep) {
    const uint32_t total = cpufreq_kept, kept = total > CpuFreqRing ? CpuFreqRing : total;
    const uint32_t head[4] = {'C' | 'P' << 8 | 'F' << 16 | (uint32_t)'1' << 24, total, kept, 0};
    rc = put_head(emit, arg, "kernel.cpufreq.bin", 0100644, 0, 0, 16 + (uint64_t)kept * sizeof(struct cpufreq_rec));
    if (!rc) rc = emit(head, sizeof head, arg);
    for (uint32_t at = 0; !rc && at < kept;) {
      const uint32_t n = kept - at < 4096 ? kept - at : 4096;
      for (uint32_t k = 0; k < n; ++k) {
        struct cpufreq_rec r = native_cpufreq_ring[(total - kept + at + k) & (CpuFreqRing - 1)];
        uint64_t ns = 0;
        absolutetime_to_nanoseconds(r.abs, &ns);
        r.abs = ns / 1000;
        memcpy(chunk + (size_t)k * sizeof r, &r, sizeof r);
      }
      rc = emit(chunk, n * sizeof(struct cpufreq_rec), arg);
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
