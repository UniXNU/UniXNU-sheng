# iPadOS 27 Bluetooth startup

The runtime overlays reach stage 5, status 0. PID 1 exits with namespace 7,
subcode 1, `No service cache`. Automatic reset is disabled.

## Cache input

Capture `untagged-3858ddb02` belongs to image SHA-256
`dc172358b5c90b04a581370ad8f30191b95e85881fd7d34b6f19b3a449f8a506`,
board `b558633fe6127aba996d59877c7d7c563a2df832` and SM8550
`6f8acd4849c2e07c432bea33a4f3d8103a05e20a`. The log writer reports zero
write errors. The parser receives a complete readable binary plist whose
length and hash differ from the packed cache:

| Value | Packed cache | Device parser input |
| --- | --- | --- |
| Bytes | 1327012 | 1327067 |
| FNV-1a | 5a19e661b07cc7d9 | fcee8a414ee038a2 |
| Binary object count | 44478 | 44479 |
| Offset table | 1149068 | 1149119 |

The loader trusts code hash `c81688714f2abfb55a47015a07bb6208a8b5e10a`
and completes signature attachment and delivery. The Wi-Fi baseline cache
also reaches the same fatal cache path. The source of the extra 55 bytes is
unresolved.

The packed dictionary retains all 715 system jobs, adds the Wi-Fi supplicant
and changes four BlueTool fields: UserName root, ThrottleInterval 3,
EnablePressuredExit false and EnableTransactions true. Its other top-level
entries retain their original values. Plist lint and the host private XPC
parser accept the system, Wi-Fi and Bluetooth caches.

`sheng-cache-log=1` captures the loader log and, on the matched 24A446 failure
stack, the actual parser input. The snapshot is bounded to 4 MiB and published
once. The log writer exports launchd-parser-input.plist, the system-path cache,
the Data addon cache and the loader log to the existing reserved diagnostic
region. This cache-only export leaves GPU and latency capture disabled.
`check-cache-input.py` covers bounds, failed-copy teardown, immutable bytes,
one-shot publication and the fixed filesystem paths through ASan/UBSan.

## UART DMA

Private context-bank selection reaches register programming but returns EIO
at readback before UART startup. The board instead uses the firmware-managed
bypass context already shared by touch and audio. UART14 stream 0x423 receives
the 64-bit physical addresses of 32 KiB of DMA buffers. The shared SMMU lock
serializes route installation, and the existing touch route remains intact.
The board needs no private 16 KiB page tables or shared-context fault polling.

The shared-bank register fixture and arm64e compilation pass. The GENI model
already verifies 64-bit DMA pointer writes. Device UART, firmware, scan, pairing,
BLE, audio, power and Wi-Fi coexistence verification remains pending.

## Capture

From the workspace root, with the device in fastboot:

```sh
fastboot boot UniXNU/out/sheng/inputs/sheng-linux-log.img
python3 UniXNU-sheng/scripts/read-sheng-log.py --wait 50 --out /path/to/capture
```

The reader copies the reserved log and diagnostic regions without mounting
userdata, then returns to fastboot. Inspect msgbuf.txt, slots.txt and the
exported plist files before booting another system image. QcaBluetooth remains
a local checkout selected with `--local-module`.
