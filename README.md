# UniXNU sheng

Device support for Xiaomi Pad 6S Pro 12.4, codename `sheng`, with Qualcomm
SM8550. The device profile selects
[UniXNU-sm8550-common](https://github.com/UniXNU/UniXNU-sm8550-common). One
branch builds every OS profile; `--os 17|26|27` picks the target (the 27
profile is the one under active maintenance).

## Structure

| Path | Contents |
| --- | --- |
| `porting.toml` | Subsystem inputs, findings, evidence and completion criteria |
| `board.toml` | Device configuration and component selection |
| `feeds.toml` | Pinned SoC source |
| `configs/sheng_defconfig` | Device kernel settings |
| `dts/sheng.dts` | Memory, CPU selection, and peripheral configuration |
| `patches/xnu/series` | Board patches |
| `proprietary-files.txt` | Firmware input paths |
| `package/` | Device policy, display, input, and diagnostics |
| `scripts/` | Device build and inspection tools |

`Adreno740` and `AdrenoMetal` provide the GPU implementation. Shared GMU,
command database, and GPU SMMU operations are in the SoC repository's
`Sm8550Platform` kit. sheng supplies GPU votes, signed ZAP firmware,
reserved memory, and display integration. GPU initialization uses the
staged platform build. The hardware DTS contains GPU resources; the ADT
exports nodes with `unixnu,adt-name`.

`ShengUfs` includes the device's storage and persistence policy.
`ShengHidInput` and `ShengNovatekTouch` provide the input integration.
`QcomGeniSpi` provides the shared SPI controller interface.
`Ath12kWifi` supplies the Wi-Fi stack through board PCIe, MSI, DMA,
and network interfaces. The payload volume is 208 MiB. The device selects
sRGB display properties and a 120 Hz display timing request.

## Development record

Maintain [porting.toml](porting.toml) with every subsystem change. It records
hardware, configuration, procedures, findings, evidence and completion criteria.
The [device page](https://unixnu.com/devices/sheng) renders this record.
Maintainers and agents follow the [same guide](https://unixnu.com/porting/records).
The required `Porting record` CI check validates structure, component coverage
and updates to findings or evidence for changed code.

This repository owns the ShengUfs, ShengHidInput, ShengNovatekTouch and
ShengPreflight submodules. Shared components come from the selected profiles
and UniXNU catalog. Read the [UFS guide](https://unixnu.com/porting/ufs) before
changing storage resources or policy.

## Build

One command from this repository builds everything: dependencies, preflight,
patches, kernel, package, firmware, payload, boot image and the manifest.

```sh
UNIXNU_UNIQUE_CHIP_ID=<8 hex digits> scripts/package.sh --os 27 --uefi ../uefi-sheng
```

- The firmware inputs come from the official fastboot archive pinned in
  `board.toml` `[firmware.image]`: the preflight downloads it with concurrent
  HTTP ranges, keeps only the porting inputs, deletes the archive and
  generates the `Binaries/sheng` modules the firmware volume builds from.
  Extracted firmware lives in no repository.
- `UNIXNU_UNIQUE_CHIP_ID` is the platform seed of the device, 8 hex digits.
  It is baked into the images and stays out of every repository.
- The kernelcache, trust caches and OS volumes come from the IPSW the OS
  profile pins; the generated GPU headers come from the pinned sources the
  kernel archives fetch.
- Outputs land under `out/sheng/<profile>/<config>/` of the UniXNU checkout:
  `boot/boot.img`, `payload/payload.img`, `kernel/kernel.macho` and
  `package.manifest.json`.
- `--check` stops after deps, preflight and patches; `--no-deps` skips
  fetching on a machine whose sources are already pinned.

See the [packaging script](https://unixnu.com/porting/packaging) page for the
fixed steps, the board.toml keys and the verification the run performs.

## Build on GitHub

`.github/workflows/package.yml` runs the same script on a macOS runner and
offers the images as the run's artifacts.

1. Fork this repository and set its `UNIXNU_UNIQUE_CHIP_ID` secret to the
   platform seed of the device.
2. Actions, package, Run workflow, pick the OS profile.
3. Download the run's artifacts, then delete the run. The images serve the one
   device whose seed they bake; they are not kept.

The run installs and asserts its own tools; nothing else is needed on the
runner.

## Documentation

The documentation site is maintained in UniXNU:

- [sheng device guide](https://unixnu.com/devices/sheng)
- [Building](https://unixnu.com/building)
- [Device repository specification](https://unixnu.com/porting/device-repository)
- [Device-tree specification](https://unixnu.com/porting/device-tree)
- [Component catalog](https://unixnu.com/reference/components)
- [Migration](https://unixnu.com/porting/migration)

Report build results and device boot, storage, display, input, and GPU
observations separately, with source revisions and commands.
