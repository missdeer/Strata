# Intel GPU SYCL probe

On Windows, with the Intel oneAPI DPC++ compiler and a working Intel GPU driver installed, run from the repository:

```bat
tools\run_sycl_probe.bat
```

The script initializes oneAPI, compiles `tools/sycl_probe.cpp`, and runs `build-sycl-probe/sycl_probe.exe`.
It does not build Strata or download a model. For a non-default oneAPI installation, set `STRATA_ONEAPI_ENV`
to the full path of its environment `.bat` file first.

The program lists visible devices and tests every Intel GPU entry. A physical GPU may appear twice, once through
Level Zero and once through OpenCL. It checks FP32 buffer computation, device and shared USM, FP16 computation,
and a 32-lane sub-group reduction against known results. Unsupported optional features are reported as skipped.
The matrix extension and USM atomic capabilities are queried, not tested by executing kernels.

Exit codes: 0 = executed tests passed; 1 = a test failed; 2 = no Intel GPU visible; 3 = device enumeration failed.
Inherited `ONEAPI_DEVICE_SELECTOR` or `SYCL_DEVICE_FILTER` values can hide devices and are printed when present.

These small tests do not establish full Strata compatibility, inference speed, or model memory capacity.
The reported global memory is the runtime's limit, not free or dedicated VRAM.

Measured on this PC (2026-10-05): i9-12900HK, Iris Xe, approximately 32 GB system RAM, Windows,
Intel graphics driver 32.0.101.7092, oneAPI DPC++ 2026.1.1. All five tests passed on both Level Zero and OpenCL.
Both backends reported FP16, USM device/shared/host, and sub-group sizes 8/16/32; FP64 and USM atomic
shared/host were absent, and the matrix query returned zero combinations. No full model was tested.
