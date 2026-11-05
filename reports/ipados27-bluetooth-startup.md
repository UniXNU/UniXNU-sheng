# iPadOS 27 Bluetooth startup

The device log is `untagged-55f92f8343`, captured through the read-only Linux
log reader. The temporary boot image SHA-256 is
`d94ce87446e5942f72762b2df9417cd1f66240ff124a1382104f4deac734cf4d`.
It contains board `8f2af527c7204b07535b9e015a53c05d8029c0ee` and
QcaBluetooth `442ea7da0004c6265f325c676f69eb315e6cc720`.

## Observations

- The shared launchd cache and Wi-Fi executable overlays reach stage 5, status 0.
- The Bluetooth library and executable overlays both reach stage 5, status 0.
- The cache loader queries its code hash through AMFI; the call returns IOReturn 0.
  This records a completed query; its log does not expose the returned trust boolean.
- PID 1 exits with namespace 7, subcode 1, `No service cache`.
- Bluetooth startup returns `-28` at 115200 baud. The source maps this to ENOSPC
  from SMMU stream/context/ASID selection before UART or firmware initialization.
- The log writer reports no write errors. Automatic reset is disabled.

The cache is accepted by the host plist and XPC parsers. Device acceptance is
unresolved. The next diagnostic boot enables `sheng-cache-log=1` and records the
loader's own file with the log writer's relative boot time. Failed Bluetooth
attachment emits SMMU IDs, stream capacity and eight disabled-bank summaries.
These rows distinguish stream-slot, context-bank and ASID exhaustion.

## Capture

From the workspace root, with the device in fastboot:

```sh
fastboot boot UniXNU/out/sheng/inputs/sheng-linux-log.img
python3 UniXNU-sheng/scripts/read-sheng-log.py --wait 50 --out /path/to/capture
```

The reader copies the reserved log region without mounting or writing userdata,
then returns to fastboot. Inspect `msgbuf.txt` and `latest.txt` in the captured
boot directory before booting another system image.

QcaBluetooth remains a local checkout selected with `--local-module`.
Scanning, pairing, BLE, audio, power and Wi-Fi coexistence measurements follow
startup repair.
