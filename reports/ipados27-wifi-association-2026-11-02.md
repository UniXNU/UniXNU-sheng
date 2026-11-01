# iPadOS 27 Wi-Fi association

WifiNet supports the 592-byte legacy and 656-byte 24A446 association
requests. Both use version 1; request length selects the layout. The network
fields begin at offsets 0x0c and 0x10 respectively. Credentials support open
networks, WPA passphrases and 32-byte PMKs. Layout and length checks reject
malformed requests before authentication.

Explicit power-off and disconnect clear IFF_RUNNING and publish BSD and
Apple80211 link-down, SSID and BSSID events. Status queries hide the old
connection while firmware teardown completes. Late association callbacks
are suppressed during this interval.

Repeated joins to the connected network preserve the successful result and
publish completion. Repeated joins during authentication await the active
attempt. Network switching waits up to 2000 ms for teardown before queuing
the new connection. Association completion uses an empty event followed by
a result query. Authentication queries report WPA2 personal as 0x10 and
support 12-byte and 16-byte reply buffers.

## Validation

`scripts/check-wifi-association.py` passes with AddressSanitizer and
UndefinedBehaviorSanitizer. Coverage includes both request layouts,
credentials, repeated joins, network switching, teardown timeout,
disconnect notifications and authentication replies.

The 24A446 kernel, payload and boot image build successfully. Packed-image
checks pass for the repair diagnostics, corecrypto `_kprintf`, all 266
original adapter branches and the Data seed, trust bundle and GPU addon.

The current local image is
`UniXNU/out/wifi-state-retry-20261102/boot.img` (185733120 bytes).
Its SHA-256 is
`be1a5905f3a238213d105f1692bc2a4f93912e3d41ddd887459c8de5f2d71ed7`.
The embedded kernel SHA-256 is
`145c75a779dcb11786ac2cf819c83150fb530655610f76d270c660176e854324`.
It enables `log-wifi=1 sheng-wifi-aj=0 sheng-diag=1 sheng-diag-export=240`.
Automatic reset is disabled. This image has not been booted on the device.

## Device status

Settings can toggle Wi-Fi and list SSIDs. Manual WPA2 association and DHCP
have device confirmation. The current state changes await device validation
of joining, network switching, the Wi-Fi icon and password-sheet dismissal.
Credential persistence, group-key disconnects and authentication timeouts
remain open.

Throughput is low. Investigation targets include VHT MCS decoding,
aggregation and retries. The receive aggregation limit is eight subframes;
the reported rate is a fixed 54. Controlled transfer measurements and
firmware rate statistics are pending.
