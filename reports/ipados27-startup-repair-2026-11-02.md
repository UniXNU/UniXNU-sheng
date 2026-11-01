# iPadOS 27 startup repair observations

These observations were recorded on 2026-11-02 while the repairs were developed in an uncommitted working tree. They are retained as a historical source report, not as commit-pinned host/device qualification. The raw logs and binary artifacts remain in the local workspace; their paths below are relative to its UniXNU checkout, not links to published artifacts.

The packaging area remains `implemented`. No new build, device test or workflow dispatch was performed when publishing this report.

## Observation 1

sheng, iPadOS 27.0.1 24A446, boot-prelink-fixed.img: instruction fetch abort at 0xfffffff00dbb4c74 with LR 0xfffffff00b5a3340 in native_legacy_display_allocate. Corecrypto and SourceStorage initialization had completed.

Local artifact: `out/panic27-fix-20261102/device-log-read-20261102/untagged-90cbf4fac/msgbuf.txt`.

Recorded command (historical):

```sh
read-sheng-log.py --port /dev/cu.usbmodemshenglog1 --no-reboot
```

## Observation 2

62 prelink regressions pass. The packed kernel has 266 fixed original-function branches (205 mobile graphics, 51 HID, eight IOSurface, two GPU), preserved absolute GOT values and no additional native external branch instructions. This is host evidence, not a desktop boot result.

Local artifact: `out/startup27-fix-20261102/startup-static-audit.json`.

Recorded command (historical):

```sh
python3 -m unittest discover -s kits/XnuPrelink -p 'test_*.py'; python3 out/startup27-fix-20261102/audit-startup.py
```

## Observation 3

sheng, 24A446: NativeLegacyDisplay and NativeHidInput initialized, APFS mounted the root OS and SystemCryptex, then load_init_program stopped with offline scratch Data mount failed: 2. The payload lacked initrd and PackedRAMDisk.

Local artifact: `out/startup27-fix-20261102-v3/device-run2/untagged-1304f7048/msgbuf.txt`.

## Observation 4

Recovered runtime inputs match boot-framepath.img: seed 1bde7901df3acd0e8a14cd89928bedc12c07ef2128c6bce18ffccf3611a666a3 equals the existing data-seed-215g-skipca/data.dbr. The payload includes the original owner trust cache and GPU addon, required ADT placeholders and 870-byte normal arguments without autoreset or data-reset. Checks cover 266 branches, five absolute data symbols and 189 vnode descriptor pointer sites. Local inputs are stored in the ignored UniXNU unixnu.local.toml.

Local artifact: `out/startup27-fix-20261102-v4/verification.json`.

Recorded command (historical):

```sh
python3 out/startup27-fix-20261102-v4/verify-image.py; python3 scripts/check-unixnu.py
```

## Observation 5

The repaired v4 kernel and restored runtime inputs ran to the 120-second auto-reset deadline: log state 2, 61 rounds, zero write errors and no panic. SpringBoard PID 30, backboardd PID 61 and wifid PID 49 are present. The diagnostic image differs from the normal payload only by sheng-autoreset=120. Visual presentation was not verified. Two unresolved data imports and 24 trap references remain outside this boot qualification.

Local artifact: `out/startup27-fix-20261102-v4/hardware-verification.json`.

Recorded command (historical):

```sh
fastboot boot diagnostic/boot-diagnostic.img; fastboot boot sheng-linux-log.img; read-sheng-log.py --no-reboot
```

## Subsequent normal packaging run

The normal package.sh pipeline completed and produced boot.img with SHA-256 `142d66c645d2dc9e6323d712381d836073facf196537c163e8070c053cdc28cf`. An independently inspected payload retained 266 corrected adapter branches, five absolute data symbols, 24 pre-existing trapped imports and two unresolved data references.

A diagnostic variant with only `sheng-autoreset=120` added recorded 120 seconds, 61 log rounds, state 2 and zero write errors. No kernel panic was found in that capture. It reused the original Data seed. Visual UI behavior, Wi-Fi Settings and full sheng Actions execution were not qualified.

Local records: `out/package-integration-hardware-20261102/hardware-verification.json` and `out/package-integration-hardware-20261102/device-run/untagged-1e549aad6/header.txt`.

The implemented repair snapshots are [UniXNU 474cfee](https://github.com/UniXNU/UniXNU/commit/474cfeeb4e35b3fc91ec98cd2bb54e1ab25389b8), [XnuPrelink b1888c1](https://github.com/UniXNU/XnuPrelink/commit/b1888c12456c46a2226d63c794465dfcee08d465), and [sheng 6dafef9](https://github.com/UniXNU/UniXNU-sheng/commit/6dafef94283ea6e68ccaae981476c6684fc4079d). These were committed after the local observations; the historical rows above do not assert an exact clean-checkout reproduction at those commits.
