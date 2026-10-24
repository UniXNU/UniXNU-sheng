#!/usr/bin/env python3
"""Close out one bounded diagnostic release; never operates the device.

The scope is progress/timeout instrumentation plus retained upper-userland
regression evidence. It is not an all-hardware, GPU, or iPadOS readiness gate.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
BASE = ROOT / 'build/ufs-root-diagnostics/progress'
TRUST = '7416ca4f8377146626bb284e2133c76bee6cb4e3ee0af5d648b186235a7323a7'


def require(value, reason):
    if not value:
        raise ValueError(reason)


def load(path):
    return json.loads(path.read_text())


def sha(path):
    with path.open('rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--release', type=Path, default=BASE/'release.json')
    p.add_argument('--output', type=Path, default=BASE/'decision.json')
    args = p.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.unlink(missing_ok=True)
    r = load(args.release)
    for field in ('source_sha256', 'evidence_sha256'):
        for name, digest in r[field].items():
            require(sha(ROOT/name) == digest, 'Changed '+field+': '+name)
    require(sha(ROOT/r['candidate']) == r['candidate_sha256'], 'Candidate changed')
    require(sha(ROOT/r['image']) == r['image_sha256'], 'Boot image changed')
    require(subprocess.check_output(['git','-C',str(ROOT/'drivers/ShengUfs'),'rev-parse','HEAD'],text=True).strip()
            == r['native_ufs_commit'], 'UFS revision changed')
    package = load(BASE/'package-audit/report.json')
    for name in ('candidate_sha256','image_sha256','data_seed_sha256','embedded_kernel_sha256'):
        require(package[name] == r[name], 'Package mismatch: '+name)
    require(package['original_trust_cache_sha256'] == TRUST and
            package['patch_reproduction'] == 'byte-for-byte match', 'Original trust/renderer mismatch')
    require(package['boot_args'][-2:] == ['boot-progress=1','-apfs_shared_datavolume'], 'Missing diagnostic/Data arguments')
    require(load(BASE/'host/report.json')['passed'] and load(BASE/'code/report.json')['passed'], 'Host/code checks failed')
    build = load(ROOT/'build/boot-progress/build-report.json')
    require(build['source_sha256'] == sha(ROOT/'kits/sheng/BootProgress.c') and
            build['core_sha256'] == sha(ROOT/'kits/sheng/BootProgressCore.h'), 'Stale diagnostic build')
    for name, digest in load(BASE/'code/report.json')['objects'].items():
        require(sha(ROOT/name) == digest, 'Changed compiled object: '+name)
    results = {}
    for name in ('final-live','final-idle','final-limit','final-off','final-userland','final-userland-headless'):
        d = load(BASE/name/'gdb-stop.json'); results[name] = d
        require(d['kernel_sha256'] == r['candidate_sha256'] and d['native_4k_mmu_observed'], name+': candidate/native4K mismatch')
        require(d['trust_cache_bundle_sha256'] == TRUST, name+': changed trust cache')
        require(d['offline_qemu_vmm_present_declared'] == name.startswith('final-userland'), name+': VMM scope mismatch')
        require(d['qemu_accelerator'] == ('hvf' if name.startswith('final-userland') else 'tcg'), name+': accelerator mismatch')
        if name in ('final-live','final-idle','final-limit'):
            b = d['boot_progress']; reason = 1 if name == 'final-idle' else 2
            require(b['enabled'] == 1 and b['bound'] == 1 and b['frozen'] == 1 and b['reason'] == reason,
                    name+': missing actual frozen checkpoint')
            require(d['stop_symbol'] == '_boot_progress_stop_checkpoint', name+': wrong stop')
            if name == 'final-live':
                require(b['seconds'] >= 120 and b['irqs'] > 100 and b['monitor_ticks'] > 10 and b['verified'] > 1000 and
                        b['failures'] == 0 and b['log_lines'] > 10, 'Continuous real-counter run incomplete')
                require(not d.get('boot_progress_clock_fixture'), 'Real-counter run used a synthetic epoch')
            else:
                require(d.get('boot_progress_clock_fixture'), name+': clock fixture not disclosed')
        else:
            require(d['boot_progress']['enabled'] == 0, name+': diagnostic unexpectedly active')
        if name in ('final-live','final-off','final-userland','final-userland-headless'):
            require(d['owner_root_binding_audit']['boot_root_accepted'] and d['original_amfi_table_owner_observed'] and
                    d['native_virtio_integrity']['_native_virtio_integrity_failures'] == 0, name+': root/AMFI/integrity')
            require(d['scratch_data']['input']['sha256'] == r['data_seed_sha256'], name+': wrong Data fixture')
    entry = results['final-off']['followed_user_entry']
    require(entry['observed'] and int(entry['pstate'],16)&31 == 0, 'Original dyld EL0 missing with diagnostics off')
    # Record the actual deeper result, never require/claim a desktop from a process table.
    processes = results['final-userland'].get('process_list') or []
    names = {p['name'] for p in processes}
    rebuild = load(BASE/'rebuild-equivalence.json')
    require(rebuild['passed'] and rebuild['qualified_sha256'] == r['candidate_sha256'], 'Rebuild evidence mismatch')
    exception = load(BASE/'final-springboard-exception/gdb-stop.json')
    symbols = load(BASE/'springboard-exception-symbols.json')
    require(exception['kernel_sha256'] == rebuild['rebuilt_sha256'] and
            exception['stop_symbol'] == '_exception_triage' and not exception['bounded_timeout_interrupted'] and
            any(e['matched'] and e['process']['name'] == 'SpringBoard' for e in exception['entry_process_events']),
            'Missing actual SpringBoard exception')
    require(symbols['local_symbols_uuid_matches'] and symbols['actual_exception'] == 'EXC_BREAKPOINT' and
            symbols['static_original_assertion']['text'] == 'failed to find the main CADisplay' and
            not symbols['original_files_modified'], 'Exception attribution failed')
    old = load(ROOT/'build/xnu-prelink/native-owner-stage5-candidate.json')
    new = load(Path(str(ROOT/r['candidate']).replace('.macho','.json')))
    require(old['components'] == new['components'] and old['original_executable_sha256'] == new['original_executable_sha256'], 'Original executable components changed')
    pixels = load(BASE/'live-pixels.json')
    require(pixels['console_visible_after_rebind'] and pixels['console_tail_white_pixels'] > 1000 and
            pixels['final_console_tail_equals_live'] and pixels['final_console_tail_white_pixels'] > 1000,
            'Console erased by later initialization')
    data = load(BASE/'data-provision.json')
    require(data['original_file_symlink_inventory_unchanged'] and not data['added_keys_or_user_records'] and
            data['fixture_sha256'] == r['data_seed_sha256'], 'Data preparation changed source contents')
    for name, count in [('renderer-exec',10),('renderer-ufs',28)]:
        d = load(BASE/name/'report.json')
        require(len(d['matched_lines']) == count and d['renderer_bytes'] <= 2608 and
                not d['hardware_or_xnu_boot_qualified'], 'Panic screen incomplete')
    args.output.write_text(json.dumps(dict(
        diagnostic_batch_offline_complete=True, required_checks_remaining_in_this_batch=[],
        release_sha256=sha(args.release), image_sha256=r['image_sha256'],
        normal_counter_limit_seconds=results['final-live']['boot_progress']['seconds'],
        upper_userland_process_count=len(processes),
        headless_userland_process_count=len(results['final-userland-headless'].get('process_list') or []), springboard_alive_at_final_sample='SpringBoard' in names,
        springboard_execution_observed_in_forensic_run=True,
        springboard_display_initialization_exception='failed to find the main CADisplay',
        upper_userland_uses_explicit_offline_vmm=True,
        hardware_tested=False, boot_sent=False, device_action_authorized=False,
        cpu_hardware_4k_qualified=False, gpu_hardware_4k_qualified=False,
        functional_ipados_ui_qualified=False, all_hardware_checks_complete=False,
        limitation='IRQ-off/CPU hard lock can stop software refresh and timeout; physical UFS/GPU/display require hardware evidence.'
    ), indent=2)+'\n')
    print('PASS: bounded progress release qualified offline; no device operation or complete-iPadOS claim')


if __name__ == '__main__':
    main()
