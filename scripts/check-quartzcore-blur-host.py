#!/usr/bin/env python3
"""Qualify unchanged target blur GPU output and three cold cache configurations.

The whole source library is hash-pinned. The authored descriptors cover named
pairs and two blur variants, not every UI constant/format/state combination.
All archives/caches/logs stay ignored. No physical device is contacted.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
KIT = ROOT / 'drivers/AdrenoMetal'
LIBRARY = ROOT / '.cache/firmware/target-volumes/os-mount/System/Library/Frameworks/QuartzCore.framework/default.metallib'
LIBRARY_SHA = '606f913adf520420f9b4bb666efb782a68c830f082f1e559e17b20d0d13adbc9'
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', type=Path, help='Fresh directory to preserve the exact controls; otherwise use a temporary directory')
args = parser.parse_args()


def require(value, reason):
    if not value:
        raise SystemExit(reason)


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


require(LIBRARY.is_file() and sha(LIBRARY) == LIBRARY_SHA, 'Exact supplied target QuartzCore library required')
require(shutil.disk_usage(ROOT).free > 5 * 1024**3 + 256 * 1024**2, 'Preserve 5 GiB and bounded build/cache allowance')
loader = os.environ.get('NMB_HOST_LOADER')
if not loader:
    brew = shutil.which('brew')
    require(brew, 'Set NMB_HOST_LOADER to an actual host Vulkan loader')
    prefix = subprocess.check_output([brew, '--prefix', 'molten-vk'], text=True, timeout=10).strip()
    loader = str(Path(prefix) / 'lib/libMoltenVK.dylib')
require(Path(loader).is_file(), 'Actual host Vulkan driver required')
out = ROOT / 'build/ui-shader-coverage'
out.mkdir(parents=True, exist_ok=True)
bridge = ROOT / 'build/adreno-metal-bridge/host/libadreno_metal_bridge.a'
require(bridge.is_file(), 'Build the current host bridge first')
flags = ['-std=c++17', '-fobjc-arc', '-O1', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-Wno-deprecated-declarations', '-I', KIT]
program = out / 'quartzcore_blur_host'
subprocess.run(['xcrun', 'clang++', *flags, KIT / 'NativeBridgeMetalFrontend.mm', KIT / 'bridge/quartzcore_blur_host.mm', bridge,
                '-framework', 'Foundation', '-framework', 'Metal', '-framework', 'IOSurface', '-framework', 'CoreFoundation',
                '-o', program], check=True, timeout=90)
sdk = subprocess.check_output(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], text=True, timeout=10).strip()
ios_object = out / 'quartzcore-blur-ios.o'
subprocess.run(['xcrun', 'clang++', '-target', 'arm64-apple-ios17.0', '-isysroot', sdk, *flags,
                '-c', KIT / 'bridge/quartzcore_blur_host.mm', '-o', ios_object], check=True, timeout=60)
require(struct.unpack_from('<3I', ios_object.read_bytes()) == (0xfeedfacf, 0x100000c, 0), 'Actual ordinary ARM64 object')
temporary = None
if args.output:
    work = args.output.resolve()
    require(not work.exists(), 'Preserve the existing cache control; select a fresh output directory')
    work.mkdir(parents=True)
else:
    temporary = tempfile.TemporaryDirectory(prefix='quartzcore-cache-', dir=out)
    work = Path(temporary.name)
archive = work / 'ui-archive.bin'
cases = {}
try:
    inventory = subprocess.run([str(program), loader, str(work / 'inventory-cache'), str(LIBRARY), str(archive), 'inventory'],
                               capture_output=True, text=True, timeout=180)
    (work / 'inventory.log').write_text(inventory.stdout + inventory.stderr)
    shutil.copyfile(work / 'inventory.log', out / 'last-inventory.log')
    require(inventory.returncode == 0 and 'ordinary=4 tile_requires=11 pairs=22 bare_ok=14 missing_vertex=6 missing_attachment=2' in inventory.stdout,
            'Exact bare-descriptor failures must have typed input/descriptor causes')
    for label, mode in [('producer', 'produce'), ('archive-only', 'consume'), ('provisioned', 'consume')]:
        cache = work / (label + '-cache')
        if label == 'provisioned':
            shutil.copytree(work / 'producer-cache', cache)
        run = subprocess.run([str(program), loader, str(cache), str(LIBRARY), str(archive), mode], capture_output=True,
                             text=True, timeout=180)
        log = work / (label + '.log')
        log.write_text(run.stdout + run.stderr)
        shutil.copyfile(log, out / ('last-' + label + '.log'))
        require(run.returncode == 0 and 'valid descriptor pairs=22 strict archive=pass' in run.stdout and
                'PASS: unchanged target blur functions' in run.stdout, 'Actual target blur/archive control failed: ' + label)
        rows = re.findall(r'^round (float|half) (\d+) vk_makings=(\d+) artifact_hit=(\d+) miss=(\d+) driver_load=(\d+) hit=(\d+) miss=(\d+)$', run.stdout, re.M)
        require(len(rows) == 12 and all(all(int(v) == 0 for v in r[2:]) for r in rows), 'Every repeated GPU draw must have no new pipeline/cache work')
        pixels = re.findall(r'^blur (float|half) rounds=6 passes=12 texels=768 max_error=([0-9.e+-]+) GPU=pass$', run.stdout, re.M)
        require([p[0] for p in pixels] == ['float', 'half'] and all(float(p[1]) <= .003 for p in pixels), 'Complete actual GPU/reference results')
        end = re.search(r'^cache finished artifact_hit=(\d+) miss=(\d+) write=(\d+) driver_load=(\d+) hit=(\d+) miss=(\d+) save=(\d+) same=(\d+) vk_makings=(\d+)$', run.stdout, re.M)
        require(end, 'Complete cumulative compiler/cache statistics')
        hits, misses, writes, loads, driver_hits, driver_misses, saves, same, makings = map(int, end.groups())
        require(makings == 24, 'Exactly 24 real logged pipeline creations; diagnostics must be enabled')
        if label == 'producer':
            require(misses > 0 and writes > 0 and driver_misses > 0 and saves > 0, 'Genuine cold producer compilation')
        elif label == 'archive-only':
            require(misses == writes == 0 and driver_misses > 0, 'Archive alone skips translation but still misses driver pipelines')
        else:
            require(misses == writes == driver_misses == 0 and loads == driver_hits == 24, 'Actual provisioned driver feedback hits with no translation/driver miss')
        cases[label] = dict(artifact_hits=hits, artifact_misses=misses, artifact_writes=writes, driver_loads=loads,
            driver_hits=driver_hits, driver_misses=driver_misses, driver_saves=saves, driver_unchanged=same,
            pipeline_creations=makings, repeated_draw_pipeline_creations=0, max_float_error=float(pixels[0][1]),
            max_half_error=float(pixels[1][1]), log_sha256=sha(log))
    require(sha(LIBRARY) == LIBRARY_SHA, 'Original target library remains unchanged')
    inputs = [KIT / 'NativeBridgeMetalFrontend.h', KIT / 'NativeBridgeMetalFrontend.mm', KIT / 'bridge/quartzcore_blur_host.mm',
              KIT / 'bridge/NativeMetalObjects.rs', KIT / 'bridge/NativeMetalObjects.h']
    report = dict(evidence='actual host GPU with unchanged target shaders; cold processes and real cache feedback',
        native_metal_base_revision=subprocess.check_output(['git', '-C', str(KIT), 'rev-parse', 'HEAD'], text=True).strip(),
        checker_sha256=sha(Path(__file__)),
        input_library_sha256=LIBRARY_SHA, source_sha256={str(p.relative_to(ROOT)): sha(p) for p in inputs},
        host_bridge_sha256=sha(bridge), process_sha256=sha(program), ios_object_sha256=sha(ios_object),
        host_driver_sha256=sha(Path(loader)), archive_sha256=sha(archive), archive_bytes=archive.stat().st_size,
        inventory_log_sha256=sha(work / 'inventory.log'), functions=167, vertex_functions=41, fragment_functions=111,
        kernel_functions=15, ordinary_compute_pipelines=4, tile_attachment_requirements=11,
        bare_pairs_success=14, bare_pairs_missing_vertex=6, bare_pairs_missing_attachments=2,
        compatible_named_render_pairs=22, additional_blur_variants=2, gpu_reference_texels=768,
        gaussian_rounds_per_precision_per_process=6, precision_variants=['float', 'half'], cases=cases,
        coefficient_reference=[1, 6, 15, 20, 15, 6, 1], normalization=64, tolerance=.003,
        tile_shaders_qualified=False, target_ui_descriptors_observed=False, complete_ui_variant_coverage=False,
        ios_shader_execution=False, adreno_execution=False, active_native_adreno_4k=False,
        hardware_timing_qualified=False, hardware_ready=False, stable_120_fps_qualified=False)
    (out / 'quartzcore-blur-checkpoint.json').write_text(json.dumps(report, indent=2) + '\n')
    if args.output:
        (work / 'checkpoint.json').write_text(json.dumps(report, indent=2) + '\n')
    print('PASS: 22 compatible target pairs, two original GPU blur variants and cold archive/provisioned-cache controls')
    print('Archive-only still misses the driver; same-host provisioned cache reports 24/24 hits, zero translation/driver misses')
    print('No complete UI variant pack, iOS/Adreno execution or stable 120 FPS is qualified')
finally:
    if temporary:
        temporary.cleanup()
