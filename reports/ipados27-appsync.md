# AppSync adaptation, 2026-11-07

The implementation targets sheng iPadOS 27.0.1 (24A446). It applies to all
user application bundles on Data, including unsigned, fakesigned and expired
packages, without an application list or a payload rebuild for each update.
The common implementation lives in `UniXNU/scripts/appsync/`. The board owns
`sheng-appsync-runtime.patch`, `sheng-appsync-user-trust.patch`, their series
entries, `payload.runtime.appsync` and the `appsync=1` boot argument.

## Implementation

[AppSync Unified, revision 235aca6cddfbdc9fa87fcb5b2aec2df37ed6d65a](https://github.com/akemin-dayo/AppSync/tree/235aca6cddfbdc9fa87fcb5b2aec2df37ed6d65a)
provides the separation between installation validation, FrontBoard launch
trust and an LSApplicationWorkspace installer. The adaptation retains GPLv3
attribution. The inspected 24A446 API is InstalledContentLibrary's
`MICodeSigningVerifier performValidationWithError:`; FrontBoardServices retains
`FBSSignatureValidationService trustStateForApplication:`. Hooks check method
encodings and only alter user `.app` trees under `/private/var/` or `/var/`.

The installation hook enables native ad-hoc validation, disables preloaded
trust-cache and online authorization requirements, skips profile ID validation,
and restores the original options on all exits. If native validation fails,
`Signing.m` locks and preflights the bundle, removes obsolete provisioning
profiles, recursively repairs signatures/resources using the bundled ldid,
and runs native validation again. Existing entitlements are preserved. Valid
packages remain unchanged. The repair rejects encrypted or malformed Mach-O
files and escaping symlinks, and has a bounded signer deadline.

The signer is source-built from pinned Procursus ldid, OpenSSL and libplist
revisions and archive hashes in `signer-sources.json`. `ldid-adhoc.patch` marks
outputs with no CMS identity as ad-hoc, as required by native AMFI. The runtime
includes dependency licenses; the build cache retains objects and static
libraries for relinking. Signing is offline on the device.

The kernel registers validated user CodeDirectory hashes before the native
AMFI check in `ubc_cs_blob_add`, through the existing engineering trust-cache
interface. Registration deduplicates under its lock and is bounded to 65536
modules per boot. Executables, updated apps and nested libraries are handled
on demand. VM code-page checks remain active. FrontBoard supplies its trusted
state for user bundles. `appsync=0` disables the mounts and dynamic registration.

The three original OS executables are pinned by hash. Their Foundation load
is routed through a signed adapter that re-exports Foundation while preserving
ordinals, code layout and chained/PAC fixups. The complete SpringBoard/helper
bundles are included. Runtime code hashes, including the signer, enter the
boot trust bundle; user application hashes are not build inputs. Original
trust modules and the original Data seed are preserved.

## Existing and new applications

Existing applications with usable signature structures are covered after
booting the image, without reinstalling every application. Truly unsigned or
structurally damaged code needs the installation repair path before launch.
Install a compatible IPA on device using:

```sh
/usr/libexec/unixnu-appinst /path/on/device/Example.ipa
```

Other installers using MobileInstallation use the same hooks. This does not
decrypt FairPlay content or make incompatible CPU/OS binaries executable.

## Completed host verification

`UniXNU/build/appsync-full-tests.log` records the successful production hook,
signing, kernel trust and runtime integration suite. It covers ASan/UBSan,
concurrent trust registration, unknown apps and updates, nested and fat code,
unsigned/non-Apple/expired signatures, resource/profile repair, preserved
entitlements, unsafe input rejection and independent codesign verification.
The runtime test retains original trust modules and Data and is idempotent.
The seven runtime-input tests and documentation build also passed.
The host models the initial iOS certificate rejection; it does not establish
that Apple's actual iOS verifier or dyld accepts the complete device workflow.

## Combined image and device run

The full kernel/payload build is recorded in
`UniXNU/build/appsync-full-package.log`. The image also includes the concurrent
backlight changes in `package/AppleARMBacklight.cpp` and `package/BootLog.c`,
including brightness-nits handling, calibration properties and diagnostics.
A chained volatile assignment was split into separate writes for C++23.

The original Data seed SHA-256 is
`1bde7901df3acd0e8a14cd89928bedc12c07ef2128c6bce18ffccf3611a666a3`.
The target output is
`UniXNU/out/sheng/ipados-27.0.1-24A446/sheng_ipados27/boot/appsync-all-users/boot.img`.
The full kernel/payload and boot-image builds exited 0. The final boot image
is 185733120 bytes with SHA-256
`22c02863eacc39f7d542dee3674f85b7822fdc3c1efc897fe2cba107bf551739`.
Its default arguments include `appsync=1` and `sheng-backlight=1`. Adjacent
`boot.json`, `payload.json`, `appsync-service.json` and `manifest.json` record
the inputs and outputs.

The authorized `fastboot -s b714917c boot boot.img` completed successfully:
transfer OKAY in 4.218 seconds, Booting OKAY in 0.174 seconds, exit status 0.
This was a temporary boot; no flash or Data initialization command was issued.
The fastboot result establishes that the bootloader accepted the image. Desktop,
application installation/launch and backlight behavior still need observation
on the device.
