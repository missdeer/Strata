// Fault injection for native worker lifecycle tests; never packaged/deployed.
#define STRATA_NETWORK_BACKEND_EXPORTS
#include "strata/core/network_backend.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#ifdef _WIN32
#define API extern "C" __declspec(dllexport)
#else
#define API extern "C" __attribute__((visibility("default")))
#endif

namespace {
std::string mode() {
    const char* value = std::getenv("STRATA_TEST_BACKEND_MODE");
    return value ? value : "";
}
}
API const char* strata_network_error() { return "injected backend failure"; }
API void* strata_network_create(int, int, uint64_t) { return new int(0); }
API void strata_network_destroy(void* handle) { delete static_cast<int*>(handle); }
API uint64_t strata_network_format_bytes(int gu, int down) { return gu == 22 && down == 42 ? 1510400 : 0; }
API int strata_network_all_finite(const void* data, uint64_t bytes) {
    const auto* p = static_cast<const uint8_t*>(data);
    for (uint64_t i = 0; i < bytes; i += 4) {
        uint32_t word; std::memcpy(&word, p + i, 4);
        if ((word & 0x7f800000) == 0x7f800000) return 0;
    }
    return 1;
}
API int strata_network_compute(void* handle, int, int, int, const float*, int, int n,
                              const int32_t*, const int32_t*, const uint8_t* const*, int, float* output) {
    // A second call would succeed. The worker must keep the backend failed and
    // refuse that call, including when the coordinator reconnects.
    if ((*static_cast<int*>(handle))++ == 0) {
        if (mode() == "compute") return 0;
        if (mode() == "nonfinite") {
            const uint32_t inf = 0x7f800000; std::memcpy(output, &inf, 4); return 1;
        }
    }
    std::fill(output, output + n * 2560, 0.f); return 1;
}
API int strata_network_reduce(const float*, const int32_t*, const float*, int nt, int, int, float* output) {
    if (mode() == "reduce") return 0;
    std::fill(output, output + nt * 2560, 0.f); return 1;
}
