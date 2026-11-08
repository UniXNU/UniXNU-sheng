# V3.1 touch host checks

These checks run against the actual UniXNU-sheng checkout: drivers/ShengNovatekTouch,
drivers/ShengHidInput and package. They also verify the companion central UniXNU
builder through --unixnu-root. No production source implementation is duplicated
under this test directory.

Use the coordinated touch changes in the two driver checkouts, the board checkout
and the central UniXNU checkout. The nineteen-file hash profile detects a mixed
or unreviewed checkout. The canonical device/ keys in that profile map to the board
root; UniXNU/ keys map to the central checkout. They are report namespaces, not
directories to create in this repository.

From the board repository root, on Linux with Python 3.10+, a GNU-compatible C11
and C++17 compiler and address/undefined-behavior sanitizer support:

```sh
python3 -B tools/run_touch_v3p1_host.py \
  --unixnu-root ../UniXNU \
  --out ../touch-v3p1-host-work \
  --report-dir ../touch-v3p1-host-reports \
  --legacy-author-core
```

--unixnu-root defaults to an adjacent UniXNU checkout. --board-root can explicitly
select another board checkout for the same checked files; test fixtures still
come from this repository. Choose a new --out directory outside both repositories.
No package installation, network download, cloud runner, Apple SDK, kernel build,
firmware input or device operation is performed. CC and CXX can select compatible
host compilers.

## Test contents

The ten original C++ fixtures remain byte-identical, with their existing notices
and 122 named checks: Core22, coverage22, engine7, formula/config11, nuisance1,
partial17, Probe8, quantization1, region18 and timeline15. The four C components
are compiled from the checked-out Novatek source; inline board headers are used
directly. Nineteen is the checked file count, including two build scripts, not a
count of compiled objects.

The HID suite extracts the actual checked-out touchSend body and tests it with
explicit platform stubs: 15 named checks plus 9,936 generated contact-set
transitions. The original sender is a pinned control, not an alternate runtime
implementation. The planner header also compiles as C. The public protocol
chip model runs without its optional real firmware. --legacy-author-core runs
the unmodified checked-out author fixture after verifying its pinned public hash.

[FIXTURE-PROVENANCE.json](FIXTURE-PROVENANCE.json) binds all thirteen fixture
files. [expected-production-hashes.json](tools/expected-production-hashes.json)
and [expected-author-fixture.json](tools/expected-author-fixture.json) bind the
runtime/build inputs and original author fixture.

## Evidence and limits

[host-results.json](results/host-results.json) records the fresh execution of this
repository-layout runner, normalized commands, exact source hashes, compiler
versions, individual checks and separate suite results.
[probe-helper-equivalence.json](results/probe-helper-equivalence.json) records
the unchanged text of the eight extracted helpers and declarations relative to
the successful original V3.1 version.

The Probe extractor replaces exactly two ARM barriers with host barriers. The HID
extractor changes the self type and exactly two ARM barriers. Clocks, allocation,
trace sinks, driver availability and HID submission results use platform stubs.
An acknowledged stub submission is not proof that the operating system consumed
an event. Author SPI/SMMU coordination outside the extracted helpers, build
scripts, target ABI/timing, boot stability and physical sensor/pen behavior need
their own native or device checks.

## Privacy and licenses

Matrices, trajectories and HID transitions are synthetic. No captured sensor
frame, firmware blob, credential, identity, raw device log or private machine
path is included or required. Build binaries and full local command output stay
in --out; reports normalize repository and work paths.

See [LICENSING.md](LICENSING.md). Existing file-level notices and imported-code
licenses take priority.
