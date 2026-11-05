# iPadOS 27 Bluetooth startup

Device startup reaches the Wi-Fi cache and Bluetooth runtime overlays at
stage 5, status 0. PID 1 exits with namespace 7, subcode 1, `No service cache`.
Bluetooth DMA attachment reports context-bank ENOSPC before UART startup.
Automatic reset is disabled.

## Launchd cache

The packed cache retains all 715 system jobs and adds the Wi-Fi supplicant.
Only BlueTool changes: UserName is root, ThrottleInterval is 3,
EnablePressuredExit is false and EnableTransactions is true. The six top-level
entries retain their original values outside LaunchDaemons. Plist lint and the
host private XPC parser accept the system, Wi-Fi and Bluetooth caches.

The device also rejects the unchanged Wi-Fi baseline cache with BlueTool's
original job. Its loader reports trusted code hash
`8016fc5545e8f80fa8c90f529f68a71b66645c3e`. The loader completes signature
attachment and sends the validated cache to launchd. Device parser acceptance
remains unresolved.

Capture `untagged-116dc15ab` records this result with image SHA-256
`bdf756b74064408877eb353db8d1fdefb860fe3657f4462d14359fac46e1bedf`,
board `46fcac0c81c35b822d07dbc4f374547101ff5f5c` and SM8550
`276bb6e16bbc6f5505776e62ecd5172d54c32ce9`. Its log writer reports zero
write errors. The baseline image changes only the cache and its signature.

`sheng-cache-log=1` captures the loader's file using the log writer's relative
boot clock. On the matched 24A446 failure stack it also records launchd's XPC
data length, head, trailer, complete FNV-1a hash and string-cache metadata.
Bounded copyin reads compare the actual parser input with the packed file.
The Bluetooth cache is 1327012 bytes with FNV-1a `5a19e661b07cc7d9`;
the Wi-Fi baseline is 1327003 bytes with FNV-1a `3bcfff9d8deafc9c`.

## SMMU allocation

The device reports 98 free stream slots, 66 unrouted context banks and four
banks excluded by existing configuration. Selection fails at the context-bank
stage. Failure rows reuse the allocator's original reads without rescanning
MMIO. They include SCTLR and fault status.

The allocator accepts an unrouted, unconfigured bank with SCTLR.M clear even
when other SCTLR reset bits remain set. Register simulation covers this state
and skips a bank with M set. ASan/UBSan and arm64e compilation pass; the corrected
selection awaits device verification.

## Capture

From the workspace root, with the device in fastboot:

```sh
fastboot boot UniXNU/out/sheng/inputs/sheng-linux-log.img
python3 UniXNU-sheng/scripts/read-sheng-log.py --wait 50 --out /path/to/capture
```

The reader copies the reserved log region without mounting or writing userdata,
then returns to fastboot. Inspect msgbuf.txt and slots.txt before booting another
system image. QcaBluetooth is selected from the local checkout with
`--local-module`. Native scanning, pairing, BLE, audio, power and Wi-Fi
coexistence measurements follow successful startup.
