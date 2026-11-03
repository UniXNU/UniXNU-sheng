# iPadOS 27 Wi-Fi implementation

sheng on iPadOS 27.0.1 (24A446) uses Linux ath12k/mac80211/cfg80211 with
wpa_supplicant 2.12. Board code supplies hardware operations and Apple80211;
the kernel handles packets and the userspace service handles authentication.

Reference sources:

- https://github.com/ianchb/sm8550-mainline
- https://github.com/ianchb/debian-sheng

## Current implementation

| Area | Implementation |
| --- | --- |
| Counters | DMA, packet copies, reuse, RX batches, worker activity and TX release reasons |
| Sources | Pinned Linux v7.0.14 wireless stack, matching headers and supplicant 2.12 |
| Station | Production cfg80211/mac80211 lifecycle, association, aggregation and reordering |
| Authentication | Userspace original SME/WPA, PMF, four-way handshake and rekey |
| Buffers | Shared DMA storage, copy-on-write, bounded reuse and direct mbuf/DMA copies |
| Transmit | mac80211 FQ/AQL, direct queue entry, bounded busy fallback and completion progress |
| Receive | Original A-MSDU split, nonlinear input and batches of up to 64 mbufs |
| Interrupts | MSI vector delivery, deadline-based sleep and bounded work/NAPI slices |
| Integration | en0/Apple80211, signed launchd service/cache, trust bundle and Data firmware |

The native control endpoint is `org.unixnu.wifi.control`. Its owner IDs and
connection generations match key, authorization and EAPOL events to active
requests. Open and WPA2-PSK networks with PMF are supported. Profiles live in
RAM; the OS supplies saved credentials and network selection.

## Connection state

WifiNet accepts 592-byte and 656-byte association requests. The 24A446
asynchronous result has 540 initialized bytes; result and validity are at
0x214/0x218. Success follows port authorization. Each accepted request has
a 30-second deadline; cancellation completes its pending result once.
Disconnect and power-off clear visible state and publish link-down events.

Association RSSI comes from the BSS result. The worker refreshes cached peer
RSSI once per second and publishes changes of at least 3 dB. Rate queries
use cached PHY samples; SNR is floored at zero.

## Service and firmware

The signed service runs as `/usr/libexec/unixnu-wpa_supplicant`, with
configuration at `/private/var/ngpu/etc/wifi/wpa_supplicant.conf`.
Packaging preserves 715 stock jobs and adds its own launchd job. The cache
uses Apple `plutil binary1` serialization and a detached SHA-256/4 KiB-page
signature; the trust bundle contains its hash and the service hash.

Firmware loads from `/private/var/ngpu/firmware/ath12k/WCN7850/hw2.0/`.
The loader and packer share `package/WifiFirmware.h`. A local manifest pins
amss.bin, m3.bin and board-2.bin. VFS reads begin after addon installation;
regular-file type, length and complete reads are checked before caching.
The Data seed and saved credentials are preserved.

## Performance

TX copies directly from the retained mbuf into DMA. RX copies skb fragments
into aligned cached mbufs, refills in 16-packet groups and submits up to
64 frames per batch. DMA clones and A-MSDU payloads share storage. Six reuse
classes cache up to 1 MiB; the WCN7850 window provides 128 MiB.

mac80211 retains aggregation, reordering, FQ and AQL decisions. NAPI has a
256-packet/2 ms budget; work dispatch has a 64-item/2 ms budget. Sustained
board work checks preemption every 2 ms. Cached link statistics refresh
without firmware RPCs. Disabled GPU/display diagnostics skip collection,
formatting and register reads.

## Verification and tools

| Check | Command |
| --- | --- |
| Installed source identity | `python3 tools/fetch-sources.py --check` in Ath12kWifi |
| Core lifecycle, timeout and signal | `python3 tools/test-mainline-runtime.py` in Ath12kWifi |
| SME/WPA and rekey | `python3 tools/test-supplicant.py` in Ath12kWifi |
| cfg80211 mapping | `python3 tools/test-cfg80211-control.py` in Ath12kWifi |
| Exact production linkage | `python3 tools/check-mainline-link.py --build-report FILE --kernel FILE` in Ath12kWifi |
| Apple80211 completion | `python3 scripts/check-wifi-association.py` in the board repository |
| Firmware VFS reads | `python3 scripts/check-wifi-firmware.py` in the board repository |
| RX boundary and DMA capacity | `python3 scripts/check-wifi-rx.py`, `python3 scripts/check-wifi-dma.py` in the board repository |

Host sanitizer fixtures pass with simulated radio operations. The core
lifecycle frees all 287 tracked allocations. The production audit covers
125 objects and 13 XNU imports with zero missing imports or duplicate
definitions. Kernel, payload and packed-image checks pass, including
firmware hashes, signatures, 715 stock jobs, 12 trust modules and the Data seed.
Run affected checks after source changes and retain their reports.

Device verification covers Wi-Fi toggling, SSID scanning, WPA2 association,
DHCP, network traffic and join-result delivery. Current device checks
pending: cancellation/timeout completion, runtime signal display, Data
firmware startup, throughput, jitter and heat.

The current temporary image is `build/wifi-mainline/fix9/boot.img`.
Its packed verification is `build/wifi-mainline/fix9/host-report.json`.
Automatic reset and GPU/display diagnostics are disabled; Wi-Fi diagnostics
are enabled. Fastboot has accepted the image for temporary boot.
