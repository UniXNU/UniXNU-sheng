# Licensing of repository touch tests

The ten original C++ fixtures and all thirteen original fixture files retain
their exact bytes and existing notices. Existing file-level SPDX identifiers,
copyright and upstream notices take priority.

New portable runner/adaptation and newly authored test portions without a
file-level notice follow the main project's BSD-2-Clause-Patent default. This
scope does not relicense Apache-2.0 components, the imported baseline HID sender,
or other third-party portions. Production source loaded from the driver and
board checkouts retains its own upstream license.

Complete applicable texts and notice:

- [UniXNU BSD-2-Clause-Patent](LICENSES/BSD-2-Clause-Patent-UniXNU.txt)
- [HID BSD-2-Clause-Patent](LICENSES/BSD-2-Clause-Patent-HID.txt)
- [Apache 2.0](LICENSES/Apache-2.0.txt)
- [Novatek notice](LICENSES/Novatek-NOTICE.txt)

Synthetic fixtures do not include proprietary sensor captures or firmware.
Test metadata and results do not change the licenses of the code they describe.
