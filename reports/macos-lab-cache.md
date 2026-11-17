# macOS lab cache publication

The `Build images` step determines whether the workflow publishes reusable build inputs and kernel compile records. Both save steps require its `success` outcome. Cache keys include the run identifier; restoration uses the matching branch or main prefix. The pinned translator checkout is refreshed after restoration.

| Condition | Result |
| --- | --- |
| Image assembly succeeds | Build inputs and kernel records are eligible for publication. |
| Image assembly fails or is skipped | Both cache saves are skipped. |

[Failure-path verification](https://github.com/UniXNU/UniXNU-sheng/actions/runs/43981441762) used source `f08e5c5e1449ecc0730a335e66978c52732f451b` with an unavailable core revision. Checkout failed and both cache save steps reported `skipped`.

[Successful image assembly](https://github.com/UniXNU/UniXNU-sheng/actions/runs/43984540448) built core `464d32a5ed56be3592af744adea29a7da1a8595a`, packaged the v0 boot image and published both build and kernel caches. The workflow source is `97f50b1d3f73cc948d43d444aa5a00a2baeb5fb1`.
