// SPDX-License-Identifier: MIT
// File-backed pages overlap reclaimable queues; retain the raw counters instead of adding them.
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <libproc.h>
#include <mach/mach.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <time.h>
#include <unistd.h>
#include <uuid/uuid.h>

static volatile sig_atomic_t stopping;
static void stop(int sig) { (void)sig; stopping = 1; }
static double seconds(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}
static uint32_t crc32(const char *s, size_t n) {
    uint32_t c = ~0u;
    for (size_t i = 0; i < n; ++i) {
        c ^= (unsigned char)s[i];
        for (unsigned b = 0; b < 8; ++b) c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1u)));
    }
    return ~c;
}
static void quoted(FILE *f, const char *s) {
    fputc('"', f);
    for (; *s; ++s) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') fprintf(f, "\\%c", c);
        else if (c < 32 || c >= 127) fprintf(f, "\\u%04x", c);
        else fputc(c, f);
    }
    fputc('"', f);
}
static int write_all(int fd, const char *p, size_t n) {
    while (n) {
        ssize_t done = write(fd, p, n);
        if (done < 0 && errno == EINTR) continue;
        if (done <= 0) return -1;
        p += done;
        n -= (size_t)done;
    }
    return 0;
}
static int open_log(const char *path) {
    int fd = open(path, O_WRONLY | O_APPEND | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    struct stat st;
    if (fd < 0) return -1;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() || st.st_nlink != 1) {
        close(fd); errno = EPERM; return -1;
    }
    if (fchmod(fd, 0600)) { close(fd); return -1; }
    return fd;
}
static int rotate(const char *path) {
    char from[1024], to[1024];
    for (int i = 3; i >= 1; --i) {
        snprintf(from, sizeof from, i == 1 ? "%s" : "%s.%d", path, i - 1);
        snprintf(to, sizeof to, "%s.%d", path, i);
        if (rename(from, to) && errno != ENOENT) return -1;
    }
    return 0;
}
static int sync_parent(const char *path) {
    char parent[1024]; snprintf(parent, sizeof parent, "%s", path);
    char *slash = strrchr(parent, '/');
    if (slash) { if (slash == parent) slash[1] = 0; else *slash = 0; }
    else strcpy(parent, ".");
    int fd = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return -1;
    int error = fsync(fd); int saved = errno; close(fd); errno = saved;
    return error;
}
static void storage(FILE *f, const char *name, const char *path) {
    struct statfs st;
    int rc = statfs(path, &st);
    fprintf(f, ",\"%s\":{\"error\":%d", name, rc ? errno : 0);
    if (!rc) {
        fprintf(f, ",\"total_bytes\":%llu,\"available_bytes\":%llu,\"source\":",
            (unsigned long long)st.f_blocks * st.f_bsize,
            (unsigned long long)st.f_bavail * st.f_bsize);
        quoted(f, st.f_mntfromname);
        fputs(",\"mount\":", f); quoted(f, st.f_mntonname);
    }
    fputc('}', f);
}
static void processes(FILE *f) {
    struct entry { int pid; uint64_t rss, footprint, user_ns, system_ns; char name[64]; } top[12] = {0};
    int pids[8192];
    int n = proc_listpids(PROC_ALL_PIDS, 0, pids, sizeof pids);
    fprintf(f, ",\"process_scan_error\":%d,\"process_scan_truncated\":%s,\"processes\":[",
        n <= 0 ? errno : 0, n >= (int)sizeof pids ? "true" : "false");
    for (int i = 0; i < n / (int)sizeof(int) && i < 8192; ++i) {
        struct rusage_info_v2 r = {0};
        if (pids[i] <= 0 || proc_pid_rusage(pids[i], RUSAGE_INFO_V2, (rusage_info_t *)&r)) continue;
        unsigned at = 0;
        while (at < 12 && top[at].rss >= r.ri_resident_size) ++at;
        if (at == 12) continue;
        memmove(top + at + 1, top + at, (11 - at) * sizeof top[0]);
        top[at] = (struct entry){ .pid = pids[i], .rss = r.ri_resident_size,
            .footprint = r.ri_phys_footprint, .user_ns = r.ri_user_time, .system_ns = r.ri_system_time };
        proc_name(pids[i], top[at].name, sizeof top[at].name);
    }
    for (unsigned i = 0; i < 12 && top[i].pid; ++i) {
        fprintf(f, "%s{\"pid\":%d,\"rss_bytes\":%llu,\"footprint_bytes\":%llu,\"user_ns\":%llu,\"system_ns\":%llu,\"name\":",
            i ? "," : "", top[i].pid, (unsigned long long)top[i].rss,
            (unsigned long long)top[i].footprint, (unsigned long long)top[i].user_ns,
            (unsigned long long)top[i].system_ns);
        quoted(f, top[i].name); fputc('}', f);
    }
    fputc(']', f);
}
static char *sample(uint64_t seq, const char *session, double gap, double previous_io,
                    int sync_error, int full_error, bool detail, size_t *length) {
    char *text = NULL;
    FILE *f = open_memstream(&text, length);
    if (!f) return NULL;
    double begin = seconds();
    fprintf(f, "{\"schema\":2,\"session\":\"%s\",\"seq\":%llu,\"wall_s\":%lld,\"monotonic_s\":%.3f,\"gap_s\":%.3f,\"previous_io_s\":%.3f,\"previous_fsync_error\":%d,\"previous_fullsync_error\":%d",
        session, (unsigned long long)seq, (long long)time(NULL), begin, gap, previous_io, sync_error, full_error);
    vm_statistics64_data_t v = {0};
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    kern_return_t rc = host_statistics64(mach_host_self(), HOST_VM_INFO64, (host_info64_t)&v, &count);
    fprintf(f, ",\"vm_error\":%d,\"page_bytes\":%llu", rc, (unsigned long long)vm_page_size);
    if (rc == KERN_SUCCESS) {
#define FIELD(name, member) fprintf(f, ",\"" name "\":%llu", (unsigned long long)v.member)
        FIELD("free_pages", free_count); FIELD("active_pages", active_count);
        FIELD("inactive_pages", inactive_count); FIELD("speculative_pages", speculative_count);
        FIELD("wired_pages", wire_count); FIELD("purgeable_pages", purgeable_count);
        FIELD("file_backed_pages", external_page_count); FIELD("anonymous_pages", internal_page_count);
        FIELD("compressor_pages", compressor_page_count);
        FIELD("uncompressed_pages", total_uncompressed_pages_in_compressor);
        FIELD("compressions", compressions); FIELD("decompressions", decompressions);
        FIELD("swapins", swapins); FIELD("swapouts", swapouts);
        FIELD("pageins", pageins); FIELD("pageouts", pageouts);
        FIELD("purges", purges); FIELD("reactivations", reactivations);
#undef FIELD
    }
    uint64_t physical = 0; size_t size = sizeof physical;
    int err = sysctlbyname("hw.memsize", &physical, &size, NULL, 0) ? errno : 0;
    fprintf(f, ",\"physical_bytes\":%llu,\"physical_error\":%d", (unsigned long long)physical, err);
    struct xsw_usage swap = {0}; size = sizeof swap;
    err = sysctlbyname("vm.swapusage", &swap, &size, NULL, 0) ? errno : 0;
    fprintf(f, ",\"swap_error\":%d,\"swap_total_bytes\":%llu,\"swap_used_bytes\":%llu", err,
        (unsigned long long)swap.xsu_total, (unsigned long long)swap.xsu_used);
    unsigned pressure = 0; size = sizeof pressure;
    err = sysctlbyname("kern.memorystatus_vm_pressure_level", &pressure, &size, NULL, 0) ? errno : 0;
    fprintf(f, ",\"pressure_error\":%d,\"pressure_level\":%u", err, pressure);
    struct rusage_info_v2 own = {0};
    err = proc_pid_rusage(getpid(), RUSAGE_INFO_V2, (rusage_info_t *)&own) ? errno : 0;
    fprintf(f, ",\"self_io_error\":%d,\"self_disk_read_bytes\":%llu,\"self_disk_write_bytes\":%llu", err,
        (unsigned long long)own.ri_diskio_bytesread, (unsigned long long)own.ri_diskio_byteswritten);
    processor_info_array_t cpus = NULL; natural_t cpu_count = 0;
    mach_msg_type_number_t info_count = 0;
    rc = host_processor_info(mach_host_self(), PROCESSOR_CPU_LOAD_INFO, &cpu_count, &cpus, &info_count);
    fprintf(f, ",\"cpu_error\":%d,\"cpu_ticks\":[", rc);
    if (rc == KERN_SUCCESS) {
        for (natural_t i = 0; i < cpu_count; ++i) {
            processor_cpu_load_info_t c = (processor_cpu_load_info_t)cpus + i;
            fprintf(f, "%s[%u,%u,%u,%u]", i ? "," : "", c->cpu_ticks[CPU_STATE_USER],
                c->cpu_ticks[CPU_STATE_SYSTEM], c->cpu_ticks[CPU_STATE_IDLE], c->cpu_ticks[CPU_STATE_NICE]);
        }
        vm_deallocate(mach_task_self(), (vm_address_t)cpus, info_count * sizeof(integer_t));
    }
    fputc(']', f);
    if (detail) {
        storage(f, "data", "/System/Volumes/Data"); storage(f, "vm_volume", "/System/Volumes/VM");
        processes(f);
        static const char *paths[] = { "/private/var/vm", "/Applications/Safari.app",
            "/System/Applications/Safari.app", "/System/Cryptexes/App",
            "/System/Cryptexes/App/System/Applications/Safari.app/Contents/Info.plist" };
        fputs(",\"paths\":[", f);
        for (unsigned i = 0; i < sizeof paths / sizeof paths[0]; ++i) {
            struct stat st;
            int error = stat(paths[i], &st) ? errno : 0;
            fprintf(f, "%s{\"path\":", i ? "," : ""); quoted(f, paths[i]);
            fprintf(f, ",\"error\":%d}", error);
        }
        fputc(']', f);
    }
    fprintf(f, ",\"sample_s\":%.3f}", seconds() - begin);
    if (fclose(f)) { free(text); return NULL; }
    return text;
}
static unsigned number(const char *s) {
    char *end; errno = 0; unsigned long n = strtoul(s, &end, 10);
    if (errno || !*s || *end || n > 86400) { fprintf(stderr, "Invalid number: %s\n", s); exit(2); }
    return (unsigned)n;
}
int main(int argc, char **argv) {
    const char *path = "/private/var/log/sheng-desktop-diagnostics.log";
    unsigned interval = 5, limit = 4 * 1024 * 1024, samples = 0;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--output") && i + 1 < argc) path = argv[++i];
        else if (!strcmp(argv[i], "--interval") && i + 1 < argc) interval = number(argv[++i]);
        else if (!strcmp(argv[i], "--samples") && i + 1 < argc) samples = number(argv[++i]);
        else if (!strcmp(argv[i], "--limit-kib") && i + 1 < argc) limit = number(argv[++i]) * 1024;
        else { fprintf(stderr, "Usage: DesktopDiagnostics [--output PATH] [--interval SECONDS] [--samples COUNT] [--limit-kib SIZE]\n"); return 2; }
    }
    if (!interval || limit < 16384 || strlen(path) > 900) return 2;
    signal(SIGTERM, stop); signal(SIGINT, stop);
    char lock[1024]; snprintf(lock, sizeof lock, "%s.lock", path);
    int lease = open_log(lock);
    if (lease < 0 || flock(lease, LOCK_EX | LOCK_NB)) { perror("Diagnostic lock"); return 1; }
    int fd = open_log(path);
    if (fd < 0) { perror("Open diagnostics"); return 1; }
    if (sync_parent(path)) { perror("Sync diagnostic directory"); return 1; }
    // Preserve an interrupted tail for the decoder, but start the next session on a new line.
    int reader = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    struct stat old;
    char last;
    if (reader < 0 || fstat(reader, &old)) return 1;
    if (old.st_size && (pread(reader, &last, 1, old.st_size - 1) != 1 ||
        (last != '\n' && write_all(fd, "\n", 1)))) return 1;
    close(reader);
    uuid_t id; char session[37]; uuid_generate(id); uuid_unparse_lower(id, session);
    uint64_t seq = 0; double previous = seconds(), previous_io = 0, full_at = 0, detail_at = 0;
    int sync_error = 0, full_error = 0;
    while (!stopping && (!samples || seq < samples)) {
        double now = seconds(); size_t n = 0;
        bool detail = now >= detail_at;
        if (detail) detail_at = now + 30;
        char *text = sample(++seq, session, now - previous, previous_io, sync_error, full_error, detail, &n);
        previous = now;
        if (!text) { perror("Sample diagnostics"); return 1; }
        char *record = NULL;
        int bytes = asprintf(&record, "{\"crc32\":\"%08x\",\"sample\":%s}\n", crc32(text, n), text);
        free(text);
        if (bytes < 0) return 1;
        struct stat st;
        if (fstat(fd, &st)) { free(record); return 1; }
        if ((uint64_t)st.st_size + (unsigned)bytes > limit) {
            if (fsync(fd) || close(fd) || rotate(path)) { free(record); perror("Rotate diagnostics"); return 1; }
            fd = open_log(path);
            if (fd < 0) { free(record); return 1; }
            if (sync_parent(path)) { free(record); return 1; }
        }
        double io = seconds();
        if (write_all(fd, record, (size_t)bytes)) { free(record); perror("Write diagnostics"); return 1; }
        free(record);
        sync_error = fsync(fd) ? errno : 0;
        if (now >= full_at || (samples && seq == samples)) {
            full_error = fcntl(fd, F_FULLFSYNC) ? errno : 0;
            full_at = now + 30;
        }
        previous_io = seconds() - io;
        if (sync_error || full_error) fprintf(stderr, "Diagnostics sync - seq=%llu fsync=%d fullsync=%d\n",
            (unsigned long long)seq, sync_error, full_error);
        if (samples && seq == samples) break;
        double remain = interval - (seconds() - now);
        if (remain > 0) { struct timespec t = { (time_t)remain, (long)((remain - (time_t)remain) * 1e9) }; nanosleep(&t, NULL); }
    }
    int result = fsync(fd) ? 1 : 0;
    if (fcntl(fd, F_FULLFSYNC)) { perror("Final fullsync"); result = 1; }
    close(fd); close(lease);
    return result;
}
