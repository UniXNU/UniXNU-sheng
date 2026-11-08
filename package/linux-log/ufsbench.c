// ufsbench: read-only block device benchmark for the sheng Linux reader (aarch64, static, no libc, raw syscalls).
// usage: ufsbench DEVICE FIRST_BYTE REGION_BYTES [SECONDS]
// Every read is O_DIRECT pread64 inside [FIRST_BYTE, FIRST_BYTE + REGION_BYTES). Queue depth N = N forked processes, each with one
// read in flight. Random offsets come from a fixed-seed xorshift per (test, process), so another OS can read the same LBAs.
// Output: one line per test: kind bs qd ops iops MB/s avg p50 p99 max (us) errors.
typedef unsigned long u64;
typedef long s64;
typedef unsigned int u32;

static s64 sys6(s64 n, s64 a, s64 b, s64 c, s64 d, s64 e, s64 f) {
  register s64 x8 __asm__("x8") = n, x0 __asm__("x0") = a, x1 __asm__("x1") = b, x2 __asm__("x2") = c, x3 __asm__("x3") = d,
                                  x4 __asm__("x4") = e, x5 __asm__("x5") = f;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5) : "memory", "cc");
  return x0;
}
#define SYS_openat 56
#define SYS_close 57
#define SYS_pipe2 59
#define SYS_read 63
#define SYS_write 64
#define SYS_pread64 67
#define SYS_exit_group 94
#define SYS_clock_gettime 113
#define SYS_clone 220
#define SYS_wait4 260
#define O_DIRECT 0200000
#define AT_FDCWD (-100)

static u64 now_ns(void) { s64 ts[2]; sys6(SYS_clock_gettime, 1, (s64)ts, 0, 0, 0, 0); return (u64)ts[0] * 1000000000UL + (u64)ts[1]; }
static void out(const char *s, u64 n) { sys6(SYS_write, 1, (s64)s, (s64)n, 0, 0, 0); }
static u64 len(const char *s) { u64 n = 0; while (s[n]) ++n; return n; }
static void puts_(const char *s) { out(s, len(s)); }
static void putu(u64 v) { char b[24]; int i = 23; b[i] = 0; do { b[--i] = (char)('0' + v % 10); v /= 10; } while (v); puts_(b + i); }
static void putfix(u64 milli) { putu(milli / 1000); puts_("."); u64 f = milli % 1000; if (f < 100) puts_("0"); if (f < 10) puts_("0"); putu(f); }
static u64 parse(const char *s) { u64 v = 0; while (*s >= '0' && *s <= '9') v = v * 10 + (u64)(*s++ - '0'); return v; }
static void *memset_(void *d, int c, u64 n) { unsigned char *p = d; while (n--) *p++ = (unsigned char)c; return d; }
void *memset(void *d, int c, u64 n) { return memset_(d, c, n); }
void *memcpy(void *d, const void *s, u64 n) { unsigned char *p = d; const unsigned char *q = s; while (n--) *p++ = *q++; return d; }

enum { kBuckets = 4000, kBucketNs = 5000 };   // 5 us buckets up to 20 ms, the last one holds everything longer
struct stats { u64 ops, sum_ns, max_ns, errors; u32 hist[kBuckets]; };
static unsigned char buffer[1 << 20] __attribute__((aligned(4096)));
static struct stats mine, total;

static void run_child(int fd, int pipe_w, u64 first, u64 region, u64 bs, int random, u64 seed, u64 stream, u64 streams, u64 deadline) {
  memset_(&mine, 0, sizeof mine);
  u64 x = seed * 0x9e3779b97f4a7c15UL + 1, blocks = region / bs, pos = blocks / streams * stream;
  while (now_ns() < deadline) {
    u64 index;
    if (random) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; index = x % blocks; }
    else { index = pos++; if (pos >= blocks) pos = 0; }
    u64 t0 = now_ns();
    s64 r = sys6(SYS_pread64, fd, (s64)buffer, (s64)bs, (s64)(first + index * bs), 0, 0);
    u64 dt = now_ns() - t0;
    if (r != (s64)bs) { ++mine.errors; continue; }
    ++mine.ops; mine.sum_ns += dt; if (dt > mine.max_ns) mine.max_ns = dt;
    u64 b = dt / kBucketNs; mine.hist[b < kBuckets ? b : kBuckets - 1]++;
  }
  const char *p = (const char *)&mine; u64 left = sizeof mine;
  while (left) { s64 w = sys6(SYS_write, pipe_w, (s64)p, (s64)left, 0, 0, 0); if (w <= 0) break; p += w; left -= (u64)w; }
  sys6(SYS_exit_group, 0, 0, 0, 0, 0, 0);
}

static u64 percentile(u64 permille) {
  u64 want = (total.ops * permille + 999) / 1000, seen = 0;
  for (u32 i = 0; i < kBuckets; ++i) { seen += total.hist[i]; if (seen >= want && want) return (u64)(i + 1) * kBucketNs; }
  return 0;
}

static void test(const char *dev, u64 first, u64 region, u64 bs, u32 qd, int random, u64 seconds, u32 id) {
  int pfd[32][2];   // one result pipe per process: a result is larger than an atomic pipe write
  for (u32 i = 0; i < qd; ++i) if (sys6(SYS_pipe2, (s64)pfd[i], 0, 0, 0, 0, 0) < 0) { puts_("pipe failed\n"); return; }
  s64 fd = sys6(SYS_openat, AT_FDCWD, (s64)dev, O_DIRECT, 0, 0, 0);
  if (fd < 0) { puts_("open failed\n"); return; }
  u64 start = now_ns() + 50000000UL, deadline = start + seconds * 1000000000UL;
  for (u32 i = 0; i < qd; ++i) {
    s64 pid = sys6(SYS_clone, 17 /* SIGCHLD */, 0, 0, 0, 0, 0);
    if (pid == 0) { while (now_ns() < start) {} run_child((int)fd, pfd[i][1], first, region, bs, random, ((u64)id << 8) | i, i, qd, deadline); }
    sys6(SYS_close, pfd[i][1], 0, 0, 0, 0, 0);
  }
  memset_(&total, 0, sizeof total);
  for (u32 i = 0; i < qd; ++i) {
    char *p = (char *)&mine; u64 left = sizeof mine;
    while (left) { s64 r = sys6(SYS_read, pfd[i][0], (s64)p, (s64)left, 0, 0, 0); if (r <= 0) break; p += r; left -= (u64)r; }
    sys6(SYS_close, pfd[i][0], 0, 0, 0, 0, 0);
    if (left) { puts_("short result\n"); break; }
    total.ops += mine.ops; total.sum_ns += mine.sum_ns; total.errors += mine.errors;
    if (mine.max_ns > total.max_ns) total.max_ns = mine.max_ns;
    for (u32 b = 0; b < kBuckets; ++b) total.hist[b] += mine.hist[b];
  }
  for (u32 i = 0; i < qd; ++i) { int st; sys6(SYS_wait4, -1, (s64)&st, 0, 0, 0, 0); }
  sys6(SYS_close, fd, 0, 0, 0, 0, 0);
  puts_(random ? "rand" : "seq "); puts_(" bs="); putu(bs); puts_(" qd="); putu(qd); puts_(" ops="); putu(total.ops);
  puts_(" iops="); putu(total.ops / seconds);
  puts_(" MBps="); putfix(total.ops * bs / seconds * 1000 / 1000000);
  puts_(" avg_us="); putfix(total.ops ? total.sum_ns / total.ops : 0);
  puts_(" p50_us="); putu(percentile(500) / 1000); puts_(" p99_us="); putu(percentile(990) / 1000);
  puts_(" max_us="); putu(total.max_ns / 1000); puts_(" err="); putu(total.errors); puts_("\n");
}

int main(int argc, char **argv) {
  if (argc < 4) { puts_("usage: ufsbench DEVICE FIRST_BYTE REGION_BYTES [SECONDS]\n"); return 2; }
  const char *dev = argv[1];
  u64 first = parse(argv[2]), region = parse(argv[3]), seconds = argc > 4 ? parse(argv[4]) : 3;
  static const u64 sizes[] = {4096, 16384, 131072, 1048576};
  static const u32 depths[] = {1, 8, 32};
  u32 id = 1;
  for (u32 s = 0; s < 4; ++s) for (u32 d = 0; d < 3; ++d) test(dev, first, region, sizes[s], depths[d], 1, seconds, id++);
  test(dev, first, region, 131072, 1, 0, seconds, id++);
  test(dev, first, region, 1048576, 1, 0, seconds, id++);
  test(dev, first, region, 1048576, 8, 0, seconds, id++);
  puts_("done\n");
  return 0;
}

__asm__(".globl _start\n_start:\n ldr x0, [sp]\n add x1, sp, #8\n bl main\n mov x8, #94\n svc #0\n");
