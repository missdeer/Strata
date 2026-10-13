# Local Windows build scripts

These scripts were moved from `tmp/`. They describe the local development
machines and their existing build trees, rather than a clean-machine installer.
Run them from `D:\Projects\Strata` with `cmd /c build\<script>.cmd` or
`python build\<script>.py <action>`. Python drivers still find the repository
root one directory above the script.

| Target | Build tree | Scripts |
| --- | --- | --- |
| Local / host16, CPU | `build-network` | `build-network.cmd`, `rebase-build-cpu.cmd`, `run-consolidated-cpu*.cmd` |
| Local / host16, NVIDIA CUDA | `build-intel` | `build-intel-oneapi.cmd`, `build-network-cuda.cmd`, `rebase-build-cuda.cmd`, `run-consolidated-cuda*.cmd` |
| host17, AMD HIP (cross-built locally) | `build-cluster/host17-core` | `build-host17-core.cmd`, `rebase-build-hip.cmd`, `run-consolidated-hip*.cmd` |
| Intel SYCL (built locally) | `build-sycl-text` | `rebase-build-sycl*.cmd`, `run-consolidated-sycl*.cmd` |
| host11, remote CPU | Remote directories named in each driver | `stage6-remote-build.py`, `stage6-prepare-host11-runtime.py`, `host11-console-build.py`, `rebase-host11-build.py` |
| Native cluster build tool | `build-cluster/builder` | `build-stage5-builder.cmd`, `rebase-build-builder.cmd` |

The local scripts use Visual Studio 2022 Community, Intel oneAPI 2026.1,
and the ROCm environment in `.rocm-win`, as applicable. Image and JSON
dependencies remain in `tmp/native-image-deps` and `tmp/jsoncons-source`.
Logs, reports, packages, and downloaded dependencies remain in their existing
locations and are not included here. Remote drivers use the existing SSH login
and `tmp/stage6-known-hosts`; provision those locally before using them.

`rebase-verify-windows.*`, `rebase-host16-checks.*`, `rebase-lan-smoke.*`,
`verify-native-host16-entry.py`, and `rebase-final-report.*` are companion
validation/report drivers. Compiler checks are in `check-*.cmd` and
`verify-*.cmd`; `verify-hip-sdk.hip` is the HIP compilation fixture.

Historical `build-native-*`, `build-launcher-*`, stage 5/6, and rebase scripts
retain their original targets, flags, dates, and safeguards. Some refer to
standalone executables replaced by `strata-host`, or require a specific source
commit or existing report. Moving them does not make them current build entry
points. `run-consolidated-*` uses the consolidated host targets; the rebase
drivers record the later verification workflow. Review a script before running
it: some configure build trees, transfer files, package runtimes, or run tests.

This migration preserves compiler commands and build settings. Only references
to the moved scripts and HIP fixture have changed. Runtime configuration and
engine source are unchanged.
