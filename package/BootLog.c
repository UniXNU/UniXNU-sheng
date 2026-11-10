// SPDX-License-Identifier: MIT
// Diagnostic log on UFS (boot argument log=1): every 2 s the text the board page would show (the diagnostic page, the live
// banner, the GPU rows, a caught panic message) and the kernel message buffer are written to the last 32 MiB of userdata through
// NativeUfs's bounded log window, so a run can be read back afterwards (scripts/read-sheng-log.py over the Linux log reader).
//
// Window layout (4096-byte blocks): 0 header, 1..32 the kernel message buffer, 33.. text slots of 16 blocks in a ring, then the
// export: the newest complete blob that user space handed over (boot_log_export_put: the Metal plugin's pipeline caches, so the next
// image can start with them) lies in NativeUfs's export region, the 256 MiB below the window, which the reader fetches on its own. Its
// two halves alternate, so the header always names a blob that is wholly on the medium.
// A round writes only what changed: the blocks of a slot its text uses (the rest of the slot keeps a NUL-terminated older text that
// the reader ignores) and the message buffer blocks that new kernel messages reached. Version 5 keeps the message area as a ring of
// the message stream (msg_mode 1): msg_head bytes were appended in all, the newest min(msg_head, area) of them end at msg_head modulo
// the area. msg_mode 0 is the linear form (oldest first, msg_bytes long) that the panic flush and diag=1 write.
// The header is written last in each round, so a reader never trusts a slot or an export the header does not count yet.
#include "BootLatencyFiles.h"
#include <kern/thread.h>
#include <kern/clock.h>
#include <string.h>
#include <pexpert/pexpert.h>
#include <kern/misc_protos.h>
#include <IOKit/IOLib.h>
extern void native_wifi_logf(int, const char *, ...) __attribute__((weak_import));
extern void boot_cache_loader_log_capture(unsigned) __attribute__((weak_import));
extern uint32_t boot_cache_input_ready(void) __attribute__((weak_import));

#if defined(NATIVE_BOOT_PROCESS_CAPTURE) && NATIVE_BOOT_PROCESS_CAPTURE
extern void boot_process_start(void);
#endif

extern int native_ufs_log_open(uint64_t *, uint64_t *, const char **) __attribute__((weak_import));
extern int native_ufs_log_write(uint64_t, uint32_t, const void *) __attribute__((weak_import));
extern uint64_t native_ufs_export_blocks(void) __attribute__((weak_import));
extern int native_ufs_export_write(uint64_t, uint32_t, const void *) __attribute__((weak_import));
extern int native_ufs_log_sync(void) __attribute__((weak_import));
// The display driver's last refused swap (LegacyDisplayProbe.cpp): counts, reason, layer state/options, request size, surface slots and the surface.
extern volatile uint32_t native_legacy_swap_accepted_count __attribute__((weak_import));
extern volatile uint32_t native_legacy_swap_rejected_count __attribute__((weak_import));
extern volatile uint32_t native_legacy_rej_reason __attribute__((weak_import));
extern volatile uint64_t native_legacy_rej_surfaces[4] __attribute__((weak_import));
extern volatile uint64_t native_legacy_rej_surface[7] __attribute__((weak_import));
extern volatile uint32_t native_legacy_rej_state[3] __attribute__((weak_import));
extern volatile uint32_t native_legacy_rej_options[3] __attribute__((weak_import));
extern volatile uint32_t native_legacy_rej_size[2] __attribute__((weak_import));
extern volatile uint32_t native_legacy_rej_rects[2][4] __attribute__((weak_import));
extern char native_legacy_rej_class[32] __attribute__((weak_import));
// Signals to started cores whose SIGPdisabled had never been cleared (xnu cpu_common.c sheng_sigp_recover).
extern volatile uint32_t sheng_sigp_recovered __attribute__((weak_import));
extern volatile uint32_t sheng_idle_sigp_cleared __attribute__((weak_import)), sheng_idle_sigp_refused __attribute__((weak_import)),
  sheng_idle_enter[8] __attribute__((weak_import)), sheng_idle_disallowed[8] __attribute__((weak_import));
extern volatile uint32_t native_hid_input_events[8] __attribute__((weak_import));
extern volatile int native_hid_input_results[8] __attribute__((weak_import));
extern char native_hid_input_registry[2048] __attribute__((weak_import));
extern char native_hid_input_props[1024] __attribute__((weak_import));
extern int native_ufs_log_panic(void) __attribute__((weak_import));
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
extern volatile int native_gpu_addon4_stage __attribute__((weak_import));
extern volatile int native_gpu_addon4_status __attribute__((weak_import));
extern volatile int native_gpu_addon5_stage __attribute__((weak_import));
extern volatile int native_staged_union_stage __attribute__((weak_import));
extern volatile int native_staged_union_status __attribute__((weak_import));
extern volatile int native_gpu_addon5_status __attribute__((weak_import));
extern volatile int native_gpu_addon3_status __attribute__((weak_import));
extern volatile int native_cryptex_os_stage __attribute__((weak_import));
extern volatile int native_cryptex_os_status __attribute__((weak_import));
extern volatile int native_gestalt_cache_status __attribute__((weak_import));
extern volatile int native_ls_store_status __attribute__((weak_import));
extern volatile uint64_t native_data_persist_present __attribute__((weak_import));
// Data disk I/O (RamBlock NativeRamBlock.cpp) and the store window's commands (ShengUfs NativeUfs.cpp), in mach_absolute_time ticks.
extern volatile uint64_t native_data_io_reads __attribute__((weak_import)), native_data_io_read_bytes __attribute__((weak_import)),
    native_data_io_read_ticks __attribute__((weak_import)), native_data_io_writes __attribute__((weak_import)),
    native_data_io_write_bytes __attribute__((weak_import)), native_data_io_write_ticks __attribute__((weak_import)),
    native_data_io_gate_ticks __attribute__((weak_import)), native_data_io_max_ticks __attribute__((weak_import)),
    native_data_flushes __attribute__((weak_import)), native_data_flush_ticks __attribute__((weak_import)),
    native_data_flush_blocks __attribute__((weak_import)), native_data_timer_flushes __attribute__((weak_import));
extern volatile uint32_t native_data_write_back __attribute__((weak_import));
extern volatile uint64_t native_ufs_store_rd_ops __attribute__((weak_import)), native_ufs_store_rd_ticks __attribute__((weak_import)),
    native_ufs_store_wr_ops __attribute__((weak_import)), native_ufs_store_wr_ticks __attribute__((weak_import)),
    native_ufs_store_sync_ops __attribute__((weak_import)), native_ufs_store_sync_ticks __attribute__((weak_import)),
    native_ufs_store_max_ticks __attribute__((weak_import));
// Command slots (ShengUfs RootCommands.h) and the root windows' reads: their count, transport time and integrity check time (CNTPCT ticks).
extern void native_ufs_slot_stats(uint32_t *, uint32_t *, uint64_t *) __attribute__((weak_import));
extern volatile uint64_t native_ufs_block_reads __attribute__((weak_import)), native_ufs_read_ticks __attribute__((weak_import)),
    native_ufs_verify_ticks __attribute__((weak_import));
extern volatile int native_staged_apps_stage __attribute__((weak_import));
extern volatile int32_t native_buttons_stats[10] __attribute__((weak_import));
extern volatile int32_t native_keyboard_stats[17] __attribute__((weak_import));
extern volatile int32_t native_keyboard_accel[4] __attribute__((weak_import));
extern volatile int32_t native_keyboard_hid_stats[6] __attribute__((weak_import));
extern volatile int32_t native_keyboard_touch_range[4] __attribute__((weak_import));
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
extern volatile uint32_t native_adreno_fence_pending[8] __attribute__((weak_import));
extern volatile uint32_t native_adreno_wptr_fence[4] __attribute__((weak_import));
extern volatile uint64_t native_adreno_wait_outer[5] __attribute__((weak_import));
extern volatile uint32_t native_adreno_export_stats[4] __attribute__((weak_import));
extern volatile int32_t native_touch_strokes[16][7] __attribute__((weak_import));
extern volatile uint32_t native_touch_stroke_count __attribute__((weak_import));
extern volatile uint32_t native_wifi_stats[29] __attribute__((weak_import));
extern volatile uint64_t native_bluetooth_stats[32] __attribute__((weak_import));
extern uint8_t native_bluetooth_boot_rx[512] __attribute__((weak_import));
extern volatile uint32_t native_bluetooth_boot_rx_length __attribute__((weak_import));
extern uint8_t native_bluetooth_boot_tail[256] __attribute__((weak_import));
extern volatile uint32_t native_bluetooth_boot_tail_length __attribute__((weak_import));
extern volatile uint32_t native_bluetooth_boot_state[12] __attribute__((weak_import));
extern volatile uint32_t native_bluetooth_processes[4] __attribute__((weak_import));
extern char *volatile native_bluetooth_boot_text __attribute__((weak_import));
extern volatile uint32_t native_bluetooth_boot_text_length __attribute__((weak_import));
extern volatile uint32_t qbt_transport_activity[4] __attribute__((weak_import));
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
extern volatile uint32_t native_dpu_more[40] __attribute__((weak_import));
extern volatile uint32_t native_ufs_dpu_smmu[24] __attribute__((weak_import));
extern volatile uint32_t native_legacy_surfaces[14] __attribute__((weak_import));
extern volatile uint32_t native_legacy_swapstat[16] __attribute__((weak_import));
extern volatile uint32_t native_adreno_tl_count __attribute__((weak_import));
extern volatile uint32_t native_adreno_stall_log_count __attribute__((weak_import));
extern volatile uint32_t native_gpu_dcvs_log_count __attribute__((weak_import));
extern volatile uint32_t native_gop_wait_reasons[4] __attribute__((weak_import));
extern volatile uint32_t native_legacy_direct[13] __attribute__((weak_import));
extern volatile uint32_t native_ufs_dpu_state __attribute__((weak_import));
extern volatile uint32_t native_ufs_dpu_check[8] __attribute__((weak_import));
extern uint32_t native_ufs_dpu_live(unsigned reg) __attribute__((weak_import));
extern volatile int32_t native_adsp_stats[16] __attribute__((weak_import));
extern volatile int32_t native_audio_stats[16] __attribute__((weak_import));
extern volatile uint32_t native_audio_amps[6][4] __attribute__((weak_import));
extern volatile int32_t native_adsp_audio[8] __attribute__((weak_import));
extern volatile int32_t native_mic_stats[16] __attribute__((weak_import));
extern volatile int32_t native_audio_service[8] __attribute__((weak_import));
extern volatile uint64_t native_audio_clock[4] __attribute__((weak_import));
extern volatile int32_t native_audio_sync[5] __attribute__((weak_import));
extern volatile uint64_t native_audio_requests[17] __attribute__((weak_import));
extern volatile int32_t native_audio_passed __attribute__((weak_import));
extern volatile uint32_t native_mic_regs[16] __attribute__((weak_import));
extern volatile int32_t native_mic_capture[8] __attribute__((weak_import));
#include "ShengAudioAbi.h"
extern const ShengAudioDiag *sheng_audio_diag(void) __attribute__((weak_import));
extern volatile uint64_t native_audio_pa __attribute__((weak_import));
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
extern volatile uint64_t native_adreno_vm_kind_us[5][3] __attribute__((weak_import));
extern volatile uint64_t native_adreno_maint_phase[8][2] __attribute__((weak_import));
extern volatile uint32_t native_adreno_table_pages[4] __attribute__((weak_import));
extern unsigned NativeAdrenoGpuContextInputBits(void) __attribute__((weak_import));
extern volatile uint32_t native_adreno_recovery[8] __attribute__((weak_import));
extern volatile uint32_t native_adreno_recovery_fault[5] __attribute__((weak_import));
extern volatile uint32_t native_adreno_recover_trace[4] __attribute__((weak_import));
extern volatile uint64_t native_adreno_fault_place[4] __attribute__((weak_import));
extern char native_adreno_fault_owner[20] __attribute__((weak_import));
extern uint32_t native_gpu_restart[4] __attribute__((weak_import));
extern volatile uint32_t native_adreno_pool_stats_uncached[2] __attribute__((weak_import));
extern volatile uint32_t native_adreno_bo_memory[8] __attribute__((weak_import));
extern volatile uint32_t native_adreno_freed_outside __attribute__((weak_import));
extern volatile uint32_t native_adreno_bo_contig[2] __attribute__((weak_import));
extern volatile uint32_t native_adreno_map_fail[7][2] __attribute__((weak_import));
extern volatile uint32_t native_adreno_wc_audit[6] __attribute__((weak_import));
extern volatile uint32_t native_adreno_submit_ibs[4] __attribute__((weak_import));
extern volatile uint32_t native_touch_edge_stats[4] __attribute__((weak_import));
extern volatile uint32_t native_touch_edge_install[3] __attribute__((weak_import));
extern volatile uint32_t native_touch_hid_multi[3] __attribute__((weak_import));
extern volatile int32_t native_backlight_request[6] __attribute__((weak_import));
extern volatile uint32_t native_usb_stats[54] __attribute__((weak_import));
extern volatile uint64_t native_adreno_reassert_ticks __attribute__((weak_import));
extern volatile uint64_t native_adreno_cache_stats[5] __attribute__((weak_import));
// NativeAdrenoPublish.cpp: per process name that opened the GPU, and the processes that asked the Metal discovery for its plugin.
struct native_adreno_client_record {char name[20]; uint32_t opens, refused, closes, slot, peak_mib, last_refusal;};
extern struct native_adreno_client_record native_adreno_client_log[8] __attribute__((weak_import));
struct native_adreno_client_text {char name[20]; char text[64];};
extern struct native_adreno_client_text native_adreno_client_text[8] __attribute__((weak_import));
extern volatile uint32_t native_adreno_mount_fail[4] __attribute__((weak_import));
extern volatile uint32_t native_adreno_table_switches[8] __attribute__((weak_import));
struct sheng_exc_record {char name[20]; uint32_t pid, type, esr, frames; uint64_t pc, lr, far, ret[12];};
extern struct sheng_exc_record sheng_exc_ring[4] __attribute__((weak_import));
extern volatile uint32_t sheng_exc_count __attribute__((weak_import));
extern volatile uint32_t native_adreno_cp_aperture[4] __attribute__((weak_import));
extern volatile uint32_t native_adreno_cp_image[22] __attribute__((weak_import));
extern bool native_adreno_preempt_stats(uint32_t *out) __attribute__((weak_import));
extern volatile uint32_t native_gpu_preempt[8] __attribute__((weak_import));
extern volatile uint32_t native_adreno_switch_count __attribute__((weak_import));
extern volatile uint32_t native_adreno_window_overflow[4][3] __attribute__((weak_import));
extern volatile uint32_t native_adreno_client_stats[5][6] __attribute__((weak_import));
extern volatile uint32_t native_adreno_client_bytes[5][2] __attribute__((weak_import));
struct native_adreno_asker_record {char name[20]; uint32_t reads;};
extern struct native_adreno_asker_record native_adreno_metal_askers[12] __attribute__((weak_import));
extern char native_gpu_coherence_row[120] __attribute__((weak_import));
extern char native_gpu_walk_row[80] __attribute__((weak_import));
extern volatile uint32_t native_adreno_walk_coherent[3] __attribute__((weak_import));
extern size_t boot_progress_profile_rows(char *, size_t) __attribute__((weak_import));
extern size_t boot_progress_spawn_rows(char *, size_t) __attribute__((weak_import));
extern volatile uint64_t native_gpu_idle_sleeps __attribute__((weak_import));
extern volatile uint64_t native_gpu_idle_timeouts __attribute__((weak_import));
extern volatile uint64_t native_gpu_idle_spins __attribute__((weak_import));
extern volatile uint32_t native_adreno_coherent_skips[4] __attribute__((weak_import));
extern volatile uint32_t native_adreno_io_coherent __attribute__((weak_import));
extern char native_cpufreq_row[120] __attribute__((weak_import));
extern uint32_t native_gpu_perf_vote[2] __attribute__((weak_import));
extern volatile uint32_t native_gpu_dcvs[15] __attribute__((weak_import));
extern volatile uint32_t native_cpufreq_gov[9] __attribute__((weak_import));
extern volatile uint32_t native_cpufreq_boosts __attribute__((weak_import));
extern volatile uint32_t native_cpufreq_frame[2] __attribute__((weak_import));
extern volatile uint32_t native_cpufreq_throttle[9] __attribute__((weak_import));
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
  // version 5: the message area's form (0 linear, 1 stream ring), the times the stream restarted from a fresh copy (the kernel ring
  // moved or was replaced), and the stream bytes appended in all (low and high word)
  uint32_t msg_mode, msg_resyncs, msg_head_lo, msg_head_hi;
};
// Writer state for the board page: 0 off, 1 waiting for the window, 2 writing, 3 stopped after errors, 4 stopped (GPT changed);
// 5 in a header written by boot_log_panic_flush.
volatile uint32_t boot_log_state, boot_log_rounds, boot_log_errors;
// Why the log window could not be opened, when the writer gave up on it (state 3 with no rounds).
const char *volatile boot_log_open_refusal;
volatile uint64_t boot_log_first_lba;
static int boot_log_started;
static uint8_t *boot_log_buffer;   // LogMsgBlocks blocks: the message buffer, then reused for a slot and the header
static uint8_t *boot_log_panic_buffer;   // LogMsgBlocks blocks for boot_log_panic_flush, which must not touch the writer's buffer
static uint8_t *boot_log_msg_ring;   // LogMsgBlocks blocks: the medium's message area as the writer last left it (stream ring)
static struct boot_log_header *volatile boot_log_live;   // the writer's header once its window is open
static volatile int boot_log_panicked;   // set by boot_log_panic_flush: the writer no longer writes

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
extern volatile uint32_t boot_diag_alloc __attribute__((weak_import));
extern volatile uint32_t boot_diag_skipped __attribute__((weak_import));
extern volatile uint32_t boot_diag_errors __attribute__((weak_import));
extern uint64_t boot_diag_scan(uint64_t) __attribute__((weak_import));
extern int boot_diag_stream(int (*)(const void *, uint32_t, void *), void *) __attribute__((weak_import));
extern void boot_diag_shot_tick(unsigned) __attribute__((weak_import));
// What the autoreset did, for the export's kernel.rings.txt: armed with its seconds, the probe result, the round it fired, the rounds it was held
// back because a log write failed.
char boot_log_autoreset_text[200] = "not started";
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
// The kernel's sampling profile (sleh_irq on iPadOS 27): one packed word per interrupt, pid << 48 | kernel << 47 | pc.
extern uint64_t sheng_prof_ring[1024] __attribute__((weak_import));
extern uint32_t sheng_prof_head __attribute__((weak_import));
extern uint32_t sheng_prof_cpu[8] __attribute__((weak_import));
extern void proc_name(int pid, char *buf, int size);
// Run-time blocks: the kernel's __DATA has no room for these (prelink: native __DATA overlaps a retained segment).
static uint64_t *prof_snap, *prof_rank;
static char *compose_spawn;
static void prof_sort(uint64_t *v, size_t n) {
  for (size_t start = n / 2; start-- > 0;)
    for (size_t root = start;;) {size_t c = 2 * root + 1; if (c >= n) break; if (c + 1 < n && v[c + 1] > v[c]) ++c; if (v[root] >= v[c]) break;
      uint64_t t = v[root]; v[root] = v[c]; v[c] = t; root = c;}
  for (size_t end = n; end > 1;) {--end; uint64_t t = v[0]; v[0] = v[end]; v[end] = t;
    for (size_t root = 0;;) {size_t c = 2 * root + 1; if (c >= end) break; if (c + 1 < end && v[c + 1] > v[c]) ++c; if (v[root] >= v[c]) break;
      t = v[root]; v[root] = v[c]; v[c] = t; root = c;}}
}

static uint32_t log_gpu_on, log_display_on;
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
    // Disk requests (count/MiB/ms, gate wait, longest in us), synchronize (count/ms/bitmap blocks, timer-started), store commands.
    if (&native_data_io_reads && &native_data_write_back) {
      uint64_t f = 0; __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f)); if (!f) f = 19200000;
#define BL_MS(t) ((unsigned long long)((t) * 1000ULL / f))
#define BL_US(t) ((unsigned long long)((t) * 1000000ULL / f))
      char io[560];
      int n = snprintf(io, sizeof io, "DIO wb=%u rd=%llu/%lluM/%llums wr=%llu/%lluM/%llums gate=%llums max=%lluus fl=%llu/%llums/%llu tf=%llu",
               native_data_write_back, (unsigned long long)native_data_io_reads, (unsigned long long)(native_data_io_read_bytes >> 20),
               BL_MS(native_data_io_read_ticks), (unsigned long long)native_data_io_writes, (unsigned long long)(native_data_io_write_bytes >> 20),
               BL_MS(native_data_io_write_ticks), BL_MS(native_data_io_gate_ticks), BL_US(native_data_io_max_ticks),
               (unsigned long long)native_data_flushes, BL_MS(native_data_flush_ticks), (unsigned long long)native_data_flush_blocks,
               (unsigned long long)native_data_timer_flushes);
      if (&native_ufs_store_rd_ops && n > 0 && (size_t)n < sizeof io)
        snprintf(io + n, sizeof io - (size_t)n, " ufs r=%llu/%llums w=%llu/%llums s=%llu/%llums umax=%lluus",
                 (unsigned long long)native_ufs_store_rd_ops, BL_MS(native_ufs_store_rd_ticks), (unsigned long long)native_ufs_store_wr_ops,
                 BL_MS(native_ufs_store_wr_ticks), (unsigned long long)native_ufs_store_sync_ops, BL_MS(native_ufs_store_sync_ticks),
                 BL_US(native_ufs_store_max_ticks));
      n = (int)strnlen(io, sizeof io);
      if (&native_ufs_slot_stats && (size_t)n < sizeof io) {
        uint32_t slots = 0, peak = 0; uint64_t waits = 0;
        native_ufs_slot_stats(&slots, &peak, &waits);
        n += snprintf(io + n, sizeof io - (size_t)n, " q=%u/%u/%llu", slots, peak, (unsigned long long)waits);
      }
      if (&native_ufs_block_reads && &native_ufs_read_ticks && &native_ufs_verify_ticks && n > 0 && (size_t)n < sizeof io)
        snprintf(io + n, sizeof io - (size_t)n, " root=%llu/%llums v=%llums", (unsigned long long)native_ufs_block_reads,
                 BL_MS(native_ufs_read_ticks), BL_MS(native_ufs_verify_ticks));
#undef BL_MS
#undef BL_US
      at = put(out, at, cap, io, sizeof io);
    }
  }
  // The payload's GPU addon (kern_exec.c native_gpu_addon_install): files in it (-1 none), written, already equal, failed, last error; then
  // the stage/status of the boot-time addon mounts (stage 5 = mounted and both entries visible): plugin, launchd job, wallpaper, audio, wallpaper floating layer
  // server plug-in.
  if (&native_gpu_addon_files && &native_gpu_addon_stage) {
    char row[180];
    snprintf(row, sizeof row, "A files=%d wr=%d same=%d fail=%d err=%d mount=%d/%d mount2=%d/%d mount3=%d/%d mount4=%d/%d mount5=%d/%d",
             native_gpu_addon_files,
             native_gpu_addon_written, native_gpu_addon_same, native_gpu_addon_failed, native_gpu_addon_error, native_gpu_addon_stage,
             native_gpu_addon_status, native_gpu_addon2_stage, native_gpu_addon2_status, &native_gpu_addon3_stage ? native_gpu_addon3_stage : 0,
             &native_gpu_addon3_status ? native_gpu_addon3_status : 0, &native_gpu_addon4_stage ? native_gpu_addon4_stage : 0,
             &native_gpu_addon4_status ? native_gpu_addon4_status : 0, &native_gpu_addon5_stage ? native_gpu_addon5_stage : 0,
             &native_gpu_addon5_status ? native_gpu_addon5_status : 0);
    at = put(out, at, cap, row, sizeof row);
    // the staged system apps' union view over /Applications (owner-staged-apps=1): stage 5 = mounted, both entries visible
    snprintf(row, sizeof row, "[staged apps union] stage %d status %d", &native_staged_union_stage ? native_staged_union_stage : 0,
             &native_staged_union_status ? native_staged_union_status : 0);
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
  // Keyboard cover (KeyboardProbe.cpp): stage (7 serving, negative the failed step) and its detail, engine protocol and clock, boot ROM
  // states, image load ms, controller interrupts, frames, keyboard/consumer/touchpad reports, attach state, hall state, input enabled,
  // I2C failures and the last error, engine interrupts, commands, authentication requests; then the HID device (KeyboardCover.cpp):
  // starts, reports, failed reports, outputs, feature reads, last failure; the cover's accelerometer; the touchpad's raw coordinate range.
  if (&native_keyboard_stats) {
    char row[240];
    int n = snprintf(row, sizeof row, "K st=%d err=%x se=%x rom=%x fw=%dms irq=%d fr=%d rep=%x att=%x hall=%x en=%d i2c=%d/%x bus=%d cmd=%d auth=%x palm=%d",
                     native_keyboard_stats[0], native_keyboard_stats[1], native_keyboard_stats[2], native_keyboard_stats[3], native_keyboard_stats[4],
                     native_keyboard_stats[5], native_keyboard_stats[6], native_keyboard_stats[7], native_keyboard_stats[8], native_keyboard_stats[9],
                     native_keyboard_stats[10], native_keyboard_stats[11], native_keyboard_stats[13], native_keyboard_stats[12],
                     native_keyboard_stats[14], native_keyboard_stats[15], native_keyboard_stats[16]);
    if (n > 0 && (size_t)n < sizeof row && &native_keyboard_hid_stats)
      n += snprintf(row + n, sizeof row - (size_t)n, " hid=%d/%d/%d/%d/%d/%x", native_keyboard_hid_stats[0], native_keyboard_hid_stats[1],
                    native_keyboard_hid_stats[2], native_keyboard_hid_stats[3], native_keyboard_hid_stats[4], native_keyboard_hid_stats[5]);
    if (n > 0 && (size_t)n < sizeof row && &native_keyboard_accel)
      n += snprintf(row + n, sizeof row - (size_t)n, " acc=%d,%d,%d/%d", native_keyboard_accel[0], native_keyboard_accel[1],
                    native_keyboard_accel[2], native_keyboard_accel[3]);
    if (n > 0 && (size_t)n < sizeof row && &native_keyboard_touch_range)
      snprintf(row + n, sizeof row - (size_t)n, " tp=x%d-%d,y%d-%d", native_keyboard_touch_range[0], native_keyboard_touch_range[1],
               native_keyboard_touch_range[2], native_keyboard_touch_range[3]);
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
  if (log_display_on && &native_legacy_state_words) {
    static char words[240];
    if (native_legacy_state_words(words, sizeof words)) {at = put(out, at, cap, "[display state]", 16); at = put(out, at, cap, words, sizeof words);}
  }
  if (log_display_on && &native_legacy_prepare_report) {
    char row[220];
    native_legacy_prepare_report(row, sizeof row);
    at = put(out, at, cap, "[scanout prepare]", 18); at = put(out, at, cap, row, sizeof row);
  }
  if (&sheng_sigp_recovered) {
    char row[48];
    snprintf(row, sizeof row, "[sigp] recovered=%u", sheng_sigp_recovered);
    at = put(out, at, cap, row, sizeof row);
  }
  if (&sheng_idle_enter) {
    char row[160];
    snprintf(row, sizeof row, "[idle] cleared=%u refused=%u enter=%u/%u/%u/%u/%u/%u/%u/%u disallowed=%u/%u/%u/%u/%u/%u/%u/%u",
             sheng_idle_sigp_cleared, sheng_idle_sigp_refused, sheng_idle_enter[0], sheng_idle_enter[1], sheng_idle_enter[2], sheng_idle_enter[3],
             sheng_idle_enter[4], sheng_idle_enter[5], sheng_idle_enter[6], sheng_idle_enter[7], sheng_idle_disallowed[0], sheng_idle_disallowed[1],
             sheng_idle_disallowed[2], sheng_idle_disallowed[3], sheng_idle_disallowed[4], sheng_idle_disallowed[5], sheng_idle_disallowed[6],
             sheng_idle_disallowed[7]);
    at = put(out, at, cap, row, sizeof row);
  }
  if (log_display_on && &native_legacy_swap_rejected_count && native_legacy_swap_rejected_count) {
    char row[220];
    snprintf(row, sizeof row, "REJ acc=%u rej=%u r=%u st=%x/%x/%x op=%x/%x/%x wh=%ux%u s=%llx/%llx/%llx/%llx cls=%.24s",
             native_legacy_swap_accepted_count, native_legacy_swap_rejected_count, native_legacy_rej_reason, native_legacy_rej_state[0],
             native_legacy_rej_state[1], native_legacy_rej_state[2], native_legacy_rej_options[0], native_legacy_rej_options[1],
             native_legacy_rej_options[2], native_legacy_rej_size[0], native_legacy_rej_size[1],
             (unsigned long long)native_legacy_rej_surfaces[0], (unsigned long long)native_legacy_rej_surfaces[1],
             (unsigned long long)native_legacy_rej_surfaces[2], (unsigned long long)native_legacy_rej_surfaces[3], native_legacy_rej_class);
    at = put(out, at, cap, "[swap rejected]", 16); at = put(out, at, cap, row, sizeof row);
    snprintf(row, sizeof row, "REJ2 rect0=%u,%u,%u,%u rect1=%u,%u,%u,%u surf %llux%llu fmt=%08llx planes=%llu stride=%llu alloc=%llu len=%llu",
             native_legacy_rej_rects[0][0], native_legacy_rej_rects[0][1], native_legacy_rej_rects[0][2], native_legacy_rej_rects[0][3],
             native_legacy_rej_rects[1][0], native_legacy_rej_rects[1][1], native_legacy_rej_rects[1][2], native_legacy_rej_rects[1][3],
             (unsigned long long)native_legacy_rej_surface[0], (unsigned long long)native_legacy_rej_surface[1],
             (unsigned long long)native_legacy_rej_surface[2], (unsigned long long)native_legacy_rej_surface[3],
             (unsigned long long)native_legacy_rej_surface[4], (unsigned long long)native_legacy_rej_surface[5],
             (unsigned long long)native_legacy_rej_surface[6]);
    at = put(out, at, cap, row, sizeof row);
  }
  if (log_display_on && &native_legacy_swap_timeline) {
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
  if (log_gpu_on && (&native_adreno_metal_rows && native_adreno_metal_rows[0][0])) {
    at = put(out, at, cap, "[metal table]", 14);
    for (unsigned r = 0; r < 48 && at + 130 < cap; ++r)
      if (native_adreno_metal_rows[r][0]) at = put(out, at, cap, (const char *)native_adreno_metal_rows[r], 96);
  }
  // The framebuffer user clients' notification slots (is the render server's vsync registered?).
  if (log_gpu_on && &native_legacy_clients_dump) {
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
  if (log_display_on && &native_legacy_properties_now) {
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
  // HID input (HidInputProbe.cpp): report counts and handleReport results per kind, then the services below the last checked input
  // device and its properties, so a report that leaves the device can be followed to the event driver that took it.
  if (&native_hid_input_events && &native_hid_input_results) {
    char row[124];
    snprintf(row, sizeof row, "[hid] events %u %u %u %u %u %u %u %u results %d %d %d %d %d %d %d %d",
             native_hid_input_events[0], native_hid_input_events[1], native_hid_input_events[2], native_hid_input_events[3],
             native_hid_input_events[4], native_hid_input_events[5], native_hid_input_events[6], native_hid_input_events[7],
             native_hid_input_results[0], native_hid_input_results[1], native_hid_input_results[2], native_hid_input_results[3],
             native_hid_input_results[4], native_hid_input_results[5], native_hid_input_results[6], native_hid_input_results[7]);
    at = put(out, at, cap, row, sizeof row);
  }
  if (&native_hid_input_registry && &native_hid_input_props) {
    const char *texts[2] = {native_hid_input_registry, native_hid_input_props};
    const char tags[2] = {'R', 'P'};
    for (unsigned t = 0; t < 2; ++t) {
      const size_t n = strnlen(texts[t], t ? 1024 : 2048);
      for (size_t i = 0; i < n && at + 130 < cap; i += 112) {
        char row[124];
        snprintf(row, sizeof row, "H%c%02u %.112s", tags[t], (unsigned)(i / 112), texts[t] + i);
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
  if (log_gpu_on && (&native_adreno_ring_log_count && native_adreno_ring_log_count)) {
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
  if (log_gpu_on && (&native_adreno_ib_log_count && native_adreno_ib_log_count)) {
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
  if (log_gpu_on && (&native_adreno_fence_phys && native_adreno_fence_phys && &native_adreno_dram_read32 && native_adreno_dram_read32 && &native_adreno_cached_read32 &&
      native_adreno_cached_read32)) {
    char row[130];
    int n = snprintf(row, sizeof row, "[fence page] pa=%llx dram:", (unsigned long long)native_adreno_fence_phys);
    for (unsigned w = 0; w < 8 && n > 0 && n < 110; ++w) {uint32_t v = 0xdead; native_adreno_dram_read32(native_adreno_fence_phys + 4 * w, &v); n += snprintf(row + n, sizeof row - n, " %x", v);}
    at = put(out, at, cap, row, sizeof row);
    n = snprintf(row, sizeof row, "[fence page] cached:");
    for (unsigned w = 0; w < 8 && n > 0 && n < 110; ++w) {uint32_t v = 0xdead; native_adreno_cached_read32(native_adreno_fence_phys + 4 * w, &v); n += snprintf(row + n, sizeof row - n, " %x", v);}
    at = put(out, at, cap, row, sizeof row);
  }
  // Kernel GPU methods by number: calls, failures, last status and total time (the calls of the last 2 s are the difference of two slots).
  if (log_gpu_on && (&native_adreno_method_stats)) {
    char row[130];
    int n = snprintf(row, sizeof row, "[gpu methods] n:calls/fail/us");
    for (unsigned m = 1; m < 20; ++m) {
      if (!native_adreno_method_stats[m][0]) continue;
      if (n > 100) {at = put(out, at, cap, row, sizeof row); n = snprintf(row, sizeof row, "[gpu methods+]");}
      n += snprintf(row + n, sizeof row - n, " %u:%u/%u/%u", m, native_adreno_method_stats[m][0], native_adreno_method_stats[m][1], native_adreno_method_stats[m][3]);
    }
    at = put(out, at, cap, row, sizeof row);
  }
  if (log_gpu_on && (&native_adreno_vm_phase_us && (native_adreno_vm_phase_us[0][0] || native_adreno_vm_phase_us[1][0]))) {
    char row[170];
    for (unsigned k = 0; k < 2; ++k) {
      const volatile uint64_t *p = native_adreno_vm_phase_us[k];
      if (!p[0]) continue;
      // NativeAdrenoVm.cpp's phase slots: 1 route check, 2 cache maintenance, 3 table transaction, 4 the mapping order, 5 the map's
      // checks before it (its preflight) or the unmap's retirement.
      snprintf(row, sizeof row, "[vm %s] n=%llu us: observe=%llu %s=%llu batch=%llu %s=%llu %s=%llu max=%llu MiB=%llu", k ? "unmap" : "map",
               (unsigned long long)p[0], (unsigned long long)p[1], k ? "acquire" : "publish", (unsigned long long)p[2], (unsigned long long)p[3],
               k ? "erase" : "insert", (unsigned long long)p[4], k ? "retire" : "preflight", (unsigned long long)p[5], (unsigned long long)p[6],
               (unsigned long long)(p[7] >> 20));
      at = put(out, at, cap, row, sizeof row);
    }
  }
  // Per kind of VM change (NativeAdrenoVm.cpp native_adreno_vm_kind_us): count/total us/spans for owned maps, imports on the fast and
  // full alias path, unmaps by page TLBI and by ASID.
  if (log_gpu_on && (&native_adreno_vm_kind_us)) {
    static const char *const kinds[5] = {"owned", "import fast", "import full", "unmap page", "unmap asid"};
    char row[300];
    int n = snprintf(row, sizeof row, "[vm kinds]");
    for (unsigned k = 0; k < 5; ++k) {
      const volatile uint64_t *v = native_adreno_vm_kind_us[k];
      n += snprintf(row + n, sizeof row - (size_t)n, " | %s %llu/%lluus/%llu", kinds[k], (unsigned long long)v[0], (unsigned long long)v[1],
                    (unsigned long long)v[2]);
    }
    if (native_adreno_vm_kind_us[0][0] || native_adreno_vm_kind_us[1][0] || native_adreno_vm_kind_us[2][0]) at = put(out, at, cap, row, sizeof row);
  }
  // [bo memory]: user BOs from the system (count, spans, refused, last refusal status, MiB live and peak), kernel-mapped BOs, import spans.
  if (log_gpu_on && (&native_adreno_bo_memory)) {
    char row[240];
    snprintf(row, sizeof row, "[bo memory] system %u BOs %u spans, refused %u (last %x), live %u MiB (peak %u) | kernel %u BOs | import spans %u | contiguous %u refused %u | freed after the lock %u",
             native_adreno_bo_memory[0], native_adreno_bo_memory[1], native_adreno_bo_memory[2], native_adreno_bo_memory[3],
             native_adreno_bo_memory[4], native_adreno_bo_memory[5], native_adreno_bo_memory[6], native_adreno_bo_memory[7],
             &native_adreno_bo_contig ? native_adreno_bo_contig[0] : 0, &native_adreno_bo_contig ? native_adreno_bo_contig[1] : 0,
             &native_adreno_freed_outside ? native_adreno_freed_outside : 0);
    at = put(out, at, cap, row, sizeof row);
  }
  // GPU clients: name opens/refused/closes, live slot (- none), highest GPU address its mappings reached (MiB), last refusal.
  if (log_gpu_on && (&native_adreno_client_log)) {
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
  // [exc]: the last four user exceptions of the processes exc-name lists (oldest first): name[pid] type, pc, lr, far, esr, then the
  // return addresses of the frame chain (dyld cache addresses: slide them back offline).
  if (&sheng_exc_ring && &sheng_exc_count && sheng_exc_count) {
    char row[300];
    const uint32_t n = sheng_exc_count, first = n > 4 ? n - 4 : 0;
    for (uint32_t k = first; k < n; ++k) {
      const struct sheng_exc_record *e = &sheng_exc_ring[k & 3];
      size_t used = (size_t)snprintf(row, sizeof row, "[exc %u] %.19s[%u] t%u pc %llx lr %llx far %llx esr %x |", k, e->name, e->pid, e->type,
                                     (unsigned long long)e->pc, (unsigned long long)e->lr, (unsigned long long)e->far, e->esr);
      for (uint32_t i = 0; i < e->frames && i < 12 && used < sizeof row - 12; ++i)
        used += (size_t)snprintf(row + used, sizeof row - used, " %llx", (unsigned long long)e->ret[i]);
      at = put(out, at, cap, row, sizeof row);
    }
  }
  // [prof]: the last 32K interrupts: samples per CPU, then the processes they landed in (pid:name:count, busiest first; pid 0 the kernel's
  // own threads, idle included). [prof N]: for the three busiest, user and kernel samples and their hottest PCs (u/k, low 40 bits).
  if (&sheng_prof_head && &sheng_prof_ring) {
    const uint32_t head = sheng_prof_head;
    const size_t n = head < 1024 ? head : 1024;
    if (n && prof_snap && prof_rank) {
      memcpy(prof_snap, sheng_prof_ring, n * sizeof(uint64_t));
      prof_sort(prof_snap, n);
      size_t procs = 0;
      for (size_t i = 0; i < n;) {size_t j = i; const uint64_t pid = prof_snap[i] >> 48; while (j < n && prof_snap[j] >> 48 == pid) ++j;
        prof_rank[procs++] = ((uint64_t)(j - i) << 16) | pid; i = j;}
      prof_sort(prof_rank, procs);
      char row[400];
      size_t used = (size_t)snprintf(row, sizeof row, "[prof] samples=%u cpu", head);
      for (unsigned c = 0; c < 8; ++c) used += (size_t)snprintf(row + used, sizeof row - used, " %u", sheng_prof_cpu[c]);
      used += (size_t)snprintf(row + used, sizeof row - used, " |");
      for (size_t k = 0; k < 12 && k < procs && used < sizeof row - 40; ++k) {
        const int pid = (int)(prof_rank[procs - 1 - k] & 0xffff);
        char name[24] = "kernel";
        if (pid) proc_name(pid, name, sizeof name);
        used += (size_t)snprintf(row + used, sizeof row - used, " %d:%s:%llu", pid, name, (unsigned long long)(prof_rank[procs - 1 - k] >> 16));
      }
      at = put(out, at, cap, row, sizeof row);
      for (size_t k = 0; k < 3 && k < procs; ++k) {
        const uint64_t pid = prof_rank[procs - 1 - k] & 0xffff;
        size_t first = 0; while (first < n && prof_snap[first] >> 48 != pid) ++first;
        size_t last = first; while (last < n && prof_snap[last] >> 48 == pid) ++last;
        size_t kernel = 0; for (size_t i = first; i < last; ++i) kernel += (prof_snap[i] >> 47) & 1;
        // the hottest PCs: (count << 48 | index of the first sample), ranked
        size_t m = 0;
        for (size_t i = first; i < last;) {size_t j = i; while (j < last && prof_snap[j] == prof_snap[i]) ++j;
          prof_rank[procs + m++] = ((uint64_t)(j - i) << 48) | i; i = j;}
        prof_sort(prof_rank + procs, m);
        used = (size_t)snprintf(row, sizeof row, "[prof %llu] n=%lu kernel=%lu |", (unsigned long long)pid, (unsigned long)(last - first),
                                (unsigned long)kernel);
        for (size_t t = 0; t < 10 && t < m && used < sizeof row - 32; ++t) {
          const uint64_t e = prof_rank[procs + m - 1 - t], word = prof_snap[e & 0xffffffffffffull];
          used += (size_t)snprintf(row + used, sizeof row - used, " %c%llx:%llu", (word >> 47) & 1 ? 'k' : 'u',
                                   (unsigned long long)(word & 0xffffffffffull), (unsigned long long)(e >> 48));
        }
        at = put(out, at, cap, row, sizeof row);
      }
    }
  }
  // [gpu tables]: per-process GPU page tables: the switch mode (2 CP, 1 CPU), the SCM CP-aperture availability/result and the CP switch
  // check (IOReturn), then switches by the CP and by the CPU, address spaces made, freed, live and peak, the last
  // failure (IOReturn; bit 31: a free) and the step a failed make stopped at, then the table pages taken on demand (live, taken, freed,
  // refused by the system) and the GPU address size (ias).
  if (log_gpu_on && (&native_adreno_table_switches)) {
    char row[200];
    snprintf(row, sizeof row, "[gpu tables] mode %u aperture %x/%x check %x | cp %u cpu %u made %u freed %u live %u peak %u fail %x step %u | pages %u taken %u freed %u refused %u ias %u",
             &native_adreno_cp_aperture ? native_adreno_cp_aperture[3] : 0, &native_adreno_cp_aperture ? native_adreno_cp_aperture[0] : 0,
             &native_adreno_cp_aperture ? native_adreno_cp_aperture[1] : 0, &native_adreno_cp_aperture ? native_adreno_cp_aperture[2] : 0,
             native_adreno_table_switches[0], native_adreno_table_switches[1], native_adreno_table_switches[2],
             native_adreno_table_switches[3], native_adreno_table_switches[4], native_adreno_table_switches[5],
             native_adreno_table_switches[6], native_adreno_table_switches[7], &native_adreno_table_pages ? native_adreno_table_pages[0] : 0,
             &native_adreno_table_pages ? native_adreno_table_pages[1] : 0, &native_adreno_table_pages ? native_adreno_table_pages[2] : 0,
             &native_adreno_table_pages ? native_adreno_table_pages[3] : 0, NativeAdrenoGpuContextInputBits ? NativeAdrenoGpuContextInputBits() : 0);
    at = put(out, at, cap, row, sizeof row);
  }
  // [gpu recovery]: msm hangcheck and GPU restarts (NativeAdrenoSubmit::recover): hangs found, restarts, failed restarts, the last hung
  // fence, the last restart's IOReturn, submissions replayed, hangs with a guilty job, hangs of the kernel's VM; then the board's restart
  // (restarts, the step a failed one stopped at, its status, RBBM_STATUS after the reset); then the SMMU faults ended before a restart
  // and the last one's FSR, FAR (hi:lo) and FSYNR0.
  if (log_gpu_on && (&native_adreno_recovery)) {
    char row[260];
    snprintf(row, sizeof row, "[gpu recovery] hangs %u restarts %u failed %u fence %u status %x replayed %u guilty %u kernel %u | board %u step %u st %x rbbm %x | faults ended %u last fsr %x far %x:%x fsynr %x",
             native_adreno_recovery[0], native_adreno_recovery[1], native_adreno_recovery[2], native_adreno_recovery[3],
             native_adreno_recovery[4], native_adreno_recovery[5], native_adreno_recovery[6], native_adreno_recovery[7],
             &native_gpu_restart ? native_gpu_restart[0] : 0, &native_gpu_restart ? native_gpu_restart[1] : 0,
             &native_gpu_restart ? native_gpu_restart[2] : 0, &native_gpu_restart ? native_gpu_restart[3] : 0,
             &native_adreno_recovery_fault ? native_adreno_recovery_fault[0] : 0, &native_adreno_recovery_fault ? native_adreno_recovery_fault[1] : 0,
             &native_adreno_recovery_fault ? native_adreno_recovery_fault[3] : 0, &native_adreno_recovery_fault ? native_adreno_recovery_fault[2] : 0,
             &native_adreno_recovery_fault ? native_adreno_recovery_fault[4] : 0);
    at = put(out, at, cap, row, sizeof row);
    // Where the last recover() stopped: step (1 fence read, 2 restart hook, 3 table write, 4 nop, 5 replay vm, 6 replay submit), the status there, fence, last submitted.
    if (&native_adreno_recover_trace && native_adreno_recover_trace[0]) {
      snprintf(row, sizeof row, "[gpu recover stop] step %u st %x fence %u last %u", native_adreno_recover_trace[0], native_adreno_recover_trace[1],
               native_adreno_recover_trace[2], native_adreno_recover_trace[3]);
      at = put(out, at, cap, row, sizeof row);
    }
    // Where the last fault was in the guilty VM (NativeAdrenoPublish.cpp native_adreno_fault_place): its process, the address, the mapping
    // at or below it (start, KiB) and how far past that mapping's end the address is, and the next mapping's start.
    if (&native_adreno_fault_place && native_adreno_fault_place[3]) {
      const uint64_t start = native_adreno_fault_place[0], bytes = native_adreno_fault_place[1], at_ = native_adreno_fault_place[3];
      snprintf(row, sizeof row, "[gpu fault place] %.20s far %llx | below %llx %lluK past end %lld | next %llx", native_adreno_fault_owner,
               (unsigned long long)at_, (unsigned long long)start, (unsigned long long)(bytes >> 10),
               start ? (long long)(at_ - (start + bytes)) : 0LL, (unsigned long long)native_adreno_fault_place[2]);
      at = put(out, at, cap, row, sizeof row);
    }
  }
  // [gpu cp image]: the GPU bank's registers before / after the CP switch check (SCTLR TCR TCR2 MAIR0 TTBR1 lo/hi CBAR FSR, then the kit
  // status), and a fault the switch left: FAR lo/hi, FSYNR0, the status after clearing (0x100|kit).
  if (log_gpu_on && (&native_adreno_cp_image)) {
    char row[300];
    const volatile uint32_t *v = native_adreno_cp_image;
    snprintf(row, sizeof row, "[gpu cp image] %x %x %x %x %x %x %x %x k%u | %x %x %x %x %x %x %x %x k%u | far %x:%x fsynr %x after %x",
             v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[16], v[8], v[9], v[10], v[11], v[12], v[13], v[14], v[15], v[17],
             v[19], v[18], v[20], v[21]);
    at = put(out, at, cap, row, sizeof row);
  }
  // [gpu windows]: per client window the mappings that fell outside it (count, last size KiB, last end MiB), the latest failed mount (stage 1 no address
  // space, 2 core open, 3 window scope; status; queue base; count), then one line per process other than the render server: why its Metal device failed.
  if (log_gpu_on && (&native_adreno_window_overflow && &native_adreno_mount_fail)) {
    char row[300];
    snprintf(row, sizeof row, "[gpu windows] ovf %u/%uK/%uM %u/%uK/%uM %u/%uK/%uM %u/%uK/%uM | mount fail stage %u st %x qbase %u n %u",
             native_adreno_window_overflow[0][0], native_adreno_window_overflow[0][1], native_adreno_window_overflow[0][2],
             native_adreno_window_overflow[1][0], native_adreno_window_overflow[1][1], native_adreno_window_overflow[1][2],
             native_adreno_window_overflow[2][0], native_adreno_window_overflow[2][1], native_adreno_window_overflow[2][2],
             native_adreno_window_overflow[3][0], native_adreno_window_overflow[3][1], native_adreno_window_overflow[3][2],
             native_adreno_mount_fail[0], native_adreno_mount_fail[1], native_adreno_mount_fail[2], native_adreno_mount_fail[3]);
    at = put(out, at, cap, row, sizeof row);
    if (&native_adreno_client_text)
      for (unsigned i = 0; i < 8 && native_adreno_client_text[i].name[0]; ++i) {
        snprintf(row, sizeof row, "[gpu client text] %.19s: %.63s", native_adreno_client_text[i].name, native_adreno_client_text[i].text);
        at = put(out, at, cap, row, sizeof row);
      }
  }
  if (log_gpu_on && (&native_adreno_metal_askers)) {
    char row[400];
    size_t used = (size_t)snprintf(row, sizeof row, "[metal askers]");
    for (unsigned i = 0; i < 12 && native_adreno_metal_askers[i].name[0] && used < sizeof row; ++i)
      used += (size_t)snprintf(row + used, sizeof row - used, " %.19s:%u", native_adreno_metal_askers[i].name, native_adreno_metal_askers[i].reads);
    at = put(out, at, cap, row, sizeof row);
  }
  // The IO-coherence proof (GpuPowerProbe coherenceProbe) and the maintenance passes it saved: map, unmap, submission, completion.
  if (log_gpu_on && (&native_gpu_coherence_row && native_gpu_coherence_row[0])) {
    char row[300];
    snprintf(row, sizeof row, "[coherence] %.110s | on=%u skipped map=%u unmap=%u submit=%u complete=%u | fence pauses slept=%llu spun=%llu timed out=%llu",
             native_gpu_coherence_row,
             &native_adreno_io_coherent ? native_adreno_io_coherent : 0, &native_adreno_coherent_skips ? native_adreno_coherent_skips[0] : 0,
             &native_adreno_coherent_skips ? native_adreno_coherent_skips[1] : 0, &native_adreno_coherent_skips ? native_adreno_coherent_skips[2] : 0,
             &native_adreno_coherent_skips ? native_adreno_coherent_skips[3] : 0,
             (unsigned long long)(&native_gpu_idle_sleeps ? native_gpu_idle_sleeps : 0), (unsigned long long)(&native_gpu_idle_spins ? native_gpu_idle_spins : 0),
             (unsigned long long)(&native_gpu_idle_timeouts ? native_gpu_idle_timeouts : 0));
    at = put(out, at, cap, row, sizeof row);
  }
  // The SMMU table walk coherence proof (GpuPowerProbe walkProbe) and the table pages published since: by ordering alone, cleaned.
  if (log_gpu_on && (&native_gpu_walk_row && native_gpu_walk_row[0] && &native_adreno_walk_coherent)) {
    char row[160];
    snprintf(row, sizeof row, "[walk] %.70s | on=%u ordered=%u cleaned=%u", native_gpu_walk_row, native_adreno_walk_coherent[0],
             native_adreno_walk_coherent[1], native_adreno_walk_coherent[2]);
    at = put(out, at, cap, row, sizeof row);
  }
  // Edge contacts (drivers/ShengHidInput edge::): wrapper installed, contacts begun, of them from an edge, events skipped.
  if (&native_touch_edge_stats) {
    char row[128];
    // install: outcome (1 another class, 2 a slot did not check out, 3 installed), slots copied, first slot that did not check out.
    snprintf(row, sizeof row, "[touch edge] installed=%u contacts=%u from_edge=%u skipped=%u install=%u/%u/%u", native_touch_edge_stats[0],
             native_touch_edge_stats[1], native_touch_edge_stats[2], native_touch_edge_stats[3],
             &native_touch_edge_install ? native_touch_edge_install[0] : 0, &native_touch_edge_install ? native_touch_edge_install[1] : 0,
             &native_touch_edge_install ? native_touch_edge_install[2] : 0);
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
  if (log_gpu_on && (&native_adreno_submit_ibs)) {
    char row[120];
    snprintf(row, sizeof row, "[gpu ibs] submissions=%u ibs=%u max=%u over16=%u", native_adreno_submit_ibs[0], native_adreno_submit_ibs[1],
             native_adreno_submit_ibs[2], native_adreno_submit_ibs[3]);
    at = put(out, at, cap, row, sizeof row);
  }
  // The submissions' write-combine work: audits, IB lines compared and found dirty, pages set back to write-combined, the re-assertion's
  // total time, and the CPU cache maintenance passes (count, total ms, MiB).
  if (log_gpu_on && (&native_adreno_wc_audit && &native_adreno_reassert_ticks && &native_adreno_cache_stats)) {
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
  // [maint]: the GPU driver's CPU cache maintenance by the phase that asked for it, passes/KiB: allocation, map, submission,
  // unmap, completion, SMMU table pages, diagnostics; then the imports whose pages were all non-cacheable (no maintenance) and the others.
  if (log_gpu_on && (&native_adreno_maint_phase && &native_adreno_pool_stats_uncached)) {
    char row[300];
    const volatile uint64_t (*p)[2] = native_adreno_maint_phase;
    snprintf(row, sizeof row, "[maint] alloc %llu/%llu map %llu/%llu submit %llu/%llu unmap %llu/%llu complete %llu/%llu "
             "table %llu/%llu diag %llu/%llu | imports uncached %u cached %u",
             (unsigned long long)p[1][0], (unsigned long long)p[1][1],
             (unsigned long long)p[2][0], (unsigned long long)p[2][1], (unsigned long long)p[3][0], (unsigned long long)p[3][1],
             (unsigned long long)p[4][0], (unsigned long long)p[4][1], (unsigned long long)p[5][0], (unsigned long long)p[5][1],
             (unsigned long long)p[6][0], (unsigned long long)p[6][1], (unsigned long long)p[7][0], (unsigned long long)p[7][1],
             native_adreno_pool_stats_uncached[1], native_adreno_pool_stats_uncached[0]);
    at = put(out, at, cap, row, sizeof row);
  }
  // CPU clusters and L3 as voted (CpuFreqProbe, refreshed every round) and the GPU's initial HFI perf/bandwidth vote.
  if (&native_cpufreq_row && native_cpufreq_row[0]) {
    char row[200];
    snprintf(row, sizeof row, "[clocks] %.118s | gpu perf %u bw %u", native_cpufreq_row, &native_gpu_perf_vote ? native_gpu_perf_vote[0] : 0,
             &native_gpu_perf_vote ? native_gpu_perf_vote[1] : 0);
    at = put(out, at, cap, row, sizeof row);
    if (&native_cpufreq_gov && (native_cpufreq_gov[1] || native_cpufreq_gov[4] || native_cpufreq_gov[7])) {
      snprintf(row, sizeof row, "[cpu gov] d0 changes=%u mhz=%u busy=%u%% | d1 changes=%u mhz=%u busy=%u%% | d2 changes=%u mhz=%u busy=%u%% | boosts=%u frame=%u/%uw",
               native_cpufreq_gov[0], native_cpufreq_gov[1], native_cpufreq_gov[2], native_cpufreq_gov[3], native_cpufreq_gov[4],
               native_cpufreq_gov[5], native_cpufreq_gov[6], native_cpufreq_gov[7], native_cpufreq_gov[8],
               &native_cpufreq_boosts ? native_cpufreq_boosts : 0, &native_cpufreq_frame ? native_cpufreq_frame[0] : 0,
               &native_cpufreq_frame ? native_cpufreq_frame[1] : 0);
      at = put(out, at, cap, row, sizeof row);
      if (&native_cpufreq_throttle) {
        const volatile uint32_t *th = native_cpufreq_throttle;
        snprintf(row, sizeof row, "[cpu throttle] busy/throttled windows, lowest MHz: d0 %u/%u %u | d1 %u/%u %u | d2 %u/%u %u",
                 th[0], th[1], th[2], th[3], th[4], th[5], th[6], th[7], th[8]);
        at = put(out, at, cap, row, sizeof row);
      }
    }
    if (&native_gpu_dcvs && native_gpu_dcvs[0]) {
      snprintf(row, sizeof row, "[gpu dcvs] level=%u changes=%u failed=%u busy=%u%% at 220:%u 295:%u 348:%u 401:%u 475:%u 550:%u 615:%u 680:%u boosts touch=%u wake=%u",
               native_gpu_dcvs[1], native_gpu_dcvs[2], native_gpu_dcvs[3], native_gpu_dcvs[4], native_gpu_dcvs[5], native_gpu_dcvs[6],
               native_gpu_dcvs[7], native_gpu_dcvs[8], native_gpu_dcvs[9], native_gpu_dcvs[10], native_gpu_dcvs[11], native_gpu_dcvs[12],
               native_gpu_dcvs[13], native_gpu_dcvs[14]);
      at = put(out, at, cap, row, sizeof row);
    }
  }
  if (log_gpu_on && (&native_adreno_export_stats && (native_adreno_export_stats[0] || native_adreno_export_stats[1]))) {
    char row[120];
    snprintf(row, sizeof row, "[export] pieces=%u refused=%u generation=%u bytes=%u | complete generation=%u bytes=%u", native_adreno_export_stats[0],
             native_adreno_export_stats[1], native_adreno_export_stats[2], native_adreno_export_stats[3], export_ready, export_ready_bytes);
    at = put(out, at, cap, row, sizeof row);
  }
  if (&native_bluetooth_stats && (native_bluetooth_stats[0] || native_bluetooth_stats[14])) {
    const volatile uint64_t *b=native_bluetooth_stats;char row[300];
    snprintf(row,sizeof row,"[bluetooth] state=%llu error=%lld phase=%llu baud=%llu tx=%llu rx=%llu irq=%llu timeout=%llu sleep=%llu smmu=%llu/%llu fsr=%llx fw=%llx qup=%llx votes=%llx soc=%llx rompatch=%llx power=%llx owners=%llx",
        b[0],(long long)b[1],b[14],b[2],b[3],b[4],b[5],b[6],b[7],b[8],b[9],b[10],b[11],b[12],b[13],b[18],b[19],b[20],b[21]);
    at=put(out,at,cap,row,sizeof row);
    snprintf(row,sizeof row,"[bluetooth uart] fault=%llx/%llx/%llx/%llx malformed=%llu unmatched=%llu dma=%llu/%llu errors=%llu/%llu loop=%llx/%llx",
        b[22],b[23],b[24],b[25],b[26],b[27],b[28],b[29],b[15],b[16],b[30],b[31]);
    at=put(out,at,cap,row,sizeof row);
    if(&qbt_transport_activity) {
      snprintf(row,sizeof row,"[bluetooth stack] power=%u state=%u connect=%u channel=%u",
          qbt_transport_activity[0],qbt_transport_activity[1],qbt_transport_activity[2],qbt_transport_activity[3]);
      at=put(out,at,cap,row,sizeof row);
    }
    if(&native_bluetooth_processes) {
      snprintf(row,sizeof row,"[bluetooth proc] bluetoothd=%u/%u BlueTool=%u/%u",
          native_bluetooth_processes[0],native_bluetooth_processes[1],native_bluetooth_processes[2],native_bluetooth_processes[3]);
      at=put(out,at,cap,row,sizeof row);
    }
    if(&native_bluetooth_boot_state && native_bluetooth_boot_state[0]) {
      volatile uint32_t *s=native_bluetooth_boot_state;
      snprintf(row,sizeof row,"[bluetooth boot] pins=%x/%x pre-tx-rx=%u status=%x m=%x s=%x tx=%x rx=%x fifo=%x rxlen=%u tlv=%x final=%x",
          s[0],s[2],s[1],s[3],s[4],s[5],s[6],s[7],s[8],s[9],s[10],s[11]);
      at=put(out,at,cap,row,sizeof row);
    }
    if(&native_bluetooth_boot_text_length && &native_bluetooth_boot_text && native_bluetooth_boot_text) {
      uint32_t length=__atomic_load_n(&native_bluetooth_boot_text_length,__ATOMIC_ACQUIRE);
      if(length>8192)length=8192;
      for(uint32_t offset=0;offset<length;offset+=200) {
        uint32_t n=length-offset;if(n>200)n=200;
        snprintf(row,sizeof row,"[bluetooth boot text] %.*s",(int)n,native_bluetooth_boot_text+offset);
        at=put(out,at,cap,row,sizeof row);
      }
    }
    if(&native_bluetooth_boot_tail_length && &native_bluetooth_boot_tail) {
      uint32_t total=__atomic_load_n(&native_bluetooth_boot_tail_length,__ATOMIC_ACQUIRE);
      uint32_t count=total<256?total:256;
      for(uint32_t offset=0;offset<count;offset+=32) {
        int used=snprintf(row,sizeof row,"[bluetooth boot tail] %u/%u ",total-count+offset,total);
        uint32_t n=count-offset;if(n>32)n=32;
        for(uint32_t i=0;i<n;i++)used+=snprintf(row+used,sizeof row-(size_t)used,"%02x",native_bluetooth_boot_tail[(total-count+offset+i)%256]);
        at=put(out,at,cap,row,sizeof row);
      }
    }
    if(&native_bluetooth_boot_rx_length && &native_bluetooth_boot_rx) {
      uint32_t length=__atomic_load_n(&native_bluetooth_boot_rx_length,__ATOMIC_ACQUIRE);
      if(length>512)length=512;
      for(uint32_t offset=0;offset<length;offset+=32) {
        int used=snprintf(row,sizeof row,"[bluetooth boot rx] %u/%u ",offset,length);
        uint32_t n=length-offset;if(n>32)n=32;
        for(uint32_t i=0;i<n;i++)used+=snprintf(row+used,sizeof row-(size_t)used,"%02x",native_bluetooth_boot_rx[offset+i]);
        at=put(out,at,cap,row,sizeof row);
      }
    }
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
  if (log_display_on && &native_dpu_stats && &native_dpu_pipes && native_dpu_stats[0]) {
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
    // dpu=2 and 3 (DisplayTimingProbe.cpp native_dpu_more): VIG 0 and 1, CTL 0, the tear check of interfaces 1 and 2, the flush latch test.
    if (&native_dpu_more && (native_dpu_more[0] | native_dpu_more[20] | native_dpu_more[31])) {
      const volatile uint32_t *m = native_dpu_more;
      for (int k = 0; k < 2; ++k) {
        snprintf(row, sizeof row, "[dpu vig%d] xy=%x out=%x/%x src1=%x unpack=%x op=%x ubwc=%x fetch=%x sw=%x", k, m[10 * k], m[10 * k + 1],
                 m[10 * k + 2], m[10 * k + 3], m[10 * k + 4], m[10 * k + 5], m[10 * k + 6], m[10 * k + 7], m[10 * k + 8]);
        at = put(out, at, cap, row, sizeof row);
      }
      snprintf(row, sizeof row, "[dpu ctl0] top=%x start=%x fetch=%x intf flush=%x pipes=%x layers=%x master=%x | tear i1 %x/%x i2 %x/%x"
               " | flush test %x->%x %dus end %x frames %u..%u", m[20], m[21], m[22], m[23], m[24], m[25], m[26], m[27], m[28], m[29], m[30],
               m[31], m[32], (int)m[33], m[34], m[35], m[36]);
      at = put(out, at, cap, row, sizeof row);
    }
  }
  // The compositor's swapped surfaces (NativeLegacyDisplay.cpp): distinct, swaps, then per surface allocation KiB, physical segments, largest KiB.
  if (log_display_on && &native_legacy_surfaces && native_legacy_surfaces[0]) {
    char row[200];
    const volatile uint32_t *v = native_legacy_surfaces;
    snprintf(row, sizeof row, "[swap surfaces] %u seen, %u swaps | %uK %u segs max %uK | %uK %u segs max %uK | %uK %u segs max %uK | %uK %u segs max %uK",
             v[0], v[1], v[2] >> 10, v[3], v[4], v[5] >> 10, v[6], v[7], v[8] >> 10, v[9], v[10], v[11] >> 10, v[12], v[13]);
    at = put(out, at, cap, row, sizeof row);
  }
  // Direct scanout (dpu-scanout): the display bank's stage 1 (NativeUfs), then the flips (NativeLegacyDisplay.cpp native_legacy_direct).
  if (log_display_on && &native_ufs_dpu_state && native_ufs_dpu_state) {
    char row[600];
    const volatile uint32_t *c = native_ufs_dpu_check;
    int n = snprintf(row, sizeof row, "[direct scanout] bank %x ttbr0=%x tcr=%x fsr %x->%x far=%x%08x fsynr0=%x watched %uus now sctlr=%x fsr=%x",
                     native_ufs_dpu_state, c[0], c[1], c[2], c[3], c[5], c[4], c[6], c[7],
                     native_ufs_dpu_live ? native_ufs_dpu_live(0) : 0xdead, native_ufs_dpu_live ? native_ufs_dpu_live(1) : 0xdead);
    if (&native_legacy_direct) {
      const volatile uint32_t *d = native_legacy_direct;
      n += snprintf(row + n, sizeof row - (size_t)n, " | flips %u mapped %u mapfail %u latch>50ms %u max %uus mean %uus released %u copied %u to boot %u | held for the GPU %u total %uus longest %uus waits>=250ms %u",
                    d[0], d[1], d[2], d[3], d[4], d[0] ? d[5] / d[0] : 0, d[6], d[7], d[8], d[9], d[10], d[11], d[12]);
    }
    at = put(out, at, cap, row, sizeof row);
  }
  // The swap lifecycle's counters (NativeLegacyDisplay.cpp native_legacy_swapstat) and how many entries the kernel rings hold; the rings are in the
  // export's kernel.rings.txt.
  if (log_display_on && &native_legacy_swapstat && native_legacy_swapstat[0]) {
    char row[420];
    const volatile uint32_t *w = native_legacy_swapstat;
    snprintf(row, sizeof row, "[swap stats] validated %u kicks %u refused(in flight %u not ready %u no head %u latch %u) fence waits %u copy %u blank %u flips %u latches %u retires %u slow %u longest %uus max depth %u | rings tl=%u stall=%u dcvs=%u | scanout waits deadline %u int %u other %u woken %u",
             w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7], w[8], w[9], w[10], w[11], w[12], w[13], w[15],
             &native_adreno_tl_count ? native_adreno_tl_count : 0, &native_adreno_stall_log_count ? native_adreno_stall_log_count : 0,
             &native_gpu_dcvs_log_count ? native_gpu_dcvs_log_count : 0,
             &native_gop_wait_reasons ? native_gop_wait_reasons[0] : 0, &native_gop_wait_reasons ? native_gop_wait_reasons[1] : 0,
             &native_gop_wait_reasons ? native_gop_wait_reasons[2] : 0, &native_gop_wait_reasons ? native_gop_wait_reasons[3] : 0);
    at = put(out, at, cap, row, sizeof row);
  }
  // The display streams in the apps SMMU as the boot loader left them (NativeUfs, dpu=2): per stream group|bank, SMR, S2CR, CBAR,
  // CBA2R, SCTLR, TTBR0, TCR, FSR.
  if (log_display_on && &native_ufs_dpu_smmu && native_ufs_dpu_smmu[0]) {
    char row[300];
    const volatile uint32_t *u = native_ufs_dpu_smmu;
    snprintf(row, sizeof row, "[dpu smmu] %x scr0=%x idr0=%x idr1=%x | 1c00 %x %x %x cbar=%x/%x sctlr=%x ttbr0=%x%08x tcr=%x fsr=%x"
             " | 1c02 %x %x %x cbar=%x/%x sctlr=%x ttbr0=%x%08x tcr=%x fsr=%x", u[0], u[1], u[2], u[3],
             u[4], u[5], u[6], u[7], u[8], u[9], u[11], u[10], u[12], u[22], u[13], u[14], u[15], u[16], u[17], u[18], u[20], u[19], u[21], u[23]);
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
    if (&native_usb_stats) {
      // QcomDwc3: stage, core id, hardware parameters 0, 3, 7, GDSCs, SMMU, repeater, rails, session bits, interrupts, power calls.
      snprintf(row, sizeof row, "[usb] st=%d id=%x hw=%x/%x/%x gdsc=%x/%x smmu=%x rpt=%x rails=%x vbus=%x irq=%u on=%u off=%u", (int)native_usb_stats[0],
               native_usb_stats[1], native_usb_stats[2], native_usb_stats[3], native_usb_stats[4], native_usb_stats[5], native_usb_stats[6],
               native_usb_stats[7], native_usb_stats[8], native_usb_stats[9], native_usb_stats[10], native_usb_stats[11], native_usb_stats[12],
               native_usb_stats[13]);
      at = put(out, at, cap, row, sizeof row);
      snprintf(row, sizeof row, "[usb core] reset=%x dsts=%x dcfg=%x gctl=%x phy=%x start=%u power=%x load=%x", native_usb_stats[14], native_usb_stats[15],
               native_usb_stats[16], native_usb_stats[17], native_usb_stats[18], native_usb_stats[19], native_usb_stats[20], native_usb_stats[21]);
      at = put(out, at, cap, row, sizeof row);
      snprintf(row, sizeof row, "[usb diag] bus=%x/%x guctl1=%x gfsr=%x dsts=%x ep=%x evcount=%x ep0=%x fsr=%x evsz=%x", native_usb_stats[22],
               native_usb_stats[23], native_usb_stats[24], native_usb_stats[25], native_usb_stats[26], native_usb_stats[27], native_usb_stats[28],
               native_usb_stats[29], native_usb_stats[30], native_usb_stats[31]);
      at = put(out, at, cap, row, sizeof row);
      snprintf(row, sizeof row, "[usb ss] retimer=%x pcs=%x reverse=%u vbus=%x ltssm=%x/%x pipe=%x cycle=%u", native_usb_stats[32], native_usb_stats[33], native_usb_stats[34], native_usb_stats[35], native_usb_stats[36], native_usb_stats[37], native_usb_stats[38], native_usb_stats[39]);
      at = put(out, at, cap, row, sizeof row);
      snprintf(row, sizeof row, "[usb ucsi] state=%x status=%x cci=%x changes=%x", native_usb_stats[40], native_usb_stats[41], native_usb_stats[42], native_usb_stats[43]);
      at = put(out, at, cap, row, sizeof row);
      snprintf(row, sizeof row, "[usb host] role=%x n=%u xhci=%x cmd=%x sts=%x port=%x/%x hcs=%x dev=%u last=%x", native_usb_stats[44], native_usb_stats[45], native_usb_stats[46], native_usb_stats[47], native_usb_stats[48], native_usb_stats[49], native_usb_stats[50], native_usb_stats[51], native_usb_stats[52], native_usb_stats[53]);
      at = put(out, at, cap, row, sizeof row);
    }
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
  // Speaker playback (AudioProbe.cpp): stage (70 done; negative: the failing step), last DSP status, DSP answers, buffer map handle,
  // buffers sent and returned, amplifiers identified / PLL locked / reporting errors (bit per amplifier), SMMU result, the two I2C engines,
  // last opcode, I2C failures, DFS level 0 kHz, ms to playing; then per amplifier: device id, revision, EINT1, EINT2.
  if (&native_audio_stats && native_audio_stats[0]) {
    char row[240];
    snprintf(row, sizeof row, "[audio] st=%d status=%d ans=%d map=%x sent=%d done=%d amps=%x lock=%x err=%x smmu=%x bus=%x/%x op=%x i2cfail=%d starts=%x serve=%dms",
             native_audio_stats[0], native_audio_stats[1], native_audio_stats[2], native_audio_stats[3], native_audio_stats[4], native_audio_stats[5],
             native_audio_stats[6], native_audio_stats[7], native_audio_stats[8], native_audio_stats[9], native_audio_stats[10], native_audio_stats[11],
             native_audio_stats[12], native_audio_stats[13], native_audio_stats[14], native_audio_stats[15]);
    at = put(out, at, cap, row, sizeof row);
    if (&native_audio_amps) {
      snprintf(row, sizeof row, "[amps] %x/%x/%x/%x %x/%x/%x/%x %x/%x/%x/%x %x/%x/%x/%x %x/%x/%x/%x %x/%x/%x/%x",
               native_audio_amps[0][0], native_audio_amps[0][1], native_audio_amps[0][2], native_audio_amps[0][3],
               native_audio_amps[1][0], native_audio_amps[1][1], native_audio_amps[1][2], native_audio_amps[1][3],
               native_audio_amps[2][0], native_audio_amps[2][1], native_audio_amps[2][2], native_audio_amps[2][3],
               native_audio_amps[3][0], native_audio_amps[3][1], native_audio_amps[3][2], native_audio_amps[3][3],
               native_audio_amps[4][0], native_audio_amps[4][1], native_audio_amps[4][2], native_audio_amps[4][3],
               native_audio_amps[5][0], native_audio_amps[5][1], native_audio_amps[5][2], native_audio_amps[5][3]);
      at = put(out, at, cap, row, sizeof row);
    }
    // The audio channel's GLINK sends (AdspProbe.cpp noteChannel): sent, deferred, refused, queued, intent asked, remote intents, the
    // largest remote intent, the last deferred length.
    if (&native_adsp_audio) {
      snprintf(row, sizeof row, "[audio-glink] sent=%d deferred=%d refused=%d queued=%d asked=%d intents=%d largest=%d lastlen=%d",
               native_adsp_audio[0], native_adsp_audio[1], native_adsp_audio[2], native_adsp_audio[3], native_adsp_audio[4],
               native_adsp_audio[5], native_adsp_audio[6], native_adsp_audio[7]);
      at = put(out, at, cap, row, sizeof row);
    }
    // Every distinct property access of the audio server, in order: selector, object, scope letter, g/s (get/set), upper case if it failed.
    if (&sheng_audio_diag && sheng_audio_diag()) {
      const ShengAudioTrace *tr = (const ShengAudioTrace *)((const uint8_t *)sheng_audio_diag() - SHENG_AUDIO_DIAG_OFFSET + SHENG_AUDIO_TRACE_OFFSET);
      const unsigned total = tr->count < SHENG_AUDIO_TRACE_SLOTS ? tr->count : SHENG_AUDIO_TRACE_SLOTS;
      for (unsigned first = 0; first < total; first += 20) {
        int n = snprintf(row, sizeof row, "[audio-trace %u/%u]", first, total);
        for (unsigned i = first; i < total && i < first + 20 && n > 0 && n < (int)sizeof row - 12; ++i) {
          const uint32_t s = tr->entry[i][0], w = tr->entry[i][1];
          // Kind 8: a throw in the audio server's routing layer at offset s; kind 4: a failing call of that layer.
          if (w & 8) {n += snprintf(row + n, sizeof row - (size_t)n, " throw@%x", s); continue;}
          const char kind = (char)((w & 4) ? 'V' : (w & 1) ? ((w & 2) ? 'S' : 's') : ((w & 2) ? 'G' : 'g'));
          n += snprintf(row + n, sizeof row - (size_t)n, " %c%c%c%c%u%c%c", (char)(s >> 24), (char)(s >> 16), (char)(s >> 8), (char)s, w >> 24,
                        (char)((w >> 16) & 0xff), kind);
        }
        at = put(out, at, cap, row, sizeof row);
      }
    }
    // What the plug-in could not answer: loads of the plug-in, then up to 16 'selector'@object/scope entries.
    if (&sheng_audio_diag && sheng_audio_diag()) {
      const ShengAudioDiag *d = sheng_audio_diag();
      int n = snprintf(row, sizeof row, "[audio-props] loads=%u vol=%u sets=%u mix=%u inmiss=%u unknown=%u", d->loads, d->volume_milli, d->volume_sets,
                       d->mix_peak, d->in_misses, d->count);
      for (unsigned i = 0; i < d->count && i < 16 && n > 0 && n < (int)sizeof row - 16; ++i) {
        const uint32_t s = d->selector[i], w = d->where[i];
        n += snprintf(row + n, sizeof row - (size_t)n, " %c%c%c%c@%u%c", (char)(s >> 24), (char)(s >> 16), (char)(s >> 8), (char)s, w >> 24,
                      (char)(w & 0xff));
      }
      at = put(out, at, cap, row, sizeof row);
    }
    // The audio service (ShengAudioService.cpp): state, opens, closes, requests, refused requests, memory maps, the lock sound press, the
    // playback / capture peak, playback periods taken before the plug-in had written them, and the plug-in's write position (frames).
    if (&native_audio_service && native_audio_service[0]) {
      const ShengAudioControl *ac = &sheng_audio_diag && sheng_audio_diag() ?
          (const ShengAudioControl *)((const uint8_t *)sheng_audio_diag() - SHENG_AUDIO_DIAG_OFFSET) : NULL;
      snprintf(row, sizeof row, "[audio-service] st=%d opens=%d closes=%d requests=%d refused=%d maps=%d press=%d peak=%d/%d late=%u holes=%u written=%llu",
               native_audio_service[0], native_audio_service[1], native_audio_service[2], native_audio_service[3], native_audio_service[4],
               native_audio_service[5], native_audio_service[6], native_audio_service[7] & 0xffff, (native_audio_service[7] >> 16) & 0xffff,
               ac ? ac->out_late : 0, ac ? ac->out_holes : 0, ac ? (unsigned long long)ac->out_written : 0ull);
      at = put(out, at, cap, row, sizeof row);
    }
    // Where the audio server writes and reads relative to the play position (frames): write-ahead least..most, read-behind least..most,
    // frames of the last write, writes.
    if (&sheng_audio_diag && sheng_audio_diag()) {
      const ShengAudioDiag *d = sheng_audio_diag();
      snprintf(row, sizeof row, "[audio-io] ahead=%d..%d behind=%d..%d frames=%u writes=%u overlap=%u/%u total=%u gaps=%u/%u int=%u/%u/%u/%u/%u/%u skew=%d..%d",
               d->ahead_min, d->ahead_max, d->behind_min, d->behind_max, d->io_frames, d->writes, d->overlaps, d->overlap_max,
               d->frames_total, d->gaps, d->gap_frames, d->interval[0], d->interval[1], d->interval[2], d->interval[3], d->interval[4],
               d->interval[5], d->skew_min, d->skew_max);
      at = put(out, at, cap, row, sizeof row);
      snprintf(row, sizeof row, "[audio-thread] pri=%d base=%d min=%d policy=%d tc=%d period=%u comp=%u constraint=%u clock=%u/%d", d->io_pri,
               d->io_base, d->io_pri_min, d->io_policy, d->io_tc, d->io_period, d->io_computation, d->io_constraint, d->host_clock,
               d->host_clock_result);
      at = put(out, at, cap, row, sizeof row);
      if (&native_audio_clock) {
        snprintf(row, sizeof row, "[audio-clock] step=%llu va=%llx pa=%llx value=%llx", native_audio_clock[0], native_audio_clock[1],
                 native_audio_clock[2], native_audio_clock[3]);
        at = put(out, at, cap, row, sizeof row);
      }
    }
    // The plug-in's last requests (AudioProbe.cpp sheng_audio_request): ms since boot and selector (0/1 output start/stop, 2/3 input).
    if (&native_audio_requests && native_audio_requests[16]) {
      int n = snprintf(row, sizeof row, "[audio-requests]");
      const uint64_t next = native_audio_requests[16], first = next > 16 ? next - 16 : 0;
      for (uint64_t i = first; i < next && n < (int)sizeof row - 16; ++i) {
        const uint64_t e = native_audio_requests[i % 16];
        n += snprintf(row + n, sizeof row - n, " %llu:%llu", e >> 4, e & 15);
      }
      at = put(out, at, cap, row, sizeof row);
    }
    // Playback and capture against the shared timeline (AudioProbe.cpp syncPlayback / syncCapture).
    if (&native_audio_sync) {
      snprintf(row, sizeof row, "[audio-sync] play resync=%d drift=%d..%d lead=%d capture resync=%d short=%d passed=%d", native_audio_sync[0],
               native_audio_sync[1], native_audio_sync[2], native_audio_sync[4], native_audio_sync[3] & 0xffff,
               (native_audio_sync[3] >> 16) & 0xffff, &native_audio_passed ? native_audio_passed : -1);
      at = put(out, at, cap, row, sizeof row);
    }
    if (&native_audio_pa) {
      snprintf(row, sizeof row, "[audio-buffer] pa=%llx", (unsigned long long)native_audio_pa);
      at = put(out, at, cap, row, sizeof row);
    }
  }
  // The microphone path (AudioProbe.cpp micBringUp): stage, PRM status, rail, TX SoundWire version, link, device status, device id,
  // codec efuse, codec SCP id, TX/VA MCLK control, interrupt status, component parameters, read timeouts, ms.
  if (&native_mic_stats && native_mic_stats[0]) {
    char row[220];
    snprintf(row, sizeof row, "[mic] st=%d prm=%x rail=%d swr=%x link=%x slaves=%x id=%x/%x efuse=%x scp=%x txclk=%x vaclk=%x irq=%x params=%x rdto=%d ms=%d",
             native_mic_stats[0], native_mic_stats[1], native_mic_stats[2], native_mic_stats[3], native_mic_stats[4], native_mic_stats[5],
             native_mic_stats[6], native_mic_stats[7], native_mic_stats[8], native_mic_stats[9], native_mic_stats[10], native_mic_stats[11],
             native_mic_stats[12], native_mic_stats[13], native_mic_stats[14], native_mic_stats[15]);
    at = put(out, at, cap, row, sizeof row);
    if (&native_mic_capture && native_mic_capture[0]) {
      snprintf(row, sizeof row, "[mic capture] st=%d status=%d asked=%d back=%d bytes=%d peak=%d mean=%d nonzero=%d", native_mic_capture[0],
               native_mic_capture[1], native_mic_capture[2], native_mic_capture[3], native_mic_capture[4], native_mic_capture[5],
               native_mic_capture[6], native_mic_capture[7]);
      at = put(out, at, cap, row, sizeof row);
    }
    if (&native_mic_regs) {
      snprintf(row, sizeof row, "[mic regs] tx %x %x %x %x va %x %x %x swr %x %x %x %x %x %x %x pin %x slew %x", native_mic_regs[0], native_mic_regs[1],
               native_mic_regs[2], native_mic_regs[3], native_mic_regs[4], native_mic_regs[5], native_mic_regs[6], native_mic_regs[7], native_mic_regs[8],
               native_mic_regs[9], native_mic_regs[10], native_mic_regs[11], native_mic_regs[12], native_mic_regs[13], native_mic_regs[14], native_mic_regs[15]);
      at = put(out, at, cap, row, sizeof row);
    }
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
  if (log_gpu_on && (&native_adreno_map_fail && native_adreno_map_fail[0][1])) {
    char row[170];
    int n = snprintf(row, sizeof row, "[map fails]");
    for (unsigned k = 0; k < 5 && native_adreno_map_fail[k][1]; ++k) n += snprintf(row + n, sizeof row - n, " %x x%u", native_adreno_map_fail[k][0], native_adreno_map_fail[k][1]);
    snprintf(row + n, sizeof row - n, " | at last: spans=%u objects=%u iova_pfn=%x handle=%x", native_adreno_map_fail[5][0], native_adreno_map_fail[5][1], native_adreno_map_fail[6][0], native_adreno_map_fail[6][1]);
    at = put(out, at, cap, row, sizeof row);
  }
  if (log_gpu_on && (&native_adreno_client_stats)) {
    // per client slot: live objects/peak, refused BoAllocs, last refusal status and bytes, highest mapped end in the window (MiB),
    // MiB of its own BOs live/peak
    char row[260];
    int n = snprintf(row, sizeof row, "[gpu objects]");
    for (unsigned k = 0; k < 5; ++k)
      if (native_adreno_client_stats[k][1] || native_adreno_client_stats[k][2])
        n += snprintf(row + n, sizeof row - n, " s%u %u/%u r%u %x/%u w%uM", k, native_adreno_client_stats[k][0], native_adreno_client_stats[k][1],
                      native_adreno_client_stats[k][2], native_adreno_client_stats[k][3], native_adreno_client_stats[k][4], native_adreno_client_stats[k][5]),
      n += &native_adreno_client_bytes ? snprintf(row + n, sizeof row - n, " o%u/%uM", native_adreno_client_bytes[k][0], native_adreno_client_bytes[k][1]) : 0;
    at = put(out, at, cap, row, sizeof row);
  }
  if (log_gpu_on && (&native_adreno_fence_wait && native_adreno_fence_wait[0])) {
    char row[160];
    snprintf(row, sizeof row, "[fence wait] calls=%u timeouts=%u last timed-out fence=%u gpu wrote=%u submitted=%u completed=%u no-job=%u refused fence=%u",
             native_adreno_fence_wait[0], native_adreno_fence_wait[1], native_adreno_fence_wait[2], native_adreno_fence_wait[3],
             native_adreno_fence_wait[4], native_adreno_fence_wait[5], native_adreno_fence_wait[6], native_adreno_fence_wait[7]);
    at = put(out, at, cap, row, sizeof row);
    // The callers' waits: the pauses run outside the connection lock, and so do the polls before the fence has passed.
    if (&native_adreno_wait_outer && native_adreno_wait_outer[0]) {
      snprintf(row, sizeof row, "[fence wait] outer waits=%llu us=%llu pauses outside the lock=%llu longest=%lluus unlocked polls=%llu",
               (unsigned long long)native_adreno_wait_outer[0], (unsigned long long)native_adreno_wait_outer[1],
               (unsigned long long)native_adreno_wait_outer[2], (unsigned long long)native_adreno_wait_outer[3],
               (unsigned long long)native_adreno_wait_outer[4]);
      at = put(out, at, cap, row, sizeof row);
    }
  }
  // The CP's registers once a fence stopped advancing: RBBM status 0-3, interrupt, CP fault, status 1, protect, ring rptr/wptr,
  // IB1 base/rem, IB2 base/rem, BV rptr, BV IB1 base/rem, BV IB2 rem, BV IB2 base.
  // The CP switch between the two rings (NativeAdrenoPreempt): counters, the ring the CP runs, a switch in flight, the control register as read now
  // (STOP, bit 0, still set: the hardware never finished it) and checkCpSwitch's steps (createVm, probeBuffer, submit, waitAll, readTtbr0, fence).
  if (log_gpu_on && (&native_adreno_preempt_stats)) {
    uint32_t v[26] = {0};
    char row[380];
    const bool on = native_adreno_preempt_stats(v);
    snprintf(row, sizeof row, "[gpu preempt] %s trig %u done %u skip %u fault %u disjoint %u wptr %u last %uus | loaded %u flight %u cntl %x | check %x %x %x %x %x f%u | dead %u rescues %u | rec0 rptr %x wptr %x err %x info %x | rec1 rptr %x wptr %x err %x info %x | cpsw irq %u last %x | switches logged %u",
             on ? "on" : "off/busy", v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9], v[10], v[11], v[12], v[13], v[14], v[15], v[16], v[17], v[18], v[19], v[20], v[21], v[22], v[23], v[24], v[25],
             &native_gpu_preempt ? native_gpu_preempt[1] : 0, &native_gpu_preempt ? native_gpu_preempt[2] : 0,
             &native_adreno_switch_count ? native_adreno_switch_count : 0);
    at = put(out, at, cap, row, sizeof row);
  }
  // Fenced WPTR writes (NativeAdrenoRing.cpp writeWptr): writes, writes dropped at least once, repeated writes, writes still dropped.
  if (log_gpu_on && (&native_adreno_wptr_fence && native_adreno_wptr_fence[0])) {
    char row[120];
    snprintf(row, sizeof row, "[gpu wptr] writes=%u dropped=%u repeats=%u lost=%u", native_adreno_wptr_fence[0], native_adreno_wptr_fence[1],
             native_adreno_wptr_fence[2], native_adreno_wptr_fence[3]);
    at = put(out, at, cap, row, sizeof row);
  }
  if (log_gpu_on && (&native_adreno_unstick && native_adreno_unstick[0])) {
    char row[200];
    // [0] write-pointer retries, [1] fence of the last, [2] translation drops, [4] bus drains with ack [5]/[6], [9..11] stalls that ended after
    // the write pointer / translations / drain stage, [12] the most timeouts one stall took, [13] a wait's timeout in us.
    snprintf(row, sizeof row, "[gpu unstick] kick=%u last=%u tlb=%u drain=%u ack=%x/%x ended: kick=%u tlb=%u drain=%u longest=%u unit=%uus",
             native_adreno_unstick[0], native_adreno_unstick[1], native_adreno_unstick[2], native_adreno_unstick[4],
             native_adreno_unstick[5], native_adreno_unstick[6], native_adreno_unstick[9], native_adreno_unstick[10],
             native_adreno_unstick[11], native_adreno_unstick[12], native_adreno_unstick[13]);
    at = put(out, at, cap, row, sizeof row);
  }
  if (log_gpu_on && (&native_adreno_fence_pending)) {
    char row[160];
    // Fences by how long they stayed pending after their first timed-out wait: under 1, 2, 4, 8, 16, 32, 64 ms, then 64 ms or more.
    snprintf(row, sizeof row, "[gpu pending] <1=%u <2=%u <4=%u <8=%u <16=%u <32=%u <64=%u more=%u", native_adreno_fence_pending[0],
             native_adreno_fence_pending[1], native_adreno_fence_pending[2], native_adreno_fence_pending[3], native_adreno_fence_pending[4],
             native_adreno_fence_pending[5], native_adreno_fence_pending[6], native_adreno_fence_pending[7]);
    at = put(out, at, cap, row, sizeof row);
  }
  if (log_gpu_on && (&native_adreno_stall && native_adreno_stall[0])) {
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
  if (log_display_on && &native_gop_wait_stats && native_gop_wait_stats[0]) {
    char row[160];
    snprintf(row, sizeof row, "[scanout wait] calls=%llu timeouts=%llu asked=%llums last=%lluus min=%lluus max=%lluus mean=%lluus", native_gop_wait_stats[0],
             native_gop_wait_stats[1], native_gop_wait_stats[2], native_gop_wait_stats[3] / 1000, native_gop_wait_stats[4] / 1000,
             native_gop_wait_stats[5] / 1000, native_gop_wait_stats[1] ? native_gop_wait_stats[6] / native_gop_wait_stats[1] / 1000 : 0);
    at = put(out, at, cap, row, sizeof row);
  }
  if (log_gpu_on && (&native_adreno_view_diag && native_adreno_view_diag[0])) {
    char row[120];
    snprintf(row, sizeof row, "[bo views] calls=%u new=%u cached=%u refused=%u last=%x inside=%u", native_adreno_view_diag[0], native_adreno_view_diag[1],
             native_adreno_view_diag[2], native_adreno_view_diag[3], native_adreno_view_diag[4], native_adreno_view_diag[5]);
    at = put(out, at, cap, row, sizeof row);
  }
  if (log_gpu_on && (&native_adreno_import_diag && native_adreno_import_diag[0])) {
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
    snprintf(row, sizeof row, "[diag] status=%d files=%u skipped=%u errors=%u bytes=%u first=%u alloc=%u", boot_diag_status, &boot_diag_files ? boot_diag_files : 0,
             &boot_diag_skipped ? boot_diag_skipped : 0, &boot_diag_errors ? boot_diag_errors : 0, diag_bytes, diag_first,
             &boot_diag_alloc ? boot_diag_alloc : 0);
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
    char *const spawn = compose_spawn;
    const size_t n = spawn ? boot_progress_spawn_rows(spawn, 16384) : 0;
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
  // A ring larger than the window keeps its newest bytes, which hold the end of a run.
  int i = size > (int)(LogMsgBlocks * LogBlock) ? size - (int)(LogMsgBlocks * LogBlock) : 0;
  for (; i < size && n < LogMsgBlocks * LogBlock; ++i) {
    char c = m->msg_bufc[(x + i) % size];
    if (c) out[n++] = (uint8_t)c;
  }
  return n;
}

// The message stream (msg_mode 1). The kernel ring is read where the last round stopped: msg_bufx is the next byte the kernel writes,
// so the bytes from the last round's index up to it are new. The first round, and any round that finds another ring (size or buffer
// changed), appends the whole ring oldest first instead. More than a ring of messages between two rounds loses the older part.
// written: the stream offset up to which the medium holds the stream (a failed write leaves its bytes for the next round).
static struct {const char *bufc; int size, bufx, synced; uint64_t head, written;} msg_stream;
static void msg_append(uint8_t c) {
  if (!c) return;
  const uint64_t cap = (uint64_t)LogMsgBlocks * LogBlock;
  boot_log_msg_ring[msg_stream.head % cap] = c;
  ++msg_stream.head;
}
// Appends the kernel messages since the last round; returns the number of bytes appended.
static uint64_t msg_stream_round(uint32_t *resyncs) {
  struct boot_log_msgbuf *m = msgbufp;
  const uint64_t before = msg_stream.head;
  if (!m || !m->msg_bufc || m->msg_size <= 0) return 0;
  const int size = m->msg_size;
  int x = m->msg_bufx;
  if (x < 0 || x >= size) x = 0;
  if (!msg_stream.synced || msg_stream.bufc != m->msg_bufc || msg_stream.size != size) {
    if (msg_stream.synced) ++*resyncs;
    const int window = (int)(LogMsgBlocks * LogBlock);
    for (int i = size > window ? size - window : 0; i < size; ++i) msg_append((uint8_t)m->msg_bufc[(x + i) % size]);
    msg_stream.bufc = m->msg_bufc; msg_stream.size = size; msg_stream.synced = 1;
  } else {
    for (int i = msg_stream.bufx; i != x; i = i + 1 == size ? 0 : i + 1) msg_append((uint8_t)m->msg_bufc[i]);
  }
  msg_stream.bufx = x;
  return msg_stream.head - before;
}
// Writes the blocks of the message area that the bytes not yet on the medium reached (at most the whole area, in up to two pieces).
static int msg_stream_write(void) {
  if (msg_stream.written == msg_stream.head) return 1;
  uint64_t first = msg_stream.written / LogBlock, end = (msg_stream.head + LogBlock - 1) / LogBlock;
  if (end - first > LogMsgBlocks) first = end - LogMsgBlocks;
  int ok = 1;
  while (ok && first < end) {
    const uint32_t at = (uint32_t)(first % LogMsgBlocks);
    uint32_t n = (uint32_t)(end - first);
    if (n > LogMsgBlocks - at) n = LogMsgBlocks - at;
    ok = native_ufs_log_write(LogMsgFirst + at, n, boot_log_msg_ring + (uint64_t)at * LogBlock);
    first += n;
  }
  if (ok) msg_stream.written = msg_stream.head;
  return ok;
}

// Log groups (boot arguments log-gpu, log-display, log-wifi, log-audio, log-input, log-power, log-sys; each =1 turns its rows on, all are off
// without them). A slot's text is a list of sections: a line that starts with "[tag]" opens one and the lines after it (until the next
// "[") belong to it. The slot's own head line and the panic text are always kept; diag has its own switch and writes other files.
enum {LogGpu, LogDisplay, LogWifi, LogAudio, LogInput, LogPower, LogSys, LogGroups};
static const char *const log_group_arg[LogGroups] = {"log-gpu", "log-display", "log-wifi", "log-audio", "log-input", "log-power", "log-sys"};
static uint32_t log_group_on;
// Tag prefixes by group, the first match wins ("gpu dcvs" before "gpu"); a tag in no list is a system row.
static const struct {const char *prefix; uint8_t group;} log_tags[] = {
  {"gpu dcvs", LogPower}, {"gpu", LogGpu}, {"ring", LogGpu}, {"ib]", LogGpu}, {"fence", LogGpu}, {"metal", LogGpu}, {"mfb", LogGpu},
  {"bo ", LogGpu}, {"vm ", LogGpu}, {"import", LogGpu}, {"walk", LogGpu}, {"coherence", LogGpu},
  {"display", LogDisplay}, {"direct scanout", LogDisplay}, {"scanout", LogDisplay}, {"dpu", LogDisplay}, {"swap", LogDisplay},
  {"wifi", LogWifi}, {"qrtr", LogWifi},
  {"audio", LogAudio}, {"mic", LogAudio}, {"amps", LogAudio}, {"adsp", LogAudio},
  {"touch", LogInput}, {"hid", LogInput},
  {"battery", LogPower}, {"charger", LogPower}, {"clocks", LogPower}, {"cpu", LogPower},
};
static unsigned log_tag_group(const char *tag) {
  for (unsigned i = 0; i < sizeof log_tags / sizeof log_tags[0]; ++i)
    if (!strncmp(tag, log_tags[i].prefix, strlen(log_tags[i].prefix))) return log_tags[i].group;
  return LogSys;
}
static void log_groups_read(void) {
  log_group_on = 0;
  for (unsigned g = 0; g < LogGroups; ++g) {uint32_t v = 0; if (PE_parse_boot_argn(log_group_arg[g], &v, sizeof v) && v == 1) log_group_on |= 1u << g;}
  log_gpu_on = !!(log_group_on & (1u << LogGpu));
  log_display_on = !!(log_group_on & (1u << LogDisplay));
}
// Drops, in place, the sections of the groups that are off; returns the new length.
static size_t log_groups_filter(char *text, size_t length) {
  size_t in = 0, out = 0;
  int keep = 1;
  while (in < length) {
    size_t end = in; while (end < length && text[end] != '\n') ++end;
    const size_t line = end - in + (end < length ? 1 : 0);
    if (text[in] == '[') {
      keep = !strncmp(text + in, "[panic]", 7) || (log_group_on >> log_tag_group(text + in + 1)) & 1;
    } else if (text[in] == '#') keep = 1;
    if (keep) {if (out != in) memmove(text + out, text + in, line); out += line;}
    in += line;
  }
  if (out < length) memset(text + out, 0, length - out);
  return out;
}

static void writer(void *arg, wait_result_t result) {
  (void)arg; (void)result;
  thread_set_thread_name(current_thread(), "boot_log");
  uint64_t first = 0, blocks = 0;
  const char *why = "";
  boot_log_state = 1;
  // The window exists once NativeUfs has read the root layout; until then the open is refused ("no root layout").
  while (!native_ufs_log_open(&first, &blocks, &why)) {
    if (strcmp(why, "no root layout")) {boot_log_open_refusal = why; boot_log_state = 3; return;}
    IOSleep(1000);
  }
  boot_log_first_lba = first;
  const uint32_t slot_count = (uint32_t)((blocks - LogSlotFirst) / LogSlotBlocks);
  if (&native_ufs_export_blocks && &native_ufs_export_write) {
    IOLockLock(export_lock);
    export_region = (uint32_t)native_ufs_export_blocks();
    IOLockUnlock(export_lock);
  }
  static struct boot_log_header h;
  memset(&h, 0, sizeof h);
  memcpy(h.magic, "SHLOG001", 8);
  h.version = 5; h.header_bytes = sizeof h;
  h.export_blocks = export_region; h.export_below = export_region; h.boot_id = log_ticks(); h.first_lba = first; h.window_blocks = (uint32_t)blocks;
  h.msg_first = LogMsgFirst; h.msg_blocks = LogMsgBlocks; h.slot_first = LogSlotFirst; h.slot_blocks = LogSlotBlocks; h.slot_count = slot_count;
  h.gpt_open = h.gpt_last = &native_ufs_store_gpt_check ? native_ufs_store_gpt_check() : 0xff;
  if (&boot_progress_tag) strlcpy(h.tag, boot_progress_tag(), sizeof h.tag);
  uint64_t f = 0; __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f)); if (!f) f = 19200000;
  const uint64_t start = log_ticks();
  // The reboot-into-fastboot path, read only: PSCI features and the PMIC cell. autoreset=<seconds of log time> then writes the cell and
  // restarts once, after that round's log is on the medium, but only when the probe found the cell writable.
  uint32_t autoreset = 0;
  int reset_ok = 0, reset_tried = 0;
  uint32_t reset_held = 0, reset_tried_at = 0;
  if (&boot_reset_probe) {
    reset_ok = boot_reset_probe(boot_log_reset_probe_text, sizeof boot_log_reset_probe_text) == 0;
    PE_parse_boot_argn("autoreset", &autoreset, sizeof autoreset);
  }
  // diag=1 turns every diagnostic on: the kernel's rings and samplers (started from boot_diag_shot_tick), the scene screenshots and
  // the export of the diagnostic files, once, 25 s before autoreset (diag-export=<seconds of log time> moves it). Without it
  // this writer keeps the log rows only.
  uint32_t diag = 0, diag_at = 0, gpu_trace = 0;
  PE_parse_boot_argn("gpu-trace", &gpu_trace, sizeof gpu_trace);
  PE_parse_boot_argn("diag", &diag, sizeof diag);
  if (gpu_trace == 1) diag = 1;
  if (diag == 1 && &boot_diag_scan && &boot_diag_stream && !PE_parse_boot_argn("diag-export", &diag_at, sizeof diag_at))
    diag_at = autoreset > 25 ? autoreset - 25 : gpu_trace == 1 ? 180 : 0;
  if (diag != 1) diag_at = 0;
  uint32_t errors = 0;
  boot_log_live = &h;
  boot_log_state = 2;
  for (;;) {
    const unsigned now = (unsigned)((log_ticks() - start) / f);
    if (&boot_cache_loader_log_capture) boot_cache_loader_log_capture(now);
    int ok = 1;
    if (boot_log_panicked) return;   // the panic flush owns the window from now on
    // Text slot, then the message buffer, then the header that makes both visible.
    memset(boot_log_buffer, 0, LogSlotBlocks * LogBlock);
    size_t text;
    if(diag==1)text=(size_t)snprintf((char *)boot_log_buffer,LogSlotBlocks*LogBlock,
      "#slot %u t=%us tag=%s latency-issues=7 export-at=%us\n",h.slots_written,now,
      &boot_progress_tag?boot_progress_tag():"",diag_at);
    else text = log_groups_filter((char *)boot_log_buffer, compose((char *)boot_log_buffer, LogSlotBlocks * LogBlock, h.slots_written, now));
    // The used blocks and the one holding the terminating NUL; the slot's later blocks keep an older text behind that NUL.
    uint32_t slot_used = (uint32_t)(text / LogBlock + 1);
    if (slot_used > LogSlotBlocks) slot_used = LogSlotBlocks;
    ok &= native_ufs_log_write(LogSlotFirst + (uint64_t)(h.slots_written % slot_count) * LogSlotBlocks, slot_used, boot_log_buffer);
    if (diag == 1 || !boot_log_msg_ring) {
      // diag=1 keeps only the latency lines, so its message area stays linear and is written whole.
      h.msg_bytes = copy_msgbuf(boot_log_buffer);
      if(diag==1)h.msg_bytes=bl_latency_messages((char *)boot_log_buffer,h.msg_bytes);
      ok &= native_ufs_log_write(LogMsgFirst, LogMsgBlocks, boot_log_buffer);
      h.msg_mode = 0;
    } else {
      msg_stream_round(&h.msg_resyncs);
      ok &= msg_stream_write();
      // The header names only what is on the medium.
      const uint64_t cap = (uint64_t)LogMsgBlocks * LogBlock, head = msg_stream.written;
      h.msg_mode = 1; h.msg_head_lo = (uint32_t)head; h.msg_head_hi = (uint32_t)(head >> 32);
      h.msg_bytes = (uint32_t)(head < cap ? head : cap);
    }
    if (ok) ++h.slots_written;
    else ++h.write_errors;
    // The newest complete export is already on the medium; the header names it.
    IOLockLock(export_lock);
    h.export_generation = export_ready; h.export_bytes = export_ready_bytes; h.export_first = export_ready_first;
    h.diag_first = diag_first; h.diag_bytes = diag_bytes;
    IOLockUnlock(export_lock);
    if (diag == 1 && &boot_diag_shot_tick) boot_diag_shot_tick(now);
    if (((diag_at && now >= diag_at) || (&boot_cache_input_ready && boot_cache_input_ready())) && !boot_diag_status) {
      thread_t diag;
      boot_diag_status = 1;
      if (kernel_thread_start_priority(diag_thread, NULL, MINPRI_KERNEL, &diag) == KERN_SUCCESS) thread_deallocate(diag);
      else boot_diag_status = -10;
    }
    h.uptime_s = now; h.rounds = ++boot_log_rounds; h.state = boot_log_state;
    // Every 30 rounds the GPT is read back; any change stops the writer for good.
    if (&native_ufs_store_gpt_check && !(h.rounds % 30)) h.gpt_last = native_ufs_store_gpt_check();
    if (boot_log_panicked) return;
    memset(boot_log_buffer, 0, LogBlock);
    memcpy(boot_log_buffer, &h, sizeof h);
    ok &= native_ufs_log_write(0, 1, boot_log_buffer);
    ok &= native_ufs_log_sync();
    if (!ok) {boot_log_errors = ++errors;} else errors = 0;
    if (errors >= LogMaxErrors) {boot_log_state = 3; return;}
    if (h.gpt_last != h.gpt_open) {boot_log_state = 4; return;}
    if (autoreset && now >= autoreset && !reset_tried && !ok) ++reset_held;
    snprintf(boot_log_autoreset_text, sizeof boot_log_autoreset_text, "armed=%us probe_ok=%d tried=%d tried_at=%us held_by_write_errors=%u diag_export_at=%us now=%us",
             autoreset, reset_ok, reset_tried, reset_tried_at, reset_held, diag_at, now);
    if (autoreset && now >= autoreset && !reset_tried && ok) {
      reset_tried = 1; reset_tried_at = now;
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

// Called by the panic screen (scripts/xnu-panic-screen.c) once the message is in boot_progress_panic_text, with interrupts masked and the
// other CPUs still running: the message buffer with the panic text appended, then a header (state 5) that makes it visible, written
// through NativeUfs's panic path. Only the first call writes; a panic before the writer opened its window writes nothing.
void boot_log_panic_flush(void) {
  static volatile int done;
  struct boot_log_header *live = boot_log_live;
  if (done || !live || !boot_log_panic_buffer || !&native_ufs_log_panic || !native_ufs_log_panic()) return;
  done = 1;
  boot_log_panicked = 1;
  uint32_t n = copy_msgbuf(boot_log_panic_buffer);
  const uint32_t cap = LogMsgBlocks * LogBlock;
  if (&boot_progress_panic_text && boot_progress_panic_text[0]) {
    uint32_t len = (uint32_t)strnlen(boot_progress_panic_text, 2048);
    if (n > cap - len - 10) {   // a full buffer gives up its oldest lines to the panic text
      const uint32_t drop = n - (cap - len - 10);
      memmove(boot_log_panic_buffer, boot_log_panic_buffer + drop, n - drop);
      n -= drop;
    }
    memcpy(boot_log_panic_buffer + n, "\n[panic] ", 9);
    memcpy(boot_log_panic_buffer + n + 9, boot_progress_panic_text, len);
    n += 9 + len;
    boot_log_panic_buffer[n++] = '\n';
  }
  struct boot_log_header h = *live;
  h.msg_bytes = n; h.msg_mode = 0; h.rounds++; h.state = 5;   // the linear form, whole area
  native_ufs_log_write(LogMsgFirst, LogMsgBlocks, boot_log_panic_buffer);
  memset(boot_log_panic_buffer, 0, LogBlock);
  memcpy(boot_log_panic_buffer, &h, sizeof h);
  native_ufs_log_write(0, 1, boot_log_panic_buffer);
  native_ufs_log_sync();
}

// Called by the board page's monitor once a second; starts the writer the first time when log=1.
void boot_log_tick(unsigned now) {
  (void)now;
  if (boot_log_started) return;
  boot_log_started = 1;
  // An unattended run must come back however it ends: on the desktop past its deadline (int-229 first suspected) or in a panic seconds
  // after the start, before UFS and this log exist (int-229: a duplicate class at PE_lockdown_iokit). So the first tick, independent of
  // log, marks the cell (any restart is fastboot) and arms the hypervisor's watchdog, which bites a minute after the pings stop:
  // after the autoreset deadline, or when a panic parks the cores.
  uint32_t autoreset = 0;
  if (PE_parse_boot_argn("autoreset", &autoreset, sizeof autoreset) && autoreset && &boot_reset_probe &&
      boot_reset_probe(boot_log_reset_probe_text, sizeof boot_log_reset_probe_text) == 0) {
    if (&boot_reset_mark_bootloader) boot_reset_mark_bootloader(boot_log_reset_mark, sizeof boot_log_reset_mark);
    if (&boot_reset_watchdog) boot_reset_watchdog(autoreset + 60, boot_log_reset_watchdog, sizeof boot_log_reset_watchdog);
  }
  uint32_t mode = 0;
  if (!PE_parse_boot_argn("log", &mode, sizeof mode) || mode != 1) return;
  log_groups_read();
  if (!&native_ufs_log_open || !&native_ufs_log_write || !&native_ufs_log_sync) return;
  boot_log_buffer = (uint8_t *)IOMallocAligned(LogMsgBlocks * LogBlock, LogBlock);
  if (!boot_log_buffer) return;
  boot_log_panic_buffer = (uint8_t *)IOMallocAligned(LogMsgBlocks * LogBlock, LogBlock);
  // Without the ring the writer falls back to the linear message area, written whole every round.
  boot_log_msg_ring = (uint8_t *)IOMallocAligned(LogMsgBlocks * LogBlock, LogBlock);
  if (boot_log_msg_ring) memset(boot_log_msg_ring, 0, LogMsgBlocks * LogBlock);
  prof_snap = (uint64_t *)IOMalloc(1024 * sizeof(uint64_t));
  prof_rank = (uint64_t *)IOMalloc(2048 * sizeof(uint64_t));
  compose_spawn = (char *)IOMalloc(16384);
  export_lock = IOLockAlloc();
#if defined(NATIVE_BOOT_PROCESS_CAPTURE) && NATIVE_BOOT_PROCESS_CAPTURE
  uint32_t diag = 0;
  if (PE_parse_boot_argn("diag", &diag, sizeof diag) && diag == 1) boot_process_start();
#endif
  thread_t thread;
  if (kernel_thread_start_priority(writer, 0, BASEPRI_KERNEL, &thread) == KERN_SUCCESS) thread_deallocate(thread);
}
