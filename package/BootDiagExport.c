// SPDX-License-Identifier: MIT
// Diagnostic file export (int-217): the unified log that logd persisted (/private/var/db/diagnostics, with the format strings in
// /private/var/db/uuidtext) and the crash reports, read by the kernel itself, so neither a sandbox nor a launchd job that never runs
// (adrenoprobed) stands in the way. BootLog.c asks for the stream once (sheng-diag-export=<seconds>) and writes it into the half of the
// export region the pipeline-cache blob does not hold; scripts/read-sheng-log.py unpacks it into the run's diag/ directory.
// Stream: "SHEXR001", u32 stream bytes, u32 0, then the cache export's stream (drivers/NativeMetal/CacheExport.h, "M2VCACH1") with root
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

// In the order of their worth: the format strings of earlier runs are on the host already, and the crash reports are rarely needed.
// A root may be a single file: the render server's shader priming state (PrimeShaders.h) in its persistent cache directory, small and
// the only record of how far priming got (backboardd runs as mobile; the root home is the fallback).
static const char *const diag_roots[] = {
  "/private/var/mobile/Library/Caches/com.apple.backboardd/m2v/prime.log",
  "/private/var/mobile/Library/Caches/com.apple.backboardd/m2v/prime-bad",
  "/private/var/root/Library/Caches/com.apple.backboardd/m2v/prime.log",
  "/private/var/root/Library/Caches/com.apple.backboardd/m2v/prime-bad",
  "/private/var/db/diagnostics",
  "/private/var/db/uuidtext",
  "/private/var/mobile/Library/Logs/CrashReporter",
};
extern volatile uint64_t native_gop_virtual __attribute__((weak_import));
extern void qsort(void *, size_t, size_t, int (*)(const void *, const void *));
static uint32_t shot_width, shot_height;   // the half-resolution screenshot; 0 when there is no scanout

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

// The stream's length (without the 16-byte blob header) for at most budget bytes, after scanning the roots; 0 when nothing could be read.
uint64_t boot_diag_scan(uint64_t budget) {
  if (!diag_files) diag_files = IOMallocData(sizeof(struct diag_file) * DiagMaxFiles);
  if (!diag_note) diag_note = IOMallocData(DiagNote);
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
  uint8_t *chunk = IOMallocData(DiagChunk);
  if (!chunk) return -1;
  vfs_context_t ctx = vfs_context_kernel();
  int rc = emit("M2VCACH1", 8, arg);
  const uint32_t root_len = 1, count = boot_diag_files + 1 + (shot_width ? 1 : 0);
  if (!rc) rc = emit(&root_len, 4, arg);
  if (!rc) rc = emit("/", 1, arg);
  if (!rc) rc = emit(&count, 4, arg);
  if (!rc && shot_width) {
    // Every second pixel of every second row, read from the scanout mapping the display presenter set up.
    PE_Video video = {};
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
  IOFreeData(chunk, DiagChunk);
  return rc;
}
