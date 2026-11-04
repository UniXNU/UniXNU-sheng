# iPadOS 27 Bluetooth native routing

The QcaBluetooth source at `c177c19930d07b8f99aa1547eff83c958d3b9995` provides
an offline vendor-routing stage for the original 24A446 arm64e `bluetoothd`.
Input SHA-256:
`cfd94854d0a2efd3ca4a2de6e55349f43fd0d274a4b8bfa9937f29d9f7208cb1`.

From the component checkout:

```sh
out/native-venv/bin/python tools/test-native.py "$BLUETOOTHD"
python3 tools/patch-ios27.py "$BLUETOOTHD" --output out/ios27/bluetoothd.vendor-stage
```

The host check passes 76 instruction cases at two ASLR slides. It covers type
zero registration, preserved nonzero range checks, vendor table and hook clearing,
generic callback preservation, optional 0x0c5b selection and H4 pre-stop. PAC operations are
modeled without device keys; stack and memory instructions execute in Unicorn
2.1.4. The check rejects a changed binary hash and edits outside the declared
sites and empty executable tail.

H4 pre-stop skips the empty H4 extension; regular close and other transports
retain their native paths. The inspector records USERLAND24 chained fixups and ten native capability
slots. Capability publication and runtime installation remain in
development. The generated stage carries `adapter_ready=false`. Bluetooth
scanning, pairing, audio and power await device execution.
