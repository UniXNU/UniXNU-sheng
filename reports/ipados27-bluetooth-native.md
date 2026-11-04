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

Normal traffic stays in UART.SK and the native stack. Runtime installation,
executable signing and device scanning, pairing, audio and power verification
are pending.
