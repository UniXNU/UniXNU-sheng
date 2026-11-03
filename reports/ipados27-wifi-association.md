# iPadOS 27 Wi-Fi association

WifiNet accepts 592-byte and 656-byte association requests. Both use version 1;
network fields start at 0x0c and 0x10 respectively. Credentials support open
networks, passphrases and 32-byte PMKs. Authentication queries return WPA2
personal as 0x10 and support 12-byte and 16-byte reply buffers.

## Completion

A new accepted join clears the visible connection snapshot and records a
pending request. Success follows port authorization. The 24A446 asynchronous
event contains 540 initialized bytes, with result and validity at 0x214/0x218;
the 592-byte request path uses an event followed by a result query.

Cancellation ends a pending request once. An unanswered join expires after
30 seconds and queues disconnect. Switching networks drains teardown before
queuing the next request. Disconnect and power-off clear IFF_RUNNING and
publish BSD/Apple80211 link-down, SSID and BSSID events. Connection generations
filter delayed authorization and key events.

Saved credentials come from the OS, so a previously joined network can
connect while its password sheet is empty. RSSI comes from the association
BSS and cached peer state, refreshed once per second with a 3 dB event threshold.

## Verification

`scripts/check-wifi-association.py` passes ASan/UBSan for wire layouts,
credentials, completion events, switching, cancellation and power-off.
`drivers/Ath12kWifi/tools/test-mainline-runtime.py` covers join deadlines,
port authorization, delayed events and RSSI filtering. Device verification
covers WPA2, DHCP, traffic and join-result delivery; cancellation/timeout
and signal display are pending on the current image.

Build, packaging and firmware details are in
[Wi-Fi implementation](ipados27-wifi-mainline-migration.md).
