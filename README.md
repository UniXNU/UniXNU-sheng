# UniXNU sheng

Device support for Xiaomi Pad 6S Pro 12.4, codename `sheng`, with Qualcomm
SM8550. The device profile selects
[UniXNU-sm8550-common](https://github.com/UniXNU/UniXNU-sm8550-common) and the
`ipados-17.7.11-21H461` OS profile in UniXNU.

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

The firmware must embed UniXNU, UniXNUDxe, DarwinBoot and the shared
`UniXNURamDiskDxe`, with DiskIoDxe and FAT support. These modules expose the
payload configuration and its boot entries. Follow
[firmware integration](https://unixnu.com/porting/uefi-firmware#required-integration).

Run these commands from the UniXNU checkout:

```sh
./unixnu defconfig sheng_defconfig --device ../UniXNU-sheng
./unixnu fetch --device ../UniXNU-sheng
./unixnu status --device ../UniXNU-sheng --json
./unixnu build kernel --device ../UniXNU-sheng --kernelcache /path/to/kernelcache
./unixnu build firmware --device ../UniXNU-sheng --uefi ../uefi-sheng
./unixnu build payload --device ../UniXNU-sheng --unique-chip-id "$UNIXNU_UNIQUE_CHIP_ID" \
  --data-ramdisk /path/to/data.dbr --trust-cache /path/to/trustcache
./unixnu bootimg --device ../UniXNU-sheng --uefi ../uefi-sheng --dtb /path/to/firmware.dtb
```

Outputs are under `out/sheng/ipados-17.7.11-21H461/sheng/`.
The payload step compiles the hardware DTS and writes `kernel.macho.adt`.
The boot image takes the firmware's FDT with `--dtb`.

Build inputs include the selected kernelcache, verified root policies,
vendor firmware, generated GPU register and power headers, UEFI source,
and the Data and trust-cache inputs used by the payload. Keep binary
inputs in local caches. `scripts/data/adreno-inputs.json` in UniXNU records
GPU firmware pins.

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

## Local build inputs

Save device paths and boot arguments in `unixnu.local.toml` in the UniXNU
checkout. See the [configuration reference](https://unixnu.com/reference/configuration#local-build-inputs).
Then use `./unixnu kernel`, `./unixnu firmware`, `./unixnu payload`, and
`./unixnu bootimg`. Argument changes require rebuilding the payload and boot
image. See [boot arguments](https://unixnu.com/xnu/boot-arguments#apply-arguments)
and [green-bar diagnosis](https://unixnu.com/guide/debugging#green-bar).
