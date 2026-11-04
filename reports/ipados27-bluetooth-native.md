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

Component registration, the full kernel/image rebuild and device scanning,
pairing, audio and power verification are pending.
