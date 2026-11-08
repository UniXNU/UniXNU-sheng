# AppSync on sheng iPadOS 27

AppSync covers every user application bundle on Data on sheng iPadOS 27.0.1
(24A446), including unsigned, fakesigned and expired packages, with no
application list and no payload rebuild per app. The common implementation is
`UniXNU/scripts/appsync/`. The board owns `sheng-appsync-runtime.patch`,
`sheng-appsync-user-trust.patch`, their series entries,
`payload.runtime.appsync` and the `appsync=1` boot argument.

## Implementation

[AppSync Unified 235aca6](https://github.com/akemin-dayo/AppSync/tree/235aca6cddfbdc9fa87fcb5b2aec2df37ed6d65a)
provides the split between installation validation, FrontBoard launch trust
and an LSApplicationWorkspace installer; the adaptation keeps its GPLv3
attribution. The hooks target InstalledContentLibrary's `MICodeSigningVerifier
performValidationWithError:` and FrontBoardServices'
`FBSSignatureValidationService trustStateForApplication:`, check method
encodings, and only alter user `.app` trees under `/private/var/` or `/var/`.

The installation hook enables native ad-hoc validation, drops the preloaded
trust-cache and online authorization requirements, skips profile ID validation
and restores the original options on every exit. When native validation fails,
`Signing.m` locks and preflights the bundle, removes obsolete provisioning
profiles, re-signs code and resources recursively and validates again.
Entitlements are preserved, valid packages stay unchanged, and encrypted or
malformed Mach-O files and escaping symlinks are rejected.

The signer is `/usr/lib/libunixnu-ldid.dylib`, built from the Procursus ldid,
OpenSSL and libplist sources pinned in `signer-sources.json` and loaded inside
installd. It memory-maps the files it signs, signs `PlugIns/` and `Extensions/`
bundles as nested code and marks signatures without a CMS identity as ad-hoc.
Signing is offline; dependency licenses ship with the runtime.

In `ubc_cs_blob_add`, before the native AMFI check, the kernel registers
validated user CodeDirectory hashes in the engineering trust cache.
Registration deduplicates under its lock, is bounded to 65536 modules per boot
and covers executables, updates and nested libraries on demand. VM code-page
checks stay active. After `load_machfile`, exec clears the platform status that
trust-cache membership gives an appsync user `.app` main executable, so the app
runs as an ordinary third-party process; Apple platform binaries are outside
`/var`. `appsync=0` disables the mounts and the registration.

The three original OS executables are pinned by hash and load Foundation
through a signed adapter that re-exports it with the original ordinals, code
layout and chained/PAC fixups. Runtime code hashes, including the signer, are
in the boot trust bundle; user application hashes are not build inputs.

## Applications

Installed applications with usable signatures run without reinstalling.
Unsigned or damaged code goes through the installation repair. Install a
compatible IPA over USB with any MobileInstallation client, or on the device:

```sh
/usr/libexec/unixnu-appinst /path/on/device/Example.ipa
```

FairPlay-encrypted apps and binaries for another CPU or OS do not run.

## Verification

The host suite in `UniXNU/scripts/appsync/` runs the production hooks, signing,
kernel trust registration and runtime integration under ASan/UBSan. It covers
concurrent registration, unknown apps and updates, nested and fat code,
unsigned, non-Apple and expired signatures, resource and profile repair,
preserved entitlements, unsafe input rejection and codesign checks of the
output.

On sheng, an unsigned, decrypted arm64 app installs over USB, launches, stays
usable and remains installed across reboots.
