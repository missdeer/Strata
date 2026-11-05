#pragma once

#include <stdint.h>

#if defined(_WIN32) && defined(STRATA_NETWORK_BACKEND_EXPORTS)
#define STRATA_NETWORK_DECL __declspec(dllexport)
#else
#define STRATA_NETWORK_DECL
#endif

// Shared C ABI for native and Python workers. The caller owns all task buffers;
// compute/reduce finish using them before returning. A handle is used serially.
#ifdef __cplusplus
extern "C" {
#endif

STRATA_NETWORK_DECL const char* strata_network_error(void);
STRATA_NETWORK_DECL void* strata_network_create(int workers, int device, uint64_t cache_mib);
STRATA_NETWORK_DECL void strata_network_destroy(void* handle);
STRATA_NETWORK_DECL uint64_t strata_network_format_bytes(int gate_up, int down);
STRATA_NETWORK_DECL int strata_network_all_finite(const void* data, uint64_t bytes);
STRATA_NETWORK_DECL int strata_network_compute(void* handle, int gate_up, int down, int layer,
                           const float* input, int tokens, int entries,
                           const int32_t* rows, const int32_t* ids,
                           const uint8_t* const* blobs, int k, float* output);
STRATA_NETWORK_DECL int strata_network_reduce(const float* parts, const int32_t* rows, const float* weights,
                          int tokens, int entries, int k, float* output);

#ifdef __cplusplus
}
#endif

#undef STRATA_NETWORK_DECL
