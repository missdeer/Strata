# Windows native runtime acceptance, 2026-10-11

The tested LAN pair is host16 (i9-12900HK, RTX 4060 Ti 16 GiB, about 32 GiB RAM)
and host11 (i7-10700, about 64 GiB RAM). Both use IQ3_S, the same 0.1.42 CUDA
engine and portable CPU backend, context 8192, prefill 512, temperature 0,
remote reduction, and a 1 GiB coordinator expert RAM cache. The worker uses
7 background threads and preloads all 46.84 GiB of assigned experts into a
48 GiB cache budget. Effective engine settings, argv, configuration digest and
binary hashes are in [summary.json](summary.json).

Protocol/configuration and API golden checks passed. All 56 expert groups have
identical output hashes; each group uses 2 warmups and 20 measured requests.
The native coordinator/worker use package/System32-only PATH and their sampled
process trees contain no Python. Python is an independent measurement driver.
No existing deployment or startup configuration was switched.

The first untraced run of the final mapping/Unicode implementation is retained
as `native-api.json`, including its failed comparison: 6 of 14 code responses
contain three extra indented blank lines. Token counts and usage agree, but exact
text hashes do not. Subsequent untraced repetitions are retained with their
directory prefixes; one was identical, one again varied. The full-text repeat
shows tokens 198 and 6987 correspond to the whitespace difference. Two separate
token-trace diagnostics did not reproduce the variation, and all 28 requests in
each had equal raw-token decoding and SSE text. This does not identify the cause
of the earlier differences. Stage 6's stable output/performance comparison remains
unfinished; no speed improvement is established by these measurements.

Both normal measurement paths disable per-task logging and profiling. API groups
use 2 warmups and 12 measured requests. TTFT includes prefill; decode tok/s comes
from the engine clock. These are repeated prompts with session cache reuse, not
cold prompt measurements. Nearest-rank p95 for 12 samples is the maximum. CPU,
RSS, system RAM and GPU/VRAM are sampled every 0.5 seconds; other development
activity was not excluded, peaks may be missed, and shared pages may be counted
twice in process-tree RSS. The driver reports application/frame bytes, excluding
HTTP headers and TCP/IP overhead. Token-trace runs are diagnostic only.

Files:

- `python/native-api.json`: per-request timings, output hashes, usage and network deltas.
- `python/native-experts.json`: all shapes, raw samples, frame bytes and output hashes.
- `python/native-*resources.json`: raw 0.5-second samples and process names.
- `native-repeat-*`: preserved untraced reruns, including both output variants.
- `native-refresh-66dc9fc0-*`: first token-trace diagnostic; excluded from the performance table.
- `real-api-parity.json`: eight real LAN API/engine checks, all equal.
- `api-complete.json`, `text-final.json`: 53 API checks and 15 text groups passed.
- `whitespace-stream.json`: indented blank lines at different token delivery rates,
  including optional raw-token trace, passed.
- `token-sse-diagnostic.json`: final diagnostic's 28 raw-token/SSE comparisons.
- `stage6-*.txt`: CPU/CUDA/launcher and host11 target-compiled worker checks.
- `hip-default-path.json`: native ON/OFF HIP engine hashes are equal. CUDA engine
  hashes before/after the runtime changes are equal in `engine-default-hashes.json`.

CPU, CUDA sm_89, HIP gfx1102/gfx1103 and SYCL native-host configurations compiled.
CUDA GPU experts ran; HIP/SYCL GPU inference, the Windows SYCL engine, Linux and
a fresh Windows VM were not validated. The separate host11 `/QxHost` build was
compiled on the i7-10700 with oneAPI 2026.1/Visual Studio 2022 and checked for
correctness. Its speed relative to portable AVX2 was not measured.

Reproduce with `tools/validate_native_lan.py` and `tools/summarize_native_runtime.py`;
see [docs/MULTI_HOST.md](../../../docs/MULTI_HOST.md) for arguments and boundaries.
The acceptance driver exits unsuccessfully when exact output/usage differs.
