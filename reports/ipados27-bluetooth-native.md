# iPadOS 27 Bluetooth native adapter

QcaBluetooth source `97f48bf20bd47f233681668d50e5f5638e72384b` supplies the native controller
adapter for the original 24A446 arm64e `bluetoothd`. Input SHA-256:
`cfd94854d0a2efd3ca4a2de6e55349f43fd0d274a4b8bfa9937f29d9f7208cb1`.

From the component checkout:

```sh
out/native-venv/bin/python tools/test-native.py "$BLUETOOTHD"
python3 tools/test-type0.py "$BLUETOOTHD"
python3 tools/build-type0.py
python3 tools/patch-ios27.py "$BLUETOOTHD" --output out/ios27/bluetoothd.native
```

The host check passes 144 instruction cases at two ASLR slides, covering native
routing, LE feature capture, capability getters, lower setters, authenticated
call boundaries and H4 pre-stop. PAC operations are modeled without device keys.
The production publisher passes 58 ASan/UBSan HCI and policy cases. The arm64e
capability library compiles and passes code-signature verification.

Capabilities follow real command and LE feature reports. An asynchronous 0x203b
query provides advertising capacity, capped at six native records. Eleven model
slots, configuration caches and lower flags agree, including coded and 2M PHY.
Broadcaster mode is refreshed before GAP initialization. Original PACIBSP entry
pads and USERLAND24 signing metadata remain intact.

The native runtime is built by QcaBluetooth `e24972d027ad0d503ce89b01cc434a0c9130e451` and
board integration `1258c87da0cdbb0a25f59b030cd48997c2793548`. It contains signed BlueTool,
bluetoothd and QcaBTType0.dylib plus hash-checked firmware. Read-only unions expose
them under /usr/sbin and /usr/lib, with the library mounted first. The original
bluetoothd identifier and entitlements are preserved. BlueTool keeps its service
name and runs as root with transactions enabled and pressured exit disabled.
An already powered-off controller completes the sleep request successfully.

From the workspace root:

```sh
python3 UniXNU-sheng/scripts/check-bluetooth-runtime.py \
  --runtime /path/to/generated/runtime --os-root /path/to/original/os \
  --seed-record /path/to/preserved/runtime/inputs.json
```

The actual combined addon passes verification for 45 packed files,
three signed arm64e Bluetooth binaries, 716 launchd jobs,
12 unchanged input trust modules and
the preserved Data seed. The host libxpc reader accepts the merged cache.
The native bluetoothd job remains unchanged; the existing Wi-Fi owner job stays
present. Firmware matches the board manifest.

The exported Bluetooth node supplies H4 UART.SK encoding and 3.2 Mbaud as
little-endian properties. A matching IOService publishes them with the stable
private address before userland starts. The Linux boundary converts that public
address to bdaddr_t order. Protocol simulation and target source compilation
pass. The executable/library mount patch applies and kern_exec.c compiles for
arm64e iPadOS 27. Normal traffic stays in UART.SK and the native stack.

## Complete image

The local build uses UniXNU `f3c3092156d0410b0976a4d3e1202388ca1e719a`,
board `8f2af527c7204b07535b9e015a53c05d8029c0ee`,
SM8550 `1ebec150cd2d8f7bb7fe9d75c72c00479a791750` and
QcaBluetooth `442ea7da0004c6265f325c676f69eb315e6cc720`.
The board startup entry has C linkage. Archive validation covers the Bluetooth
board, core, shim and SoC source dependencies. Clock-restore failures terminate
all three wake paths with the original hardware error and stop forwarding and
retries. The protocol sanitizer check and target compile pass.

The complete native kernel links all nine checked Bluetooth exports. Fileset
prelink passes the existing regression policy. The 176 MiB FAT payload and
185733120-byte temporary boot image build successfully. Boot-image unpacking
matches its firmware, DTB and payload inputs. SHA-256:

| Artifact | SHA-256 |
| --- | --- |
| Prelinked kernel | `0439e3b2d6d96c69ab7307cf574c636b948b5ee040425f3c2d4ba9b8fece1c52` |
| FAT payload | `4b4cda53eb0cd30fc9fced41b40712b8a8d29b204005c3c0d2638e3ab3de071b` |
| Temporary boot image | `d94ce87446e5942f72762b2df9417cd1f66240ff124a1382104f4deac734cf4d` |

Direct FAT extraction verifies six files against their build hashes, the signed
runtime/trust inputs, preserved Data seed, H4/3.2 Mbaud properties and the enabled
Bluetooth boot argument. The payload kernel retains all nine exports. The panic
screen is enabled and the entry probe has no automatic reset.

From the UniXNU checkout, use the installation's preserved local inputs:

```sh
./unixnu build kernel payload -D ../UniXNU-sheng --os 27 \
  --config /path/to/installation-inputs.toml \
  --local-module driver:QcaBluetooth=/path/to/QcaBluetooth
./unixnu bootimg -D ../UniXNU-sheng --os 27 \
  --config /path/to/installation-inputs.toml --dtb /path/to/sheng.dtb \
  --ramdisk /path/to/payload.img -o /path/to/boot-bluetooth.img
```

The build uses the explicit local component before catalog publication. Device
scanning, pairing, reconnection, BLE, audio, idle power and Wi-Fi coexistence
measurements are pending.
