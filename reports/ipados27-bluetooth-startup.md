# iPadOS 27 Bluetooth startup

The runtime overlays reach stage 5, status 0. PID 1 exits with namespace 7,
subcode 1, `No service cache`. Automatic reset is disabled.

## Cache input

Capture `untagged-50547214e` belongs to image SHA-256
`5cc5c4bb58503acc0735a5b8034791c94dc5d3c2f504b116ffece66dc2b5fc0a`,
board `b378748c2e246b5f1c2f4fd36a6127d761b136e3` and SM8550
`d3ca65d508dd17b9689dbc69574792d4eec27f65`. The log writer reports zero
write errors. The parser receives a damaged binary plist whose
length and hash differ from the packed cache:

| Value | Packed cache | Device parser input |
| --- | --- | --- |
| Bytes | 1327012 | 1327067 |
| FNV-1a | 5a19e661b07cc7d9 | fcee8a414ee038a2 |
| Binary object count | 44478 | 44479 |
| Offset table | 1149068 | 1149119 |

The loader trusts code hash `c81688714f2abfb55a47015a07bb6208a8b5e10a`
and completes signature attachment and delivery. The Wi-Fi baseline cache
also reaches the same fatal cache path. The parser, system-path and Data-path copies are byte-identical. Each begins
with the complete packed cache and ends with 55 stale bytes, so its binary
plist trailer points at the old object table. All three copies fail parsing.
The addon writer now sets va_data_size with va_mode after each successful
write. This completes truncation when kernel vnode_open takes the non-compound
path. `check-addon-writer.py` reproduces the stale tail and verifies exact size,
unchanged-file skipping, growth, empty files and failed writes through
ASan/UBSan. Device verification of the replacement is pending.

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

The device configures UART14 stream 0x423 in bypass slot 5 and shared bank 69,
with physical DMA address 0x8ddb07000 and 32 KiB of buffers. UART protocol 2
(version 0x204) and QUP 0x30090000 are present. The version request ends in
EILSEQ before the first transmitted bytes, with one IRQ recorded.

The board waits for controller reset release and its settling delay before
arming RX. GENI clears latched reset-time status at start and retains the
original IRQ status only when reporting a fault. Live line and DMA errors
remain terminal. The GENI model covers stale status clearing, live faults and
64-bit DMA through ASan/UBSan; the board compiles for arm64e.

The firmware-managed bypass bank is shared with touch and audio under the
SMMU lock. It needs no private 16 KiB page tables or shared-context fault
polling. Native scan, pairing, BLE, audio, RX wake, power and Wi-Fi coexistence
verification remains pending.

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
