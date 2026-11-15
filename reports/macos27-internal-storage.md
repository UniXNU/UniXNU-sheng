# macOS 27.3 internal UFS windows

Xiaomi Pad 6S Pro (sheng), macOS 27.3 build 26C5035r.

The tested device source is `0aa9193002eab9879e5b4cb4ff8df7e200df1ac6`.
Its ShengUfs tree matches PR #12 at `a45476824275f5c0c3c94825ac1be0efece44edc`.
The kernel was built locally with the Xcode 26.6 toolchain.

Capture `s2d373-68f68e191`, boot session `B7CEEB79-EB24-4DAF-AC5F-15DED7926D4E`, records eight storage snapshots during the timed RAM boot:

- All three `NativeUfsBlock` windows publish Internal location and Virtual Interface interconnect properties.
- `/dev/disk5s1` remains mounted at `/System/Volumes/Preboot/Cryptexes/App` as sealed, read-only APFS in every snapshot.
- `/Applications/Safari.app` points to the App cryptex's Safari bundle, whose target directory is accessible.
- The boot preserves the installed Data store, records 302 seconds of kernel uptime and returns through the Linux reader to command-responsive fastboot.

The preceding captures `s9ed3b-71c50dadf` and `sf86a5-6bfdc0b33` also mount the App cryptex after reboot with the same storage source.

Command: `SERIAL=b714917c READER=sheng-linux-log.img OUT=capture scripts/run-sheng-unattended.sh sheng-macos27-pass-snapshot.img 540`.
