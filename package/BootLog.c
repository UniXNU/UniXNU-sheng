// SPDX-License-Identifier: MIT
// Diagnostic log on UFS (boot argument sheng-log=1): every 2 s the text the board page would show (the diagnostic page, the live
// banner, the GPU rows, a caught panic message) and the kernel message buffer are written to the last 32 MiB of userdata through
// NativeUfs's bounded log window, so a run can be read back afterwards (scripts/read-sheng-log.py over the Linux log reader).
//
// Window layout (4096-byte blocks): 0 header, 1..32 the kernel message buffer (oldest first), 33.. text slots of 8 blocks in a ring,
// then the export (version 3, int-150): the newest complete blob that user space handed over (boot_log_export_put: the Metal plugin's
// pipeline caches, so the next image can start with them) lies in NativeUfs's export region, the 256 MiB below the window, which the
// reader fetches on its own. Its two halves alternate, so the header always names a blob that is wholly on the medium. (Version 2 kept a
// 16 MiB area at the window's tail; the UI's pipeline caches outgrew it in int-148 and int-149.)
// The header is written last in each round, so a reader never trusts a slot or an export the header does not count yet.
#include <kern/thread.h>
#include <kern/clock.h>
#include <string.h>
#include <pexpert/pexpert.h>
#include <kern/misc_protos.h>
#include <IOKit/IOLib.h>

extern int native_ufs_log_open(uint64_t *, uint64_t *, const char **) __attribute__((weak_import));
extern int native_ufs_log_write(uint64_t, uint32_t, const void *) __attribute__((weak_import));
extern uint64_t native_ufs_export_blocks(void) __attribute__((weak_import));
extern int native_ufs_export_write(uint64_t, uint32_t, const void *) __attribute__((weak_import));
extern int native_ufs_log_sync(void) __attribute__((weak_import));
extern unsigned native_ufs_store_gpt_check(void) __attribute__((weak_import));
extern uint32_t cs_text_fixup_parsed __attribute__((weak_import));
extern uint32_t cs_text_fixup_applied __attribute__((weak_import));
extern char boot_progress_page_text[48][120] __attribute__((weak_import));
extern char boot_progress_banner_text[49][120] __attribute__((weak_import));
extern volatile unsigned boot_progress_banner_rows __attribute__((weak_import));
extern char boot_progress_ext[8][48] __attribute__((weak_import));
extern char boot_progress_ext_wide[2][112] __attribute__((weak_import));
extern char boot_progress_panic_text[2048] __attribute__((weak_import));
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
extern volatile int native_gpu_addon_files __attribute__((weak_import));
extern volatile int native_gpu_addon_written __attribute__((weak_import));
extern volatile int native_gpu_addon_same __attribute__((weak_import));
extern volatile int native_gpu_addon_failed __attribute__((weak_import));
extern volatile int native_gpu_addon_error __attribute__((weak_import));
extern volatile int native_gpu_addon_stage __attribute__((weak_import));
extern volatile int native_gpu_addon_status __attribute__((weak_import));
extern volatile int native_gpu_addon2_stage __attribute__((weak_import));
extern volatile int native_gpu_addon2_status __attribute__((weak_import));
extern volatile int native_gpu_addon3_stage __attribute__((weak_import));
extern volatile int native_gpu_addon3_status __attribute__((weak_import));
extern volatile int native_cryptex_os_stage __attribute__((weak_import));
extern volatile int native_cryptex_os_status __attribute__((weak_import));
extern volatile int native_gestalt_cache_status __attribute__((weak_import));
extern volatile int native_ls_store_status __attribute__((weak_import));
extern volatile uint64_t native_data_persist_present __attribute__((weak_import));
extern volatile int native_staged_apps_stage __attribute__((weak_import));
extern volatile int32_t native_buttons_stats[10] __attribute__((weak_import));
extern volatile int32_t native_backlight_stats[16] __attribute__((weak_import));
extern volatile int native_staged_apps_dirs __attribute__((weak_import));
extern volatile int native_staged_apps_files __attribute__((weak_import));
extern volatile int native_staged_apps_mib __attribute__((weak_import));
extern volatile int native_staged_apps_failed __attribute__((weak_import));
extern volatile int native_staged_apps_error __attribute__((weak_import));
extern char native_staged_apps_path[96] __attribute__((weak_import));
extern const char *boot_progress_tag(void) __attribute__((weak_import));
extern unsigned boot_ui_trace_role_rows(unsigned, char [][120], unsigned, unsigned) __attribute__((weak_import));
extern unsigned boot_ui_trace_hot_rows(char [][120], unsigned) __attribute__((weak_import));
extern unsigned long vm_kernel_slide;
extern volatile uint64_t native_gop_wait_stats[7] __attribute__((weak_import));
extern volatile uint32_t native_adreno_fence_wait[8] __attribute__((weak_import));
extern volatile uint32_t native_adreno_stall[24] __attribute__((weak_import));
extern volatile uint32_t native_adreno_stall_mem[59] __attribute__((weak_import));
extern volatile uint32_t native_adreno_unstick[15] __attribute__((weak_import));
extern volatile uint64_t native_adreno_wait_outer[4] __attribute__((weak_import));
extern volatile uint32_t native_adreno_export_stats[4] __attribute__((weak_import));
extern volatile int32_t native_touch_strokes[16][7] __attribute__((weak_import));
extern volatile uint32_t native_touch_stroke_count __attribute__((weak_import));
extern volatile uint32_t native_wifi_stats[29] __attribute__((weak_import));
extern volatile uint32_t native_wifi_dump[108] __attribute__((weak_import));
extern volatile uint32_t native_wifi_pmic[472] __attribute__((weak_import));
extern volatile uint32_t native_wifi_ep[16] __attribute__((weak_import));
extern volatile uint32_t native_wifi_pon[12] __attribute__((weak_import));
extern volatile uint32_t native_wifi_cfg[32] __attribute__((weak_import));
extern volatile uint32_t native_wifi_chip[16] __attribute__((weak_import));
extern volatile uint32_t native_wifi_ev[48] __attribute__((weak_import));
extern volatile uint32_t native_wifi_ldo[24] __attribute__((weak_import));
extern volatile uint32_t native_wifi_mhi[16] __attribute__((weak_import));
extern volatile uint32_t native_wifi_mhi_ms[8] __attribute__((weak_import));
extern char native_wifi_text[512][112] __attribute__((weak_import));
extern volatile uint32_t native_wifi_text_n __attribute__((weak_import));
extern char native_wifi_note[128][112] __attribute__((weak_import));
extern volatile uint32_t native_wifi_note_n __attribute__((weak_import));
extern int native_ufs_extra_fault(uint32_t *) __attribute__((weak_import));
extern struct {uint32_t stage, scans, frames, beacons, bss_seen; int last_err;} nw_wifi_stats __attribute__((weak_import));
extern volatile uint64_t nw_wifi_where[4] __attribute__((weak_import));
extern volatile uint32_t native_dpu_stats[20] __attribute__((weak_import));
extern volatile uint32_t native_touch_jitter[3] __attribute__((weak_import));
extern volatile uint32_t native_touch_hold[14] __attribute__((weak_import));
extern char native_lookup_log[160][112] __attribute__((weak_import));
extern volatile unsigned native_lookup_count __attribute__((weak_import));
extern volatile uint32_t native_dpu_pipes[54] __attribute__((weak_import));
extern volatile int32_t native_adsp_stats[16] __attribute__((weak_import));
extern volatile int32_t native_audio_stats[16] __attribute__((weak_import));
extern char native_adsp_text[96] __attribute__((weak_import));
extern volatile int32_t native_battery_stats[16] __attribute__((weak_import));
extern volatile int32_t native_qrtr_stats[16] __attribute__((weak_import));
extern volatile int32_t native_battery_raw[10] __attribute__((weak_import));
extern volatile int32_t native_charger_stats[16] __attribute__((weak_import));
extern char native_qrtr_text[96] __attribute__((weak_import));
extern volatile uint32_t native_swipe_stats[16] __attribute__((weak_import));
extern volatile int32_t native_touch_extent[4] __attribute__((weak_import));
extern volatile uint32_t native_adreno_method_stats[20][4] __attribute__((weak_import));
extern volatile uint64_t native_adreno_vm_phase_us[2][8] __attribute__((weak_import));
extern volatile uint32_t native_adreno_map_fail[7][2] __attribute__((weak_import));
extern volatile uint32_t native_adreno_pool_stats[8] __attribute__((weak_import));
extern volatile uint32_t native_adreno_wc_audit[6] __attribute__((weak_import));
extern volatile uint32_t native_adreno_submit_ibs[4] __attribute__((weak_import));
extern volatile uint32_t native_touch_edge_stats[4] __attribute__((weak_import));
extern volatile uint32_t native_touch_hid_multi[3] __attribute__((weak_import));
extern volatile int32_t native_backlight_request[6] __attribute__((weak_import));
extern volatile uint64_t native_adreno_reassert_ticks __attribute__((weak_import));
extern volatile uint64_t native_adreno_cache_stats[5] __attribute__((weak_import));
// NativeAdrenoPublish.cpp: per process name that opened the GPU, and the processes that asked the Metal discovery for its plugin.
struct native_adreno_client_record {char name[20]; uint32_t opens, refused, closes, slot, peak_mib, last_refusal;};
extern struct native_adreno_client_record native_adreno_client_log[8] __attribute__((weak_import));
struct native_adreno_client_text {char name[20]; char text[64];};
extern struct native_adreno_client_text native_adreno_client_text[4] __attribute__((weak_import));
extern volatile uint32_t native_adreno_mount_fail[4] __attribute__((weak_import));
extern volatile uint32_t native_adreno_window_overflow[4][3] __attribute__((weak_import));
extern volatile uint32_t native_adreno_client_stats[5][6] __attribute__((weak_import));
struct native_adreno_asker_record {char name[20]; uint32_t reads;};
extern struct native_adreno_asker_record native_adreno_metal_askers[12] __attribute__((weak_import));
extern char native_gpu_coherence_row[120] __attribute__((weak_import));
extern size_t boot_progress_profile_rows(char *, size_t) __attribute__((weak_import));
extern size_t boot_progress_spawn_rows(char *, size_t) __attribute__((weak_import));
extern volatile uint64_t native_gpu_idle_sleeps __attribute__((weak_import));
extern volatile uint64_t native_gpu_idle_spins __attribute__((weak_import));
extern volatile uint32_t native_adreno_coherent_skips[4] __attribute__((weak_import));
extern volatile uint32_t native_adreno_io_coherent __attribute__((weak_import));
extern char native_cpufreq_row[120] __attribute__((weak_import));
extern uint32_t native_gpu_perf_vote[2] __attribute__((weak_import));
extern volatile uint32_t native_adreno_pool_status __attribute__((weak_import));
extern volatile uint32_t native_adreno_pool_wc __attribute__((weak_import));
extern volatile uint64_t native_adreno_fence_phys __attribute__((weak_import));
extern int (*native_adreno_dram_read32)(uint64_t, uint32_t *) __attribute__((weak_import));
extern int (*native_adreno_cached_read32)(uint64_t, uint32_t *) __attribute__((weak_import));
extern volatile uint32_t native_adreno_ring_log[4][4 + 112] __attribute__((weak_import));
extern volatile uint32_t native_adreno_ring_log_count __attribute__((weak_import));
extern volatile uint32_t native_adreno_ib_log[4][4 + 28] __attribute__((weak_import));
extern volatile uint32_t native_adreno_ib_log_count __attribute__((weak_import));
extern unsigned native_legacy_properties_now(char *, unsigned) __attribute__((weak_import));
extern unsigned native_legacy_clients_dump(char *, unsigned) __attribute__((weak_import));
extern unsigned native_legacy_swap_timeline(char *, unsigned) __attribute__((weak_import));
extern unsigned native_legacy_prepare_report(char *, unsigned) __attribute__((weak_import));
extern char native_adreno_metal_rows[48][96] __attribute__((weak_import));
extern unsigned native_legacy_state_words(char *, unsigned) __attribute__((weak_import));
extern volatile uint32_t native_adreno_view_diag[6] __attribute__((weak_import));
extern int boot_reset_probe(char *, unsigned) __attribute__((weak_import));
extern int boot_reset_bootloader(char *, unsigned) __attribute__((weak_import));
extern int boot_reset_mark_bootloader(char *, unsigned) __attribute__((weak_import));
extern int boot_reset_watchdog(uint32_t, char *, unsigned) __attribute__((weak_import));
extern volatile uint64_t native_adreno_import_diag[10] __attribute__((weak_import));
extern volatile uint32_t native_adreno_import_stage __attribute__((weak_import));
extern volatile uint64_t native_adreno_import_live[3 + 8 * 4] __attribute__((weak_import));
extern volatile uint32_t native_adreno_import_persistent[2] __attribute__((weak_import));
extern volatile uint64_t native_adreno_import_fail_pa __attribute__((weak_import)), native_adreno_import_fail_offset __attribute__((weak_import));
// What the reset probe and the autoreset attempt saw, for the [reset] section of the log.
static char boot_log_reset_probe_text[240], boot_log_reset_result[120], boot_log_reset_mark[64], boot_log_reset_watchdog[120];
// The kernel message buffer (bsd/sys/msgbuf.h); only these leading fields are read.
struct boot_log_msgbuf {int msg_magic, msg_size, msg_bufx, msg_bufr; char *msg_bufc;};
extern struct boot_log_msgbuf *msgbufp;

enum {LogBlock = 4096, LogMsgFirst = 1, LogMsgBlocks = 32, LogSlotFirst = 33, LogSlotBlocks = 16, LogPeriodMs = 2000, LogMaxErrors = 3,
      LogExportStage = 64};   // export bytes staged per write: 256 KiB
struct boot_log_header {
  char magic[8];                 // "SHLOG001"
  uint32_t version, header_bytes;
  uint64_t boot_id;              // CNTPCT when the writer started: tells one run from the next
  uint64_t first_lba;            // absolute 4096-byte LBA of block 0 on LUN 0
  uint32_t window_blocks, msg_first, msg_blocks, msg_bytes, slot_first, slot_blocks, slot_count, slots_written;
  uint32_t uptime_s, rounds, write_errors, gpt_open, gpt_last, state;
  char tag[32];
  // version 3: the newest complete export at block export_first of the export region (export_blocks long, ending export_below blocks
  // below block 0 of this window); export_bytes 0 = nothing exported yet
  uint32_t export_first, export_blocks, export_bytes, export_generation;
  uint32_t export_below;
  // version 4: the diagnostic file export (BootDiagExport.c) at block diag_first of the export region, diag_bytes long (0 = none)
  uint32_t diag_first, diag_bytes;
};
// Writer state for the board page: 0 off, 1 waiting for the window, 2 writing, 3 stopped after errors, 4 stopped (GPT changed).
volatile uint32_t boot_log_state, boot_log_rounds, boot_log_errors;
volatile uint64_t boot_log_first_lba;
static int boot_log_started;
static uint8_t *boot_log_buffer;   // LogMsgBlocks blocks: the message buffer, then reused for a slot and the header

// The export: boot_log_export_put takes the blob's pieces in order (any thread) and writes them through a staging buffer straight into
// the half of the export region that does not hold the last complete blob; once the last piece is on the medium that blob becomes the
// complete one, and the writer's next header names it. The lock serialises pieces and guards the counters.
static IOLock *export_lock;
static uint8_t *export_stage;
static uint32_t export_region;   // blocks of the export region, 0 = none
static uint32_t export_total, export_filled, export_generation, export_target, export_ready, export_ready_bytes, export_ready_first;
// The diagnostic file export takes the half the ready blob does not hold; from then on the plugin's exports are refused (-7), so neither
// overwrites the other. diag_status: 0 not asked, 1 running, 2 done, negative failed.
static uint32_t diag_first, diag_bytes;
static int diag_owned;
volatile int boot_diag_status;
extern volatile uint32_t boot_diag_files __attribute__((weak_import));
extern volatile uint32_t boot_diag_skipped __attribute__((weak_import));
extern volatile uint32_t boot_diag_errors __attribute__((weak_import));
extern uint64_t boot_diag_scan(uint64_t) __attribute__((weak_import));
extern int boot_diag_stream(int (*)(const void *, uint32_t, void *), void *) __attribute__((weak_import));
int boot_log_export_put(uint32_t generation, uint32_t total, uint32_t offset, const void *data, uint32_t size) {
  if (!export_lock || !generation || !total || !data || !size || offset > total || size > total - offset) return -1;
  IOLockLock(export_lock);
  int status = 0;
  const uint32_t half = export_region / 2, stage = LogExportStage * LogBlock;
  if (!half || !&native_ufs_export_write) status = -4;
  else if (total > (uint64_t)half * LogBlock) status = -5;
  else if (diag_owned) status = -7;
  else if (export_ready & 0x80000000u) status = -10;   // a generation with the top bit set (a device's RAM dump) is kept
  if (!status && !export_stage) export_stage = (uint8_t *)IOMallocAligned(stage, LogBlock);
  if (!status && !export_stage) status = -2;
  if (!status && offset == 0) {
    export_generation = generation; export_total = total; export_filled = 0;
    export_target = export_ready && export_ready_first == 0 ? half : 0;
  }
  if (!status && (generation != export_generation || total != export_total || offset != export_filled)) status = -3;   // only in order
  const uint8_t *from = (const uint8_t *)data;
  while (!status && size) {
    const uint32_t at = export_filled % stage, n = size < stage - at ? size : stage - at;
    memcpy(export_stage + at, from, n);
    export_filled += n; from += n; size -= n;
    const int full = at + n == stage, done = export_filled == export_total;
    if (full || done) {
      const uint32_t used = at + n, blocks = (used + LogBlock - 1) / LogBlock;
      if (used % LogBlock) memset(export_stage + used, 0, blocks * LogBlock - used);
      if (!native_ufs_export_write(export_target + (export_filled - used) / LogBlock, blocks, export_stage)) {
        status = -6; export_generation = 0;   // the next generation starts over
      }
    }
    if (!status && done) {export_ready = export_generation; export_ready_bytes = export_total; export_ready_first = export_target;}
  }
  IOLockUnlock(export_lock);
  return status;
}

// The diagnostic export's writer: the same staging as boot_log_export_put, into its own half.
struct diag_sink {uint8_t *stage; uint32_t target, filled;};
static int diag_flush(struct diag_sink *k) {
  const uint32_t stage = LogExportStage * LogBlock, used = k->filled % stage ? k->filled % stage : stage;
  const uint32_t blocks = (used + LogBlock - 1) / LogBlock;
  if (used % LogBlock) memset(k->stage + used, 0, blocks * LogBlock - used);
  return native_ufs_export_write(k->target + (k->filled - used) / LogBlock, blocks, k->stage) ? 0 : -6;
}
static int diag_emit(const void *data, uint32_t size, void *arg) {
  struct diag_sink *k = arg;
  const uint32_t stage = LogExportStage * LogBlock;
  const uint8_t *from = data;
  while (size) {
    const uint32_t at = k->filled % stage, n = size < stage - at ? size : stage - at;
    memcpy(k->stage + at, from, n);
    k->filled += n; from += n; size -= n;
    if (at + n == stage && diag_flush(k)) return -6;
  }
  return 0;
}
static void diag_thread(void *arg, wait_result_t wait) {
  (void)arg; (void)wait;
  IOLockLock(export_lock);
  const uint32_t half = export_region / 2;
  struct diag_sink k = {NULL, export_ready && export_ready_first == 0 ? half : 0, 0};
  diag_owned = half != 0;
  IOLockUnlock(export_lock);
  int status = -4;
  if (half && &native_ufs_export_write && (k.stage = (uint8_t *)IOMallocAligned(LogExportStage * LogBlock, LogBlock))) {
    const uint64_t stream = boot_diag_scan((uint64_t)half * LogBlock - 16);
    const uint32_t head[4] = {0, 0, (uint32_t)stream, 0};
    if (!stream) status = -8;
    else {
      status = diag_emit("SHEXR001", 8, &k);
      if (!status) status = diag_emit(&head[2], 8, &k);
      if (!status) status = boot_diag_stream(diag_emit, &k);
      if (!status && k.filled % (LogExportStage * LogBlock)) status = diag_flush(&k);
      if (!status && k.filled != stream + 16) status = -9;
    }
    IOFreeAligned(k.stage, LogExportStage * LogBlock);
  }
  IOLockLock(export_lock);
  if (!status) {diag_first = k.target; diag_bytes = k.filled;}
  IOLockUnlock(export_lock);
  boot_diag_status = status ? status : 2;
}

static uint64_t log_ticks(void) {uint64_t v; __asm__ volatile("isb\n\tmrs %0, cntpct_el0" : "=r"(v)); return v;}

// Appends a text row without its trailing blanks; returns the new length (never past cap - 1).
static size_t put(char *out, size_t at, size_t cap, const char *text, size_t max) {
  size_t n = strnlen(text, max);
  while (n && (text[n - 1] == ' ' || text[n - 1] == 0)) --n;
  for (size_t i = 0; i < n && at + 2 < cap; ++i) {char c = text[i]; out[at++] = (c >= 32 && c < 127) ? c : '?';}
  if (at + 1 < cap) out[at++] = '\n';
  return at;
}

static size_t compose(char *out, size_t cap, uint32_t slot, unsigned now) {
  size_t at = 0;
  char head[96];
  snprintf(head, sizeof head, "#slot %u t=%us tag=%s", slot, now, &boot_progress_tag ? boot_progress_tag() : "");
  at = put(out, at, cap, head, sizeof head);
  if (&boot_progress_page_text) {
    at = put(out, at, cap, "[page]", 8);
    for (unsigned i = 0; i < 48; ++i) if (boot_progress_page_text[i][0]) at = put(out, at, cap, boot_progress_page_text[i], 120);
  }
  if (&boot_progress_banner_text && &boot_progress_banner_rows) {
    unsigned rows = boot_progress_banner_rows; if (rows > 49) rows = 49;
    at = put(out, at, cap, "[banner]", 10);
    for (unsigned i = 0; i < rows; ++i) at = put(out, at, cap, boot_progress_banner_text[i], 120);
  }
  if (&boot_progress_ext) {
    at = put(out, at, cap, "[ext]", 8);
    for (unsigned i = 0; i < 8; ++i) {char row[56]; snprintf(row, sizeof row, "X%u %.47s", i, boot_progress_ext[i]); at = put(out, at, cap, row, sizeof row);}
  }
  // The Data store's counters (NativeRamBlock), a row of their own: the ext slots they once shared are taken by other kits.
  if (&native_data_persist_state && native_data_persist_state) {
    char row[400];
    snprintf(row, sizeof row, "D st=%u hdr=%u gpt=%x/%x lba=%llx pres=%llu ld=%llu wr=%llu cm=%llu er=%llu pages=%llu", native_data_persist_state,
             native_data_persist_header, native_data_persist_gpt, native_data_persist_gpt_after, (unsigned long long)native_data_persist_first_lba,
             (unsigned long long)native_data_persist_present, (unsigned long long)native_data_persist_loaded, (unsigned long long)native_data_persist_writes,
             (unsigned long long)native_data_persist_syncs, (unsigned long long)native_data_persist_errors, (unsigned long long)native_data_persist_pages);
    at = put(out, at, cap, "[data]", 8);
    at = put(out, at, cap, row, sizeof row);
  }
  // The payload's GPU addon (kern_exec.c native_gpu_addon_install): files in it (-1 none), written, already equal, failed, last error; then
  // the stage/status of the boot-time addon mounts (stage 5 = mounted and both entries visible): plugin, launchd job, wallpaper.
  if (&native_gpu_addon_files && &native_gpu_addon_stage) {
    char row[120];
    snprintf(row, sizeof row, "A files=%d wr=%d same=%d fail=%d err=%d mount=%d/%d mount2=%d/%d mount3=%d/%d", native_gpu_addon_files,
             native_gpu_addon_written, native_gpu_addon_same, native_gpu_addon_failed, native_gpu_addon_error, native_gpu_addon_stage,
             native_gpu_addon_status, native_gpu_addon2_stage, native_gpu_addon2_status, &native_gpu_addon3_stage ? native_gpu_addon3_stage : 0,
             &native_gpu_addon3_status ? native_gpu_addon3_status : 0);
    at = put(out, at, cap, row, sizeof row);
  }
  // The OS cryptex path (kern_exec.c native_cryptex_os_view; stage 5 = a WebKit extension found through /System/Cryptexes/OS) and the
  // removed MobileGestalt cache (0 removed, 2 none there).
  if (&native_cryptex_os_stage && &native_gestalt_cache_status) {
    char row[80];
    snprintf(row, sizeof row, "[cryptex os] stage=%d status=%d gestalt-cache=%d ls-store=%d", native_cryptex_os_stage, native_cryptex_os_status,
             native_gestalt_cache_status, &native_ls_store_status ? native_ls_store_status : -9);
    at = put(out, at, cap, row, sizeof row);
  }
  // The one-time copy of the OS volume's staged system apps into Data (kern_exec.c native_staged_apps_prepare): stage (2 copied, 3 copied
  // before, -1 no source), directories, files, MiB, failures and the last error.
  if (&native_staged_apps_stage) {
    char row[120];
    snprintf(row, sizeof row, "S st=%d dirs=%d files=%d mib=%d fail=%d err=%d", native_staged_apps_stage, native_staged_apps_dirs,
             native_staged_apps_files, native_staged_apps_mib, native_staged_apps_failed, native_staged_apps_error);
    at = put(out, at, cap, row, sizeof row);
    if (native_staged_apps_stage == 1 && &native_staged_apps_path) {
      snprintf(row, sizeof row, "S at %.100s", native_staged_apps_path);
      at = put(out, at, cap, row, sizeof row);
    }
  }
  // Power and volume buttons (ButtonProbe.cpp): stage (3 armed, negative the failed step), interrupts, reports, pressed mask, the power and
  // GPIO APIDs, interrupts of other APIDs cleared, PDC version, last error, spurious interrupts.
  if (&native_buttons_stats) {
    char row[120];
    snprintf(row, sizeof row, "B st=%d irq=%d rep=%d mask=%x apid=%d/%d off=%d pdc=%x err=%d spur=%d", native_buttons_stats[0], native_buttons_stats[1],
             native_buttons_stats[2], native_buttons_stats[3], native_buttons_stats[4], native_buttons_stats[5], native_buttons_stats[6],
             native_buttons_stats[7], native_buttons_stats[8], native_buttons_stats[9]);
    at = put(out, at, cap, row, sizeof row);
  }
  // Panel brightness (AppleARMBacklight.cpp): stage (3 published, negative the failed step), requests, last level, the levels read back from
  // both drivers at start, transfer errors, last failed M_IRQ status, both engines' protocol and clock configuration, last error.
  if (&native_backlight_stats) {
    char row[150];
    snprintf(row, sizeof row, "L st=%d req=%d lvl=%d a=%d b=%d xerr=%d irq=%x ea=%x eb=%x err=%d goal=%d fade=%d", native_backlight_stats[0],
             native_backlight_stats[1], native_backlight_stats[2], native_backlight_stats[3], native_backlight_stats[4], native_backlight_stats[5],
             native_backlight_stats[6], native_backlight_stats[7], native_backlight_stats[8], native_backlight_stats[9],
             &native_backlight_request ? native_backlight_request[0] : -1, &native_backlight_request ? native_backlight_request[1] : -1);
    at = put(out, at, cap, row, sizeof row);
    // The request path and the engines: request timer runs, fades begun, interrupts from a and b, interrupt mode, timeouts, transfers.
    snprintf(row, sizeof row, "L path runs=%d fades=%d irq=%d/%d mode=%d tmo=%d done=%d empty=%d last=%x", &native_backlight_request ? native_backlight_request[2] : -1,
             &native_backlight_request ? native_backlight_request[3] : -1, native_backlight_stats[10], native_backlight_stats[11],
             native_backlight_stats[12], native_backlight_stats[13], native_backlight_stats[15],
             &native_backlight_request ? native_backlight_request[4] : -1, &native_backlight_request ? (unsigned)native_backlight_request[5] : 0u);
    at = put(out, at, cap, row, sizeof row);
    // How transfers complete: interrupts from each engine, interrupt mode (1 attached, 0 polling, else the attach result), timeouts,
    // attach tries and transfers done.
    snprintf(row, sizeof row, "L2 ia=%d ib=%d mode=%d to=%d tries=%d done=%d", native_backlight_stats[10], native_backlight_stats[11],
             native_backlight_stats[12], native_backlight_stats[13], native_backlight_stats[14], native_backlight_stats[15]);
    at = put(out, at, cap, row, sizeof row);
  }
  if (&boot_progress_ext_wide)
    for (unsigned i = 0; i < 2; ++i) {char row[120]; snprintf(row, sizeof row, "W%u %.111s", i, boot_progress_ext_wide[i]); at = put(out, at, cap, row, sizeof row);}
  // Every traced process's stacks (the banner shows only what fits the panel): role 0..3 are the boot-ui-focus slots, main thread in full
  // (symbolicate with unslid = address - cache slide), then its thread groups.
  if (&boot_ui_trace_role_rows) {
    static char rows[64][120];
    at = put(out, at, cap, "[stacks]", 10);
    for (unsigned role = 0; role < 4; ++role) {
      unsigned n = boot_ui_trace_role_rows(role, rows, 64, 2);   // 2: with each thread group's deeper frames
      for (unsigned i = 0; i < n; ++i) at = put(out, at, cap, rows[i], 120);
    }
  }
  if (&native_legacy_state_words) {
    static char words[240];
    if (native_legacy_state_words(words, sizeof words)) {at = put(out, at, cap, "[display state]", 16); at = put(out, at, cap, words, sizeof words);}
  }
  if (&native_legacy_prepare_report) {
    char row[220];
    native_legacy_prepare_report(row, sizeof row);
    at = put(out, at, cap, "[scanout prepare]", 18); at = put(out, at, cap, row, sizeof row);
  }
  if (&native_legacy_swap_timeline) {
    static char timeline[4096];
    if (native_legacy_swap_timeline(timeline, sizeof timeline)) {
      at = put(out, at, cap, "[swap timeline]", 16);
      for (char *line = timeline; *line && at + 130 < cap;) {
        char *end = strchr(line, '\n');
        if (end) *end = 0;
        at = put(out, at, cap, line, 120);
        if (!end) break;
        line = end + 1;
      }
    }
  }
  if (&native_adreno_metal_rows && native_adreno_metal_rows[0][0]) {
    at = put(out, at, cap, "[metal table]", 14);
    for (unsigned r = 0; r < 48 && at + 130 < cap; ++r)
      if (native_adreno_metal_rows[r][0]) at = put(out, at, cap, (const char *)native_adreno_metal_rows[r], 96);
  }
  // The framebuffer user clients' notification slots (is the render server's vsync registered?).
  if (&native_legacy_clients_dump) {
    static char dump[4096];
    unsigned n = native_legacy_clients_dump(dump, sizeof dump);
    if (n) {
      at = put(out, at, cap, "[mfb clients]", 14);
      for (char *line = dump; *line && at + 130 < cap;) {
        char *end = strchr(line, '\n');
        if (end) *end = 0;
        at = put(out, at, cap, line, 120);
        if (!end) break;
        line = end + 1;
      }
    }
  }
  // The display object's properties now (refresh intervals, timing, scale): the lines the original driver published.
  if (&native_legacy_properties_now) {
    static char props[7168];
    unsigned n = native_legacy_properties_now(props, sizeof props);
    if (n) {
      at = put(out, at, cap, "[display]", 10);
      // The XML is one long line; 116 characters per row with a prefix, and a tag per key on its own row so refresh/timing keys stand out.
      for (unsigned i = 0; i < n && at + 130 < cap; i += 116) {
        char row[124];
        snprintf(row, sizeof row, "D%03u %.116s", i / 116, props + i);
        at = put(out, at, cap, row, sizeof row);
      }
    }
  }
  // The traced processes' threads with the highest context-switch rate (they are idle in every stack sample but woken hundreds of times a second).
  if (&boot_ui_trace_hot_rows) {
    static char hot[96][120];
    unsigned n = boot_ui_trace_hot_rows(hot, 96);
    if (n) {char head[64]; snprintf(head, sizeof head, "[hot] kernel slide=%lx", (unsigned long)vm_kernel_slide); at = put(out, at, cap, head, sizeof head);}
    for (unsigned i = 0; i < n; ++i) at = put(out, at, cap, hot[i], 120);
  }
  // The four newest ring submissions (fence, ring range, then the words the CP reads) and the first words of their IBs: all that the
  // logs keep, so a stalled fence behind newer submissions is still shown.
  if (&native_adreno_ring_log_count && native_adreno_ring_log_count) {
    const uint32_t newest = native_adreno_ring_log_count - 1;
    for (unsigned back = 0; back < 4 && back < native_adreno_ring_log_count; ++back) {
      const volatile uint32_t *r = native_adreno_ring_log[(newest - back) & 3];
      char row[130];
      snprintf(row, sizeof row, "[ring] fence=%u ring %x..%x words=%u", r[0], r[1], r[2], r[3]);
      at = put(out, at, cap, row, sizeof row);
      const uint32_t words = r[3] < 112 ? r[3] : 112;
      for (uint32_t i = 0; i < words; i += 8) {
        int n = snprintf(row, sizeof row, "R%03x", i);
        for (uint32_t k = i; k < i + 8 && k < words && n > 0 && n < 110; ++k) n += snprintf(row + n, sizeof row - n, " %08x", r[4 + k]);
        at = put(out, at, cap, row, sizeof row);
      }
    }
  }
  if (&native_adreno_ib_log_count && native_adreno_ib_log_count) {
    const uint32_t newest = native_adreno_ib_log_count - 1;
    for (unsigned back = 0; back < 4 && back < native_adreno_ib_log_count; ++back) {
      const volatile uint32_t *r = native_adreno_ib_log[(newest - back) & 3];
      char row[130];
      snprintf(row, sizeof row, "[ib] fence=%u iova=%x%08x bytes=%u", r[0], r[2], r[1], r[3]);
      at = put(out, at, cap, row, sizeof row);
      for (uint32_t i = 0; i < 28; i += 8) {
        int n = snprintf(row, sizeof row, "I%02x", i);
        for (uint32_t k = i; k < i + 8 && k < 28 && n > 0 && n < 110; ++k) n += snprintf(row + n, sizeof row - n, " %08x", r[4 + k]);
        at = put(out, at, cap, row, sizeof row);
      }
    }
  }
  // The fence page, word by word as DRAM holds it and as the CPU's cacheable alias reads it (the CP writes the timestamps with a GPU-side write).
  if (&native_adreno_fence_phys && native_adreno_fence_phys && &native_adreno_dram_read32 && native_adreno_dram_read32 && &native_adreno_cached_read32 &&
      native_adreno_cached_read32) {
    char row[130];
    int n = snprintf(row, sizeof row, "[fence page] pa=%llx dram:", (unsigned long long)native_adreno_fence_phys);
    for (unsigned w = 0; w < 8 && n > 0 && n < 110; ++w) {uint32_t v = 0xdead; native_adreno_dram_read32(native_adreno_fence_phys + 4 * w, &v); n += snprintf(row + n, sizeof row - n, " %x", v);}
    at = put(out, at, cap, row, sizeof row);
    n = snprintf(row, sizeof row, "[fence page] cached:");
    for (unsigned w = 0; w < 8 && n > 0 && n < 110; ++w) {uint32_t v = 0xdead; native_adreno_cached_read32(native_adreno_fence_phys + 4 * w, &v); n += snprintf(row + n, sizeof row - n, " %x", v);}
    at = put(out, at, cap, row, sizeof row);
  }
  // Kernel GPU methods by number: calls, failures, last status and total time (the calls of the last 2 s are the difference of two slots).
  if (&native_adreno_method_stats) {
    char row[130];
    int n = snprintf(row, sizeof row, "[gpu methods] n:calls/fail/us");
    for (unsigned m = 1; m < 20; ++m) {
      if (!native_adreno_method_stats[m][0]) continue;
      if (n > 100) {at = put(out, at, cap, row, sizeof row); n = snprintf(row, sizeof row, "[gpu methods+]");}
      n += snprintf(row + n, sizeof row - n, " %u:%u/%u/%u", m, native_adreno_method_stats[m][0], native_adreno_method_stats[m][1], native_adreno_method_stats[m][3]);
    }
    at = put(out, at, cap, row, sizeof row);
  }
  if (&native_adreno_vm_phase_us && (native_adreno_vm_phase_us[0][0] || native_adreno_vm_phase_us[1][0])) {
    char row[170];
    for (unsigned k = 0; k < 2; ++k) {
      const volatile uint64_t *p = native_adreno_vm_phase_us[k];
      if (!p[0]) continue;
      snprintf(row, sizeof row, "[vm %s] n=%llu us: observe1=%llu %s=%llu batch=%llu observe2=%llu %s=%llu max=%llu MiB=%llu", k ? "unmap" : "map",
               (unsigned long long)p[0], (unsigned long long)p[1], k ? "acquire" : "publish", (unsigned long long)p[2], (unsigned long long)p[3],
               (unsigned long long)p[4], k ? "retire" : "-", (unsigned long long)p[5], (unsigned long long)p[6], (unsigned long long)(p[7] >> 20));
      at = put(out, at, cap, row, sizeof row);
    }
  }
  if (&native_adreno_pool_stats) {
    char row[200];
    snprintf(row, sizeof row, "[bo pool] %u MiB (last reserve status %x, wc %x): %u BOs from it, %u fell back, in use %u MiB (peak %u) | scattered: %u BOs %u spans | import spans %u",
             native_adreno_pool_stats[0], native_adreno_pool_status, &native_adreno_pool_wc ? native_adreno_pool_wc : 0, native_adreno_pool_stats[1], native_adreno_pool_stats[2], native_adreno_pool_stats[3],
             native_adreno_pool_stats[4], native_adreno_pool_stats[5], native_adreno_pool_stats[6], native_adreno_pool_stats[7]);
    at = put(out, at, cap, row, sizeof row);
  }
  // GPU clients: name opens/refused/closes, live slot (- none), highest GPU address its mappings reached (MiB), last refusal.
  if (&native_adreno_client_log) {
    char row[400];
    size_t used = (size_t)snprintf(row, sizeof row, "[gpu clients]");
    for (unsigned i = 0; i < 8 && native_adreno_client_log[i].name[0] && used < sizeof row; ++i) {
      const struct native_adreno_client_record *r = &native_adreno_client_log[i];
      char slot[8];
      if (r->slot < 8) snprintf(slot, sizeof slot, "%u", r->slot); else snprintf(slot, sizeof slot, "-");
      used += (size_t)snprintf(row + used, sizeof row - used, " %.19s %u/%u/%u s%s %uM r%x", r->name, r->opens, r->refused, r->closes, slot,
                               r->peak_mib, r->last_refusal);
    }
    at = put(out, at, cap, row, sizeof row);
  }
  // [gpu windows]: per client window the mappings that fell outside it (count, last size KiB, last end MiB), the latest failed mount (stage 1 no free
  // slot, 2 core open, 3 window scope; status; slot; count), then one line per process other than the render server: why its Metal device failed.
  if (&native_adreno_window_overflow && &native_adreno_mount_fail) {
    char row[300];
    snprintf(row, sizeof row, "[gpu windows] ovf %u/%uK/%uM %u/%uK/%uM %u/%uK/%uM %u/%uK/%uM | mount fail stage %u st %x slot %u n %u",
             native_adreno_window_overflow[0][0], native_adreno_window_overflow[0][1], native_adreno_window_overflow[0][2],
             native_adreno_window_overflow[1][0], native_adreno_window_overflow[1][1], native_adreno_window_overflow[1][2],
             native_adreno_window_overflow[2][0], native_adreno_window_overflow[2][1], native_adreno_window_overflow[2][2],
             native_adreno_window_overflow[3][0], native_adreno_window_overflow[3][1], native_adreno_window_overflow[3][2],
             native_adreno_mount_fail[0], native_adreno_mount_fail[1], native_adreno_mount_fail[2], native_adreno_mount_fail[3]);
    at = put(out, at, cap, row, sizeof row);
    if (&native_adreno_client_text)
      for (unsigned i = 0; i < 4 && native_adreno_client_text[i].name[0]; ++i) {
        snprintf(row, sizeof row, "[gpu client text] %.19s: %.63s", native_adreno_client_text[i].name, native_adreno_client_text[i].text);
        at = put(out, at, cap, row, sizeof row);
      }
  }
  if (&native_adreno_metal_askers) {
    char row[400];
    size_t used = (size_t)snprintf(row, sizeof row, "[metal askers]");
    for (unsigned i = 0; i < 12 && native_adreno_metal_askers[i].name[0] && used < sizeof row; ++i)
      used += (size_t)snprintf(row + used, sizeof row - used, " %.19s:%u", native_adreno_metal_askers[i].name, native_adreno_metal_askers[i].reads);
    at = put(out, at, cap, row, sizeof row);
  }
  // The IO-coherence proof (GpuPowerProbe coherenceProbe) and the maintenance passes it saved: map, unmap, submission, completion.
  if (&native_gpu_coherence_row && native_gpu_coherence_row[0]) {
    char row[260];
    snprintf(row, sizeof row, "[coherence] %.110s | on=%u skipped map=%u unmap=%u submit=%u complete=%u | fence pauses slept=%llu spun=%llu",
             native_gpu_coherence_row,
             &native_adreno_io_coherent ? native_adreno_io_coherent : 0, &native_adreno_coherent_skips ? native_adreno_coherent_skips[0] : 0,
             &native_adreno_coherent_skips ? native_adreno_coherent_skips[1] : 0, &native_adreno_coherent_skips ? native_adreno_coherent_skips[2] : 0,
             &native_adreno_coherent_skips ? native_adreno_coherent_skips[3] : 0,
             (unsigned long long)(&native_gpu_idle_sleeps ? native_gpu_idle_sleeps : 0), (unsigned long long)(&native_gpu_idle_spins ? native_gpu_idle_spins : 0));
    at = put(out, at, cap, row, sizeof row);
  }
  // Edge contacts (drivers/ShengHidInput edge::): wrapper installed, contacts begun, of them from an edge, events skipped.
  if (&native_touch_edge_stats) {
    char row[100];
    snprintf(row, sizeof row, "[touch edge] installed=%u contacts=%u from_edge=%u skipped=%u", native_touch_edge_stats[0],
             native_touch_edge_stats[1], native_touch_edge_stats[2], native_touch_edge_stats[3]);
    at = put(out, at, cap, row, sizeof row);
  }
  // Multi-finger reports (drivers/ShengHidInput): reports with two or more fingers down, the most at once, lifts of vanished fingers.
  if (&native_touch_hid_multi) {
    char row[80];
    snprintf(row, sizeof row, "[touch multi] reports=%u max=%u lifts=%u", native_touch_hid_multi[0], native_touch_hid_multi[1],
             native_touch_hid_multi[2]);
    at = put(out, at, cap, row, sizeof row);
  }
  // IBs per GPU submission: submissions, IBs, the most in one, and those with more than 16 (a frame used to be split at 16).
  if (&native_adreno_submit_ibs) {
    char row[120];
    snprintf(row, sizeof row, "[gpu ibs] submissions=%u ibs=%u max=%u over16=%u", native_adreno_submit_ibs[0], native_adreno_submit_ibs[1],
             native_adreno_submit_ibs[2], native_adreno_submit_ibs[3]);
    at = put(out, at, cap, row, sizeof row);
  }
  // The submissions' write-combine work: audits, IB lines compared and found dirty, pages set back to write-combined, the re-assertion's
  // total time, and the CPU cache maintenance passes (count, total ms, MiB).
  if (&native_adreno_wc_audit && &native_adreno_reassert_ticks && &native_adreno_cache_stats) {
    uint64_t f = 0;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
    if (!f) f = 19200000;
    char row[200];
    snprintf(row, sizeof row, "[wc] audited=%u lines=%u dirty=%u reset=%u reassert_ms=%llu | maintenance passes=%llu ms=%llu MiB=%llu",
             native_adreno_wc_audit[0], native_adreno_wc_audit[1], native_adreno_wc_audit[2], native_adreno_wc_audit[5],
             (unsigned long long)(native_adreno_reassert_ticks * 1000 / f), (unsigned long long)native_adreno_cache_stats[3],
             (unsigned long long)(native_adreno_cache_stats[0] * 1000 / f), (unsigned long long)(native_adreno_cache_stats[1] >> 20));
    at = put(out, at, cap, row, sizeof row);
  }
  // CPU clusters and L3 as voted (CpuFreqProbe, refreshed every round) and the GPU's initial HFI perf/bandwidth vote.
  if (&native_cpufreq_row && native_cpufreq_row[0]) {
    char row[200];
    snprintf(row, sizeof row, "[clocks] %.118s | gpu perf %u bw %u", native_cpufreq_row, &native_gpu_perf_vote ? native_gpu_perf_vote[0] : 0,
             &native_gpu_perf_vote ? native_gpu_perf_vote[1] : 0);
    at = put(out, at, cap, row, sizeof row);
  }
  if (&native_adreno_export_stats && (native_adreno_export_stats[0] || native_adreno_export_stats[1])) {
    char row[120];
    snprintf(row, sizeof row, "[export] pieces=%u refused=%u generation=%u bytes=%u | complete generation=%u bytes=%u", native_adreno_export_stats[0],
             native_adreno_export_stats[1], native_adreno_export_stats[2], native_adreno_export_stats[3], export_ready, export_ready_bytes);
    at = put(out, at, cap, row, sizeof row);
  }
  // WiFi bring-up (WifiProbe.cpp): stage, GCC votes, PCIe0 GDSC, halted branches, WLAN_EN/SW_CTRL/PERST# pins, LTSSM, root port ID, link.
  if (&native_wifi_stats && native_wifi_stats[0]) {
    char row[400];
    snprintf(row, sizeof row, "[wifi] st=%d v8=%x v0=%x gdsc=%x off=%x gpio=%06x ltssm=%x id=%08x lnk=%08x phy=%x", (int)native_wifi_stats[0],
             native_wifi_stats[1], native_wifi_stats[2], native_wifi_stats[3], native_wifi_stats[4], native_wifi_stats[5], native_wifi_stats[6],
             native_wifi_stats[7], native_wifi_stats[8], native_wifi_stats[9]);
    if (native_wifi_stats[0] >= 10 || (int)native_wifi_stats[0] <= -10)
      snprintf(row + strlen(row), sizeof row - strlen(row), " pins=%06x pwr=%x pcs=%x dbg0=%x dbg1=%x ep=%08x ms=%u wlan=%08x perst=%08x clkreq=%08x rails=%x rsc=%08x tcs=%x/%x/%x/%x rsctest=%x/%x/%x", native_wifi_stats[10],
               native_wifi_stats[11], native_wifi_stats[12], native_wifi_stats[13], native_wifi_stats[14], native_wifi_stats[15], native_wifi_stats[16],
               native_wifi_stats[17], native_wifi_stats[18], native_wifi_stats[19], native_wifi_stats[20], native_wifi_stats[21], native_wifi_stats[22], native_wifi_stats[23], native_wifi_stats[24], native_wifi_stats[25],
               native_wifi_stats[26], native_wifi_stats[27], native_wifi_stats[28]);
    at = put(out, at, cap, row, sizeof row);
    if (&native_wifi_dump && native_wifi_stats[20])
      for (unsigned s = 0; s < 6; ++s) {
        char dump[200];
        int len = snprintf(dump, sizeof dump, "[wifi-tcs%u]", s);
        for (unsigned w = 0; w < 18; ++w) len += snprintf(dump + len, sizeof dump - (unsigned)len, " %x", native_wifi_dump[s * 18 + w]);
        at = put(out, at, cap, dump, sizeof dump);
      }
    if (&native_wifi_pmic && native_wifi_pmic[0]) {
      char row[260];
      snprintf(row, sizeof row, "[wifi-pmic] arb=%x apids=%u regs=%u/%u gpio3=%x before=%08x.%08x.%08x write=%x after=%08x.%08x.%08x", native_wifi_pmic[0],
               native_wifi_pmic[1], native_wifi_pmic[2] & 0xffff, native_wifi_pmic[2] >> 16, native_wifi_pmic[3], native_wifi_pmic[4], native_wifi_pmic[5], native_wifi_pmic[6],
               native_wifi_pmic[10], native_wifi_pmic[7], native_wifi_pmic[8], native_wifi_pmic[9]);
      at = put(out, at, cap, row, sizeof row);
      snprintf(row, sizeof row, "[wifi-rails] cmd-db addresses ldob15 smpg6 smpg4 smpe4 smpg5 smpg2: %x %x %x %x %x %x", native_wifi_pmic[11], native_wifi_pmic[12],
               native_wifi_pmic[13], native_wifi_pmic[14], native_wifi_pmic[15], native_wifi_pmic[16]);
      at = put(out, at, cap, row, sizeof row);
      snprintf(row, sizeof row, "[wifi-rails] enabled-by-pmic-read=%x looked-at=%x last-wait-ms=%u total-ms=%u", native_wifi_pmic[22] & 0xff, native_wifi_pmic[22] >> 8 & 0xff,
               native_wifi_pmic[22] >> 16, native_wifi_pmic[23]);
      at = put(out, at, cap, row, sizeof row);
      if (&native_wifi_ldo && (native_wifi_ldo[0] || native_wifi_ldo[12])) {
        snprintf(row, sizeof row, "[wifi-1v8] before: %08x/%08x %08x/%08x %08x/%08x %08x/%08x %08x/%08x %08x/%08x", native_wifi_ldo[0], native_wifi_ldo[1], native_wifi_ldo[2], native_wifi_ldo[3],
                 native_wifi_ldo[4], native_wifi_ldo[5], native_wifi_ldo[6], native_wifi_ldo[7], native_wifi_ldo[8], native_wifi_ldo[9], native_wifi_ldo[10], native_wifi_ldo[11]);
        at = put(out, at, cap, row, sizeof row);
        snprintf(row, sizeof row, "[wifi-1v8] after: %08x/%08x %08x/%08x %08x/%08x %08x/%08x %08x/%08x %08x/%08x", native_wifi_ldo[12], native_wifi_ldo[13], native_wifi_ldo[14], native_wifi_ldo[15],
                 native_wifi_ldo[16], native_wifi_ldo[17], native_wifi_ldo[18], native_wifi_ldo[19], native_wifi_ldo[20], native_wifi_ldo[21], native_wifi_ldo[22], native_wifi_ldo[23]);
        at = put(out, at, cap, row, sizeof row);
      }
      if (&native_wifi_cfg && (native_wifi_cfg[0] || native_wifi_cfg[8])) {
        snprintf(row, sizeof row, "[wifi-cfg] first(at %u): ep=%08x cmd=%08x bar0=%08x bar1=%08x lnk=%08x dbg0=%08x mem=%08x rcmd=%08x | end: ep=%08x cmd=%08x bar0=%08x bar1=%08x lnk=%08x dbg0=%08x mem=%08x rcmd=%08x restored=%u",
                 native_wifi_cfg[16], native_wifi_cfg[0], native_wifi_cfg[1], native_wifi_cfg[2], native_wifi_cfg[3], native_wifi_cfg[4], native_wifi_cfg[5], native_wifi_cfg[6], native_wifi_cfg[7],
                 native_wifi_cfg[8], native_wifi_cfg[9], native_wifi_cfg[10], native_wifi_cfg[11], native_wifi_cfg[12], native_wifi_cfg[13], native_wifi_cfg[14], native_wifi_cfg[15], native_wifi_cfg[17]);
        at = put(out, at, cap, row, sizeof row);
        snprintf(row, sizeof row, "[wifi-smmu] gfsr=%x gfsynr0=%x cb-fsr=%x far=%x.%x fsynr0=%x sctlr=%x tcr=%x ttbr0=%x cbar=%x", native_wifi_cfg[18], native_wifi_cfg[19], native_wifi_cfg[20], native_wifi_cfg[22], native_wifi_cfg[21], native_wifi_cfg[23], native_wifi_cfg[24], native_wifi_cfg[25], native_wifi_cfg[26], native_wifi_cfg[27]);
        at = put(out, at, cap, row, sizeof row);
        snprintf(row, sizeof row, "[wifi-img] sum32(first 512K)=%u after=%u first-word=%08x size=%u", native_wifi_cfg[28], native_wifi_cfg[30], native_wifi_cfg[29], native_wifi_cfg[31]);
        at = put(out, at, cap, row, sizeof row);
      }
      if (&native_wifi_ev) for (unsigned k = 0; k < 2; ++k) {
        volatile uint32_t *e = native_wifi_ev + 24 * k;
        if (!e[8] && !e[0] && !e[12] && !e[13] && !e[18]) continue;
        snprintf(row, sizeof row, "[wifi-ev%u] er0: %08x %08x %08x %08x | %08x %08x %08x %08x er1: %08x %08x %08x %08x ctx rp/wp: %x/%x %x/%x sink: %08x %08x bhie: addrlo=%x hi=%x size=%x db=%x status=%x", k,
                 e[0], e[1], e[2], e[3], e[4], e[5], e[6], e[7], e[8], e[9], e[10], e[11], e[12], e[13], e[14], e[15], e[16], e[17], e[18], e[19], e[20], e[22], e[23]);
        at = put(out, at, cap, row, sizeof row);
      }
      if (&native_wifi_chip && native_wifi_chip[0]) {
        snprintf(row, sizeof row, "[wifi-chip] ltssm=%x hotrst=%x cookie=%x warm=%x warm-after=%x cause=%x global-before=%x after=%x mhistatus=%x->%x ee=%x aspm-ctl=%x msi-ctl=%x",
                 native_wifi_chip[0], native_wifi_chip[1], native_wifi_chip[2], native_wifi_chip[3], native_wifi_chip[4], native_wifi_chip[5], native_wifi_chip[6], native_wifi_chip[7],
                 native_wifi_chip[8], native_wifi_chip[9], native_wifi_chip[10], native_wifi_chip[11], native_wifi_chip[12]);
        at = put(out, at, cap, row, sizeof row);
      }
      if (&native_wifi_pon && native_wifi_pon[0]) {
        snprintf(row, sizeof row, "[wifi-pon] ppid/owner=%x bytes08..27=%08x %08x %08x %08x %08x %08x %08x %08x sn7=%08x sid=%08x", native_wifi_pon[0], native_wifi_pon[1], native_wifi_pon[2],
                 native_wifi_pon[3], native_wifi_pon[4], native_wifi_pon[5], native_wifi_pon[6], native_wifi_pon[7], native_wifi_pon[8], native_wifi_pon[9], native_wifi_pon[10]);
        at = put(out, at, cap, row, sizeof row);
      }
      if (&native_wifi_ep && native_wifi_ep[0]) {
        snprintf(row, sizeof row, "[wifi-ep] st=%u bar0-mask=%08x bar0=%08x cmd=%08x rp-mem=%08x mhi: len=%x ver=%x cfg=%x bhioff=%x bhieoff=%x ctrl=%x status=%x bhi: ver=%x exec=%x st=%x err=%x",
                 native_wifi_ep[0], native_wifi_ep[1], native_wifi_ep[2], native_wifi_ep[3], native_wifi_ep[4], native_wifi_ep[5], native_wifi_ep[6], native_wifi_ep[7],
                 native_wifi_ep[8], native_wifi_ep[9], native_wifi_ep[10], native_wifi_ep[11], native_wifi_ep[12], native_wifi_ep[13], native_wifi_ep[14], native_wifi_ep[15]);
        at = put(out, at, cap, row, sizeof row);
      }
      if (&native_wifi_mhi && native_wifi_mhi[0]) {
        snprintf(row, sizeof row, "[wifi-mhi] step=%d smmu=%x part=%x amss=%u segs=%u boot=%x ee=%x state=%x bhi-status=%x bhi-err=%x dbg=%x.%x.%x mhistatus=%x ms=%u bhie=%x",
                 (int)native_wifi_mhi[0], native_wifi_mhi[1], native_wifi_mhi[2], native_wifi_mhi[3], native_wifi_mhi[4], native_wifi_mhi[5], native_wifi_mhi[6], native_wifi_mhi[7],
                 native_wifi_mhi[8], native_wifi_mhi[9], native_wifi_mhi[10], native_wifi_mhi[11], native_wifi_mhi[12], native_wifi_mhi[13], native_wifi_mhi[14], native_wifi_mhi[15]);
        at = put(out, at, cap, row, sizeof row);
        if (&native_wifi_mhi_ms) {
          snprintf(row, sizeof row, "[wifi-mhi-ms] smmu-substep=%u last-smmu-access=%x buffers=%u found=%u read=%u smmu=%u sbl=%u ctx=%u end=%u", native_wifi_mhi[12], native_wifi_mhi_ms[7], native_wifi_mhi_ms[0], native_wifi_mhi_ms[1],
                   native_wifi_mhi_ms[2], native_wifi_mhi_ms[3], native_wifi_mhi_ms[4], native_wifi_mhi_ms[5], native_wifi_mhi_ms[6]);
          at = put(out, at, cap, row, sizeof row);
        }
      }
      if (&native_wifi_text && native_wifi_text_n) {
        if (&nw_wifi_stats) {
          snprintf(row, sizeof row, "[wifi-nw] stage=%u scans=%u frames=%u beacons=%u networks=%u err=%d lines=%u", nw_wifi_stats.stage, nw_wifi_stats.scans, nw_wifi_stats.frames,
                   nw_wifi_stats.beacons, nw_wifi_stats.bss_seen, nw_wifi_stats.last_err, native_wifi_text_n);
          at = put(out, at, cap, row, sizeof row);
          if (&nw_wifi_where) {
            snprintf(row, sizeof row, "[wifi-where] kind=%llu fn=%llx depth=%llu ms=%llu", (unsigned long long)nw_wifi_where[0], (unsigned long long)nw_wifi_where[1],
                     (unsigned long long)nw_wifi_where[2], (unsigned long long)nw_wifi_where[3]);
            at = put(out, at, cap, row, sizeof row);
          }
          uint32_t f[10] = {0};
          if (&native_ufs_extra_fault && native_ufs_extra_fault(f)) {
            snprintf(row, sizeof row, "[wifi-smmu] gfsr=%08x cb-fsr=%08x far=%08x%08x fsynr0=%08x", f[0], f[2], f[4], f[3], f[5]);
            at = put(out, at, cap, row, sizeof row);
          }
        }
        // the lines added since the last slot, at most 100 per slot; the rest wait for the next one while the ring still holds them
        static uint32_t shown;
        const uint32_t n = native_wifi_text_n;
        uint32_t i = shown;
        if (n - i > 512) i = n - 512;
        for (uint32_t k = 0; i < n && k < 100; ++i, ++k) {
          char line[124];
          snprintf(line, sizeof line, "[wifi-log] %.110s", native_wifi_text[i % 512]);
          at = put(out, at, cap, line, sizeof line);
        }
        shown = i;
        // the notices among them, which the debug lines cannot push out of their own ring: at most 40 new ones per slot
        if (&native_wifi_note) {
          static uint32_t noted;
          const uint32_t m = native_wifi_note_n;
          uint32_t j = noted;
          if (m - j > 128) j = m - 128;
          for (uint32_t k = 0; j < m && k < 40; ++j, ++k) {
            char line[124];
            snprintf(line, sizeof line, "[wifi-msg] %.110s", native_wifi_note[j % 128]);
            at = put(out, at, cap, line, sizeof line);
          }
          noted = j;
        }
      }
      snprintf(row, sizeof row, "[wifi-pcie] pipe-halt=%08x perst=%08x dbg0@20ms=%x dbg1@20ms=%x pipe-mux=%x", native_wifi_pmic[17], native_wifi_pmic[18], native_wifi_pmic[19],
               native_wifi_pmic[21], native_wifi_pmic[20]);
      at = put(out, at, cap, row, sizeof row);
      // every candidate regulator peripheral that exists, before and after the votes; v is the 0x41 0x40 pair
      const unsigned nb = native_wifi_pmic[2] & 0xffff, na = native_wifi_pmic[2] >> 16;
      for (unsigned i = 0; i < nb && i < 112; ++i) {
        const uint32_t k0 = native_wifi_pmic[24 + 2 * i], v0 = native_wifi_pmic[25 + 2 * i];
        uint32_t v1 = v0;
        if (i < na && native_wifi_pmic[248 + 2 * i] == k0) v1 = native_wifi_pmic[249 + 2 * i];
        char line[120];
        snprintf(line, sizeof line, "[wifi-reg] ppid=%03x type=%x/%x before st=%02x en=%02x v=%02x%02x after st=%02x en=%02x", k0 >> 16, k0 >> 8 & 0xff, k0 & 0xff,
                 v0 >> 24, v0 >> 16 & 0xff, v0 & 0xff, v0 >> 8 & 0xff, v1 >> 24, v1 >> 16 & 0xff);
        at = put(out, at, cap, line, sizeof line);
      }
    }
  }
  // Display controller state (DisplayTimingProbe.cpp): per interface timing enable, config, counter enable, frame count then one second
  // later, line count, vsync period, status; then every pipe's size/address/stride/format and the CTLs.
  if (&native_dpu_stats && &native_dpu_pipes && native_dpu_stats[0]) {
    char row[200];
    const volatile uint32_t *d = native_dpu_stats, *p = native_dpu_pipes;
    snprintf(row, sizeof row, "[dpu] st=%d hw=%x us=%u i1 en=%x cfg=%x cnt=%x f=%u..%u ln=%u vp=%u s=%x i2 en=%x cfg=%x cnt=%x f=%u..%u ln=%u vp=%u s=%x",
             (int)d[0], d[1], d[18], d[2], d[3], d[4], d[5], d[6], d[7], d[8], d[9], d[10], d[11], d[12], d[13], d[14], d[15], d[16], d[17]);
    at = put(out, at, cap, row, sizeof row);
    for (int half = 0; half < 2; ++half) {
      int n = snprintf(row, sizeof row, "[dpu pipes%d]", half);
      for (int i = half * 5; i < half * 5 + 5 && n < (int)sizeof row; ++i)
        n += snprintf(row + n, sizeof row - n, " %x/%x/%x/%x", p[4 * i], p[4 * i + 1], p[4 * i + 2], p[4 * i + 3]);
      at = put(out, at, cap, row, sizeof row);
    }
    snprintf(row, sizeof row, "[dpu ctl] fb=%x%08x %x/%x/%x %x/%x/%x %x/%x/%x %x/%x/%x", p[41], p[40], p[42], p[43], p[44], p[45], p[46], p[47],
             p[48], p[49], p[50], p[51], p[52], p[53]);
    at = put(out, at, cap, row, sizeof row);
  }
  // Touch noise before the fuzz filter: resting or slow frames, mean step (x + y) and largest single-axis step, in 0.1 px.
  if (&native_touch_hold && native_touch_hold[7]) {
    char row[200];
    snprintf(row, sizeof row, "[touch hold] bridged=%u lifts none=%u far=%u weak=%u palm=%u taken=%u frames t2=%u t4=%u merges=%u peaks=%u full=%u ref=%u",
             native_touch_hold[0], native_touch_hold[1], native_touch_hold[2], native_touch_hold[3], native_touch_hold[4], native_touch_hold[5],
             native_touch_hold[6], native_touch_hold[7], native_touch_hold[10], native_touch_hold[11], native_touch_hold[12], native_touch_hold[13]);
    at = put(out, at, cap, row, sizeof row);
  }
  if (&native_touch_jitter && native_touch_jitter[0]) {
    char row[128];
    snprintf(row, sizeof row, "[touch jitter] frames=%u mean=%u max=%u smoothed mean=%u max=%u (0.1 px)", native_touch_jitter[0],
             native_touch_jitter[1] / native_touch_jitter[0], native_touch_jitter[2], &native_touch_hold ? native_touch_hold[8] / native_touch_jitter[0] : 0,
             &native_touch_hold ? native_touch_hold[9] : 0);
    at = put(out, at, cap, row, sizeof row);
  }
  // Distinct path lookups that failed for MobileSafari (S) and lsd (L): errno and path, in the order they first failed.
  if (&native_lookup_count && native_lookup_count) {
    char row[128];
    const unsigned n = native_lookup_count < 160 ? native_lookup_count : 160;
    snprintf(row, sizeof row, "[safari lookups] failed=%u", native_lookup_count);
    at = put(out, at, cap, row, sizeof row);
    for (unsigned i = 0; i < n; ++i) {
      snprintf(row, sizeof row, "SL%u %s", i, native_lookup_log[i]);
      at = put(out, at, cap, row, sizeof row);
    }
  }
  // Audio DSP bring-up (AdspProbe.cpp): stage and error, IPCC interrupts, the DSP's slave-kernel bits, SMEM items, AOSS mailbox, RSC id,
  // adsp.mbn KiB, ms to ready, GLINK messages in/out, channels up, opens nobody serves, last SCM result, rail votes; then its note.
  if (&native_adsp_stats && native_adsp_stats[0]) {
    char row[200];
    snprintf(row, sizeof row, "[adsp] st=%d err=%d irq=%d slave=%x items=%x qmp=%x rsc=%x fw=%dK rdy=%dms rx=%d tx=%d ch=%x unk=%d scm=%d/%x votes=%x",
             native_adsp_stats[0], native_adsp_stats[1], native_adsp_stats[2], native_adsp_stats[3], native_adsp_stats[4], native_adsp_stats[5],
             native_adsp_stats[6], native_adsp_stats[7], native_adsp_stats[8], native_adsp_stats[9], native_adsp_stats[10], native_adsp_stats[11],
             native_adsp_stats[12], native_adsp_stats[13], native_adsp_stats[14], native_adsp_stats[15]);
    at = put(out, at, cap, row, sizeof row);
    if (&native_adsp_text && native_adsp_text[0]) {
      snprintf(row, sizeof row, "[adsp] %.95s", native_adsp_text);
      at = put(out, at, cap, row, sizeof row);
    }
  }
  // IPC router link to the audio DSP and the service locator (ShengQrtr.c): stage (3 the DSP said hello), packets in, data in, bad, out,
  // refused, locator requests and answers, the DSP's node, its services, link ups/downs, unrouted data, last request name; then the
  // DSP's services as service:instance.
  if (&native_qrtr_stats && native_qrtr_stats[0]) {
    char row[200];
    snprintf(row, sizeof row, "[qrtr] st=%d in=%d data=%d bad=%d out=%d refused=%d loc=%d/%d node=%d svcs=%d up=%d down=%d unrouted=%d req=%x",
             native_qrtr_stats[0], native_qrtr_stats[1], native_qrtr_stats[2], native_qrtr_stats[3], native_qrtr_stats[4], native_qrtr_stats[5],
             native_qrtr_stats[6], native_qrtr_stats[7], native_qrtr_stats[8], native_qrtr_stats[9], native_qrtr_stats[10], native_qrtr_stats[11],
             native_qrtr_stats[12], native_qrtr_stats[13]);
    at = put(out, at, cap, row, sizeof row);
    if (&native_qrtr_text && native_qrtr_text[0]) {
      snprintf(row, sizeof row, "[qrtr] %.95s", native_qrtr_text);
      at = put(out, at, cap, row, sizeof row);
    }
  }
  // Battery power source (ShengBattery.cpp): stage, percent, mV, mA, temperature (raw), flags (present, external, charging, full,
  // notifications on), refresh cycles, answers, timeouts|errors<<16, notifications, USB type, full/design mAh, cycles, dropped|ignored<<16,
  // send refusals.
  if (&native_battery_stats && native_battery_stats[0]) {
    char row[200];
    snprintf(row, sizeof row, "[battery] st=%d pct=%d mv=%d ma=%d temp=%d fl=%x cyc=%d ans=%d to=%x ntf=%d usb=%d full=%d design=%d cycles=%d drop=%x refuse=%d",
             native_battery_stats[0], native_battery_stats[1], native_battery_stats[2], native_battery_stats[3], native_battery_stats[4],
             native_battery_stats[5], native_battery_stats[6], native_battery_stats[7], native_battery_stats[8], native_battery_stats[9],
             native_battery_stats[10], native_battery_stats[11], native_battery_stats[12], native_battery_stats[13], native_battery_stats[14],
             native_battery_stats[15]);
    at = put(out, at, cap, row, sizeof row);
    // the raw readings (ShengBattery.cpp native_battery_raw): capacity property, both fuel gauges, temperature, current, valid bits,
    // the field the last timeout skipped
    if (&native_battery_raw) {
      snprintf(row, sizeof row, "[battery-raw] cap=%d g1=%d/%d g2=%d/%d temp=%d cur=%d valid=%x lastto=%d",
               native_battery_raw[0], native_battery_raw[1], native_battery_raw[2], native_battery_raw[3], native_battery_raw[4],
               native_battery_raw[5], native_battery_raw[6], native_battery_raw[7], native_battery_raw[8]);
      at = put(out, at, cap, row, sizeof row);
    }
    // the charger (ShengBattery.cpp native_charger_stats): USB online, type, adapter type, the board's adapter type, input limit and port
    // maximum (mA), USB mV and mA, battery status, charge type, thermal level, charge current (mA), adapter watts, authentication and
    // suspend bits, limit sets|tries<<8|port<<16, last limit result
    if (&native_charger_stats) {
      snprintf(row, sizeof row, "[charger] on=%d type=%d adap=%d real=%d icl=%d max=%d mv=%d ma=%d st=%d ct=%d lvl=%d fcc=%d w=%d auth=%x lim=%x res=%d",
               native_charger_stats[0], native_charger_stats[1], native_charger_stats[2], native_charger_stats[3], native_charger_stats[4],
               native_charger_stats[5], native_charger_stats[6], native_charger_stats[7], native_charger_stats[8], native_charger_stats[9],
               native_charger_stats[10], native_charger_stats[11], native_charger_stats[12], native_charger_stats[13], native_charger_stats[14],
               native_charger_stats[15]);
      at = put(out, at, cap, row, sizeof row);
    }
  }
  // Audio framework readiness over the packet router channel (AudioProbe.cpp): stage (5 ready), queries, answers, last status, refusals,
  // timeouts, ignored packets, ms from channel up to ready, channel ups/downs, last opcode, ring drops, send refusals, mode.
  if (&native_audio_stats && native_audio_stats[0]) {
    char row[200];
    snprintf(row, sizeof row, "[audio] st=%d q=%d ans=%d status=%d refused=%d to=%d ign=%d rdy=%dms up=%d down=%d op=%x drop=%d sendfail=%d mode=%d",
             native_audio_stats[0], native_audio_stats[1], native_audio_stats[2], native_audio_stats[3], native_audio_stats[4], native_audio_stats[5],
             native_audio_stats[6], native_audio_stats[7], native_audio_stats[8], native_audio_stats[9], native_audio_stats[10], native_audio_stats[11],
             native_audio_stats[12], native_audio_stats[13]);
    at = put(out, at, cap, row, sizeof row);
  }
  // Touch strokes in panel pixels as the digitizer published them (y mirrored, 0 = the edge the UI calls top): where each finger came
  // down and lifted, how long and over how many frames, and the extent of every contact so far (edge gestures need the outer pixels).
  if (&native_touch_stroke_count && &native_touch_strokes && &native_touch_extent && native_touch_stroke_count) {
    char row[160];
    const uint32_t n = native_touch_stroke_count;
    snprintf(row, sizeof row, "[touch] strokes=%u extent x %d..%d y %d..%d (px of 3048x2032)", n, native_touch_extent[0] / 10,
             native_touch_extent[1] / 10, native_touch_extent[2] / 10, native_touch_extent[3] / 10);
    at = put(out, at, cap, row, sizeof row);
    for (uint32_t k = n > 16 ? n - 16 : 0; k < n; ++k) {
      const volatile int32_t *r = native_touch_strokes[k & 15];
      snprintf(row, sizeof row, "TS%u t=%u.%03us %ums (%d,%d)->(%d,%d) f%d", k, (uint32_t)r[0] / 1000, (uint32_t)r[0] % 1000, (uint32_t)r[1],
               r[2] / 10, r[3] / 10, r[4] / 10, r[5] / 10, r[6]);
      at = put(out, at, cap, row, sizeof row);
    }
  }
  if (&native_swipe_stats && native_swipe_stats[3]) {
    char row[200];
    snprintf(row, sizeof row, "[swipe] done=%u first=%u.%03us last=%u.%03us frames=%u tap=%u.%03us home=%u.%03us library=%u.%03us..%u.%03us", native_swipe_stats[0],
             native_swipe_stats[1] / 1000, native_swipe_stats[1] % 1000, native_swipe_stats[2] / 1000, native_swipe_stats[2] % 1000,
             native_swipe_stats[3], native_swipe_stats[4] / 1000, native_swipe_stats[4] % 1000, native_swipe_stats[5] / 1000,
             native_swipe_stats[5] % 1000, native_swipe_stats[6] / 1000, native_swipe_stats[6] % 1000, native_swipe_stats[7] / 1000,
             native_swipe_stats[7] % 1000);
    at = put(out, at, cap, row, sizeof row);
    snprintf(row, sizeof row, "[swipe2] heavy=%u.%03us controlcenter..%u.%03us notification..%u.%03us apps..%u.%03us", native_swipe_stats[16] / 1000,
             native_swipe_stats[16] % 1000, native_swipe_stats[9] / 1000, native_swipe_stats[9] % 1000, native_swipe_stats[10] / 1000,
             native_swipe_stats[10] % 1000, native_swipe_stats[11] / 1000, native_swipe_stats[11] % 1000);
    at = put(out, at, cap, row, sizeof row);
  }
  if (&native_adreno_map_fail && native_adreno_map_fail[0][1]) {
    char row[170];
    int n = snprintf(row, sizeof row, "[map fails]");
    for (unsigned k = 0; k < 5 && native_adreno_map_fail[k][1]; ++k) n += snprintf(row + n, sizeof row - n, " %x x%u", native_adreno_map_fail[k][0], native_adreno_map_fail[k][1]);
    snprintf(row + n, sizeof row - n, " | at last: spans=%u objects=%u iova_pfn=%x handle=%x", native_adreno_map_fail[5][0], native_adreno_map_fail[5][1], native_adreno_map_fail[6][0], native_adreno_map_fail[6][1]);
    at = put(out, at, cap, row, sizeof row);
  }
  if (&native_adreno_client_stats) {
    // per client slot: live objects/peak, refused BoAllocs, last refusal status and bytes, highest mapped end in the window (MiB)
    char row[200];
    int n = snprintf(row, sizeof row, "[gpu objects]");
    for (unsigned k = 0; k < 5; ++k)
      if (native_adreno_client_stats[k][1] || native_adreno_client_stats[k][2])
        n += snprintf(row + n, sizeof row - n, " s%u %u/%u r%u %x/%u w%uM", k, native_adreno_client_stats[k][0], native_adreno_client_stats[k][1],
                      native_adreno_client_stats[k][2], native_adreno_client_stats[k][3], native_adreno_client_stats[k][4], native_adreno_client_stats[k][5]);
    at = put(out, at, cap, row, sizeof row);
  }
  if (&native_adreno_fence_wait && native_adreno_fence_wait[0]) {
    char row[160];
    snprintf(row, sizeof row, "[fence wait] calls=%u timeouts=%u last timed-out fence=%u gpu wrote=%u submitted=%u completed=%u no-job=%u refused fence=%u",
             native_adreno_fence_wait[0], native_adreno_fence_wait[1], native_adreno_fence_wait[2], native_adreno_fence_wait[3],
             native_adreno_fence_wait[4], native_adreno_fence_wait[5], native_adreno_fence_wait[6], native_adreno_fence_wait[7]);
    at = put(out, at, cap, row, sizeof row);
    // The callers' waits (int-147): the polls above run under the connection lock, the pauses between them outside it.
    if (&native_adreno_wait_outer && native_adreno_wait_outer[0]) {
      snprintf(row, sizeof row, "[fence wait] outer waits=%llu us=%llu pauses outside the lock=%llu longest=%lluus",
               (unsigned long long)native_adreno_wait_outer[0], (unsigned long long)native_adreno_wait_outer[1],
               (unsigned long long)native_adreno_wait_outer[2], (unsigned long long)native_adreno_wait_outer[3]);
      at = put(out, at, cap, row, sizeof row);
    }
  }
  // The CP's registers once a fence stopped advancing: RBBM status 0-3, interrupt, CP fault, status 1, protect, ring rptr/wptr,
  // IB1 base/rem, IB2 base/rem, BV rptr, BV IB1 base/rem, BV IB2 rem, BV IB2 base.
  if (&native_adreno_unstick && native_adreno_unstick[0]) {
    char row[200];
    // [0] write-pointer retries, [1] fence of the last, [2] translation drops, [4] bus drains with ack [5]/[6], [9..11] stalls that ended after
    // the write pointer / translations / drain stage, [12] the most timeouts one stall took, [13] a wait's timeout in us.
    snprintf(row, sizeof row, "[gpu unstick] kick=%u last=%u tlb=%u drain=%u ack=%x/%x ended: kick=%u tlb=%u drain=%u longest=%u unit=%uus",
             native_adreno_unstick[0], native_adreno_unstick[1], native_adreno_unstick[2], native_adreno_unstick[4],
             native_adreno_unstick[5], native_adreno_unstick[6], native_adreno_unstick[9], native_adreno_unstick[10],
             native_adreno_unstick[11], native_adreno_unstick[12], native_adreno_unstick[13]);
    at = put(out, at, cap, row, sizeof row);
  }
  if (&native_adreno_stall && native_adreno_stall[0]) {
    char row[240];
    int n = snprintf(row, sizeof row, "[gpu stall] fence=%u n=%u", native_adreno_stall[0], native_adreno_stall[1]);
    for (unsigned k = 2; k < 24 && n > 0 && (size_t)n < sizeof row; ++k) n += snprintf(row + n, sizeof row - n, " %x", native_adreno_stall[k]);
    at = put(out, at, cap, row, sizeof row);
    // SMMU FSR, FAR lo/hi, FSYNR0, then for the BR IB1, BV IB1 and BV IB2: the dword index in the IB where the 16 words start
    // (ffff00NN: NN words before the fetch address, the IB start being unknown), then 16 words; deadbeef = not readable.
    if (&native_adreno_stall_mem)
      for (unsigned part = 0; part < 4; ++part) {
        static const char *const names[4] = {"smmu", "br ib1", "bv ib1", "bv ib2"};
        n = snprintf(row, sizeof row, "[gpu stall] %s", names[part]);
        if (part) n += snprintf(row + n, sizeof row - n, " @%x:", native_adreno_stall_mem[4 + 17 * (part - 1)]);
        const unsigned first = part ? 5 + 17 * (part - 1) : 0, count = part ? 16 : 4;
        for (unsigned k = first; k < first + count && n > 0 && (size_t)n < sizeof row; ++k) n += snprintf(row + n, sizeof row - n, " %x", native_adreno_stall_mem[k]);
        at = put(out, at, cap, row, sizeof row);
      }
    // Map handles: next at the stalled job's submission, then under the BR IB1, BV IB1, BV IB2 (>= the first: mapped after it).
    if (&native_adreno_stall_mem) {
      snprintf(row, sizeof row, "[gpu stall] handles job>=%u br=%u bv=%u bv2=%u", native_adreno_stall_mem[55], native_adreno_stall_mem[56],
               native_adreno_stall_mem[57], native_adreno_stall_mem[58]);
      at = put(out, at, cap, row, sizeof row);
    }
  }
  if (&native_gop_wait_stats && native_gop_wait_stats[0]) {
    char row[160];
    snprintf(row, sizeof row, "[scanout wait] calls=%llu timeouts=%llu asked=%llums last=%lluus min=%lluus max=%lluus mean=%lluus", native_gop_wait_stats[0],
             native_gop_wait_stats[1], native_gop_wait_stats[2], native_gop_wait_stats[3] / 1000, native_gop_wait_stats[4] / 1000,
             native_gop_wait_stats[5] / 1000, native_gop_wait_stats[1] ? native_gop_wait_stats[6] / native_gop_wait_stats[1] / 1000 : 0);
    at = put(out, at, cap, row, sizeof row);
  }
  if (&native_adreno_view_diag && native_adreno_view_diag[0]) {
    char row[120];
    snprintf(row, sizeof row, "[bo views] calls=%u new=%u cached=%u refused=%u last=%x inside=%u", native_adreno_view_diag[0], native_adreno_view_diag[1],
             native_adreno_view_diag[2], native_adreno_view_diag[3], native_adreno_view_diag[4], native_adreno_view_diag[5]);
    at = put(out, at, cap, row, sizeof row);
  }
  if (&native_adreno_import_diag && native_adreno_import_diag[0]) {
    char row[160];
    at = put(out, at, cap, "[import]", 9);
    snprintf(row, sizeof row, "calls=%llu refused=%llu last: st=%llx stage=%llu bytes=%llx addr=%llx | first: st=%llx stage=%llu bytes=%llx addr=%llx",
             native_adreno_import_diag[0], native_adreno_import_diag[1], native_adreno_import_diag[2], native_adreno_import_diag[3],
             native_adreno_import_diag[4], native_adreno_import_diag[5], native_adreno_import_diag[6], native_adreno_import_diag[7],
             native_adreno_import_diag[8], native_adreno_import_diag[9]);
    at = put(out, at, cap, row, sizeof row);
    snprintf(row, sizeof row, "stage_now=%u refused_page pa=%llx offset=%llx", native_adreno_import_stage, native_adreno_import_fail_pa,
             native_adreno_import_fail_offset);
    at = put(out, at, cap, row, sizeof row);
    if (&native_adreno_import_live) {
      snprintf(row, sizeof row, "alive=%llu free-refused=%llu last=%llx persistent=%u plain=%u", native_adreno_import_live[0],
               native_adreno_import_live[1], native_adreno_import_live[2], &native_adreno_import_persistent ? native_adreno_import_persistent[0] : 0,
               &native_adreno_import_persistent ? native_adreno_import_persistent[1] : 0);
      at = put(out, at, cap, row, sizeof row);
      for (unsigned i = 0; i < 8; i += 2) {
        const volatile uint64_t *a = &native_adreno_import_live[3 + i * 4], *b = a + 4;
        snprintf(row, sizeof row, "I p%llu %llx+%llx h%llu s%llu | p%llu %llx+%llx h%llu s%llu", a[1] >> 40, a[0], a[1] & 0xffffffffffull, a[2],
                 a[3], b[1] >> 40, b[0], b[1] & 0xffffffffffull, b[2], b[3]);
        at = put(out, at, cap, row, sizeof row);
      }
    }
  }
  if (boot_log_reset_probe_text[0]) {
    at = put(out, at, cap, "[reset]", 8);
    at = put(out, at, cap, boot_log_reset_probe_text, sizeof boot_log_reset_probe_text);
    if (boot_log_reset_mark[0]) at = put(out, at, cap, boot_log_reset_mark, sizeof boot_log_reset_mark);
    if (boot_log_reset_watchdog[0]) at = put(out, at, cap, boot_log_reset_watchdog, sizeof boot_log_reset_watchdog);
    if (boot_log_reset_result[0]) at = put(out, at, cap, boot_log_reset_result, sizeof boot_log_reset_result);
  }
  if (boot_diag_status) {
    char row[120];
    snprintf(row, sizeof row, "[diag] status=%d files=%u skipped=%u errors=%u bytes=%u first=%u", boot_diag_status, &boot_diag_files ? boot_diag_files : 0,
             &boot_diag_skipped ? boot_diag_skipped : 0, &boot_diag_errors ? boot_diag_errors : 0, diag_bytes, diag_first);
    at = put(out, at, cap, row, sizeof row);
  }
  // The signed-text fixups given as text-fixupN boot arguments: how many parsed and how many page writes they made.
  if (&cs_text_fixup_parsed && cs_text_fixup_parsed) {
    char row[64];
    snprintf(row, sizeof row, "[text fixup] parsed=%u applied=%u", cs_text_fixup_parsed, cs_text_fixup_applied);
    at = put(out, at, cap, row, sizeof row);
  }
  // Which processes were started and ended (BootProgress.c census) and the throw site of the last uncaught exception (kits BootFaultProbe.c).
  if (&boot_progress_spawn_rows) {
    static char spawn[16384];
    const size_t n = boot_progress_spawn_rows(spawn, sizeof spawn);
    if (n) {
      at = put(out, at, cap, "[spawn]", 8);
      for (char *line = spawn; *line;) {
        char *end = strchr(line, '\n');
        const size_t len = end ? (size_t)(end - line) : strlen(line);
        at = put(out, at, cap, line, len);
        if (!end) break;
        line = end + 1;
      }
    }
  }
  // The sampling profile (BootProgress.c): which processes the interrupts landed in and where, at most 6 KiB of the slot.
  if (&boot_progress_profile_rows) {
    static char profile[6144];
    const size_t n = boot_progress_profile_rows(profile, sizeof profile);
    if (n) {
      at = put(out, at, cap, "[profile]", 10);
      for (char *line = profile; *line;) {
        char *end = strchr(line, '\n');
        const size_t len = end ? (size_t)(end - line) : strlen(line);
        at = put(out, at, cap, line, len);
        if (!end) break;
        line = end + 1;
      }
    }
  }
  if (&boot_progress_panic_text && boot_progress_panic_text[0]) {
    at = put(out, at, cap, "[panic]", 8);
    at = put(out, at, cap, boot_progress_panic_text, 2048);
  }
  return at;
}

// The message buffer, oldest byte first, NUL-padded to LogMsgBlocks blocks; returns the bytes copied.
static uint32_t copy_msgbuf(uint8_t *out) {
  memset(out, 0, LogMsgBlocks * LogBlock);
  struct boot_log_msgbuf *m = msgbufp;
  if (!m || !m->msg_bufc || m->msg_size <= 0) return 0;
  int size = m->msg_size, x = m->msg_bufx;
  if (x < 0 || x >= size) x = 0;
  uint32_t n = 0;
  for (int i = 0; i < size && n < LogMsgBlocks * LogBlock; ++i) {
    char c = m->msg_bufc[(x + i) % size];
    if (c) out[n++] = (uint8_t)c;
  }
  return n;
}

static void writer(void *arg, wait_result_t result) {
  (void)arg; (void)result;
  thread_set_thread_name(current_thread(), "boot_log");
  uint64_t first = 0, blocks = 0;
  const char *why = "";
  boot_log_state = 1;
  // The window exists once NativeUfs has read the root layout; until then the open is refused ("no root layout").
  while (!native_ufs_log_open(&first, &blocks, &why)) {
    if (strcmp(why, "no root layout")) {boot_log_state = 3; return;}
    IOSleep(1000);
  }
  boot_log_first_lba = first;
  const uint32_t slot_count = (uint32_t)((blocks - LogSlotFirst) / LogSlotBlocks);
  if (&native_ufs_export_blocks && &native_ufs_export_write) {
    IOLockLock(export_lock);
    export_region = (uint32_t)native_ufs_export_blocks();
    IOLockUnlock(export_lock);
  }
  struct boot_log_header h;
  memset(&h, 0, sizeof h);
  memcpy(h.magic, "SHLOG001", 8);
  h.version = 4; h.header_bytes = sizeof h;
  h.export_blocks = export_region; h.export_below = export_region; h.boot_id = log_ticks(); h.first_lba = first; h.window_blocks = (uint32_t)blocks;
  h.msg_first = LogMsgFirst; h.msg_blocks = LogMsgBlocks; h.slot_first = LogSlotFirst; h.slot_blocks = LogSlotBlocks; h.slot_count = slot_count;
  h.gpt_open = h.gpt_last = &native_ufs_store_gpt_check ? native_ufs_store_gpt_check() : 0xff;
  if (&boot_progress_tag) strlcpy(h.tag, boot_progress_tag(), sizeof h.tag);
  uint64_t f = 0; __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f)); if (!f) f = 19200000;
  const uint64_t start = log_ticks();
  // The reboot-into-fastboot path, read only: PSCI features and the PMIC cell. sheng-autoreset=<seconds of log time> then writes the cell and
  // restarts once, after that round's log is on the medium, but only when the probe found the cell writable.
  uint32_t autoreset = 0;
  int reset_ok = 0, reset_tried = 0;
  if (&boot_reset_probe) {
    reset_ok = boot_reset_probe(boot_log_reset_probe_text, sizeof boot_log_reset_probe_text) == 0;
    PE_parse_boot_argn("sheng-autoreset", &autoreset, sizeof autoreset);
  }
  // sheng-diag-export=<seconds of log time>: export the diagnostic files once then (set it shortly before sheng-autoreset).
  uint32_t diag_at = 0;
  if (&boot_diag_scan && &boot_diag_stream) PE_parse_boot_argn("sheng-diag-export", &diag_at, sizeof diag_at);
  uint32_t errors = 0;
  boot_log_state = 2;
  for (;;) {
    const unsigned now = (unsigned)((log_ticks() - start) / f);
    int ok = 1;
    // Text slot, then the message buffer, then the header that makes both visible.
    memset(boot_log_buffer, 0, LogSlotBlocks * LogBlock);
    compose((char *)boot_log_buffer, LogSlotBlocks * LogBlock, h.slots_written, now);
    ok &= native_ufs_log_write(LogSlotFirst + (uint64_t)(h.slots_written % slot_count) * LogSlotBlocks, LogSlotBlocks, boot_log_buffer);
    h.msg_bytes = copy_msgbuf(boot_log_buffer);
    ok &= native_ufs_log_write(LogMsgFirst, LogMsgBlocks, boot_log_buffer);
    if (ok) ++h.slots_written;
    else ++h.write_errors;
    // The newest complete export is already on the medium; the header names it.
    IOLockLock(export_lock);
    h.export_generation = export_ready; h.export_bytes = export_ready_bytes; h.export_first = export_ready_first;
    h.diag_first = diag_first; h.diag_bytes = diag_bytes;
    IOLockUnlock(export_lock);
    if (diag_at && now >= diag_at && !boot_diag_status) {
      thread_t diag;
      boot_diag_status = 1;
      if (kernel_thread_start_priority(diag_thread, NULL, MINPRI_KERNEL, &diag) == KERN_SUCCESS) thread_deallocate(diag);
      else boot_diag_status = -10;
    }
    h.uptime_s = now; h.rounds = ++boot_log_rounds; h.state = boot_log_state;
    // Every 30 rounds the GPT is read back; any change stops the writer for good.
    if (&native_ufs_store_gpt_check && !(h.rounds % 30)) h.gpt_last = native_ufs_store_gpt_check();
    memset(boot_log_buffer, 0, LogBlock);
    memcpy(boot_log_buffer, &h, sizeof h);
    ok &= native_ufs_log_write(0, 1, boot_log_buffer);
    ok &= native_ufs_log_sync();
    if (!ok) {boot_log_errors = ++errors;} else errors = 0;
    if (errors >= LogMaxErrors) {boot_log_state = 3; return;}
    if (h.gpt_last != h.gpt_open) {boot_log_state = 4; return;}
    if (autoreset && now >= autoreset && !reset_tried && ok) {
      reset_tried = 1;
      if (!reset_ok) strlcpy(boot_log_reset_result, "autoreset skipped: the probe found no writable cell", sizeof boot_log_reset_result);
      else {
        // The text is only readable if the reset is refused; on success the tablet restarts into fastboot.
        int rc = boot_reset_bootloader(boot_log_reset_result, sizeof boot_log_reset_result);
        char tail[24]; snprintf(tail, sizeof tail, " (returned %d)", rc);
        strlcat(boot_log_reset_result, tail, sizeof boot_log_reset_result);
      }
    }
    IOSleep(LogPeriodMs);
  }
}

// Called by the board page's monitor once a second; starts the writer the first time when sheng-log=1.
void boot_log_tick(unsigned now) {
  (void)now;
  if (boot_log_started) return;
  boot_log_started = 1;
  // An unattended run must come back however it ends: on the desktop past its deadline (int-229 first suspected) or in a panic seconds
  // after the start, before UFS and this log exist (int-229: a duplicate class at PE_lockdown_iokit). So the first tick, independent of
  // sheng-log, marks the cell (any restart is fastboot) and arms the hypervisor's watchdog, which bites a minute after the pings stop:
  // after the autoreset deadline, or when a panic parks the cores.
  uint32_t autoreset = 0;
  if (PE_parse_boot_argn("sheng-autoreset", &autoreset, sizeof autoreset) && autoreset && &boot_reset_probe &&
      boot_reset_probe(boot_log_reset_probe_text, sizeof boot_log_reset_probe_text) == 0) {
    if (&boot_reset_mark_bootloader) boot_reset_mark_bootloader(boot_log_reset_mark, sizeof boot_log_reset_mark);
    if (&boot_reset_watchdog) boot_reset_watchdog(autoreset + 60, boot_log_reset_watchdog, sizeof boot_log_reset_watchdog);
  }
  uint32_t mode = 0;
  if (!PE_parse_boot_argn("sheng-log", &mode, sizeof mode) || mode != 1) return;
  if (!&native_ufs_log_open || !&native_ufs_log_write || !&native_ufs_log_sync) return;
  boot_log_buffer = (uint8_t *)IOMallocAligned(LogMsgBlocks * LogBlock, LogBlock);
  if (!boot_log_buffer) return;
  export_lock = IOLockAlloc();
  thread_t thread;
  if (kernel_thread_start_priority(writer, 0, BASEPRI_KERNEL, &thread) == KERN_SUCCESS) thread_deallocate(thread);
}
