# WCN7850 firmware inputs

Place amss.bin, m3.bin and board-2.bin under `wcn7850/`.
`wcn7850/manifest.json` pins their sizes and SHA-256 hashes. Firmware binaries
stay local; the repository contains the manifest and packaging code.

`scripts/build-wifi-userspace.py OUT ios27` validates the inputs and stages
them for the Data addon. `package/WifiFirmware.h` defines the shared loader
and packer path: `/private/var/ngpu/firmware/ath12k/WCN7850/hw2.0/`.

After addon installation, the VFS reader validates regular-file type and
size, reads each file and retains successful results for the boot lifetime.
The payload build preserves the Data seed and OS-saved credentials.
