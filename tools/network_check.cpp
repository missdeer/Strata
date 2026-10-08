// Exercises the actual coordinator transport and compares worker rows with the local CPU backend.
#include "strata/core/network_experts.hpp"
#include "strata/kernels/cpu/expert_layout.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

extern "C" void* strata_network_create(int, int, uint64_t);
extern "C" void strata_network_destroy(void*);
extern "C" const char* strata_network_error();
extern "C" int strata_network_compute(void*, int, int, int, const float*, int, int,
    const int32_t*, const int32_t*, const uint8_t* const*, int, float*);

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: strata-network-check PACK IPv4:port[,IPv4:port] LAYERS [expected-error-or--] [nativeGGUF]\n"
                             "Needs native_experts.txt, experts.bin or nativeGGUF, and STRATA_NETWORK_KEY.\n");
        return 2;
    }
    const std::string expected = argc > 4 && std::string(argv[4]) != "-" ? argv[4] : "";
    auto fail = [&](const std::string& error) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return !expected.empty() && error.find(expected) != std::string::npos ? 0 : 1;
    };
    const int layers = std::atoi(argv[3]);
    std::string error;
    if (!strata::kernels::cpu::expert_layout_load(argv[1], layers, 512, error)) return fail(error);
    const auto& layout = strata::kernels::cpu::expert_layout();
    std::ifstream file(std::string(argv[1]) + "/experts.bin", std::ios::binary);
    if (!file && argc < 6) return fail("cannot read experts.bin; provide nativeGGUF");
    const bool packed = bool(file);
    std::map<std::string, std::unique_ptr<std::ifstream>> shards;
    std::vector<uint8_t> blob(static_cast<size_t>(layout.max_blob));
    auto read = [&](int64_t l, int64_t e) -> const uint8_t* {
        if (packed) {
            file.seekg(static_cast<std::streamoff>(layout.blob_offset(l, e)));
            file.read(reinterpret_cast<char*>(blob.data()), static_cast<std::streamsize>(layout.blob_bytes(l)));
            return file ? blob.data() : nullptr;
        }
        const auto& f = layout.fmt[static_cast<size_t>(l)];
        for (size_t r = 0; r < 3; ++r) {
            const size_t index = static_cast<size_t>(3 * l) + r;
            const auto path = layout.gguf_file.empty() || layout.gguf_file[index].empty() ? std::filesystem::path(argv[5])
                : std::filesystem::path(argv[5]).parent_path() / layout.gguf_file[index];
            auto& shard = shards[path.string()];
            if (!shard) shard = std::make_unique<std::ifstream>(path, std::ios::binary);
            const size_t size = r < 2 ? f.up_off : f.bytes - f.down_off;
            const size_t offset = r == 0 ? 0 : r == 1 ? f.up_off : f.down_off;
            shard->seekg(static_cast<std::streamoff>(layout.gguf_off[index] + e * size));
            shard->read(reinterpret_cast<char*>(blob.data() + offset), static_cast<std::streamsize>(size));
            if (!*shard) return nullptr;
        }
        return blob.data();
    };
    const char* key = std::getenv("STRATA_NETWORK_KEY");
    const char* assignment = std::getenv("STRATA_NETWORK_ASSIGNMENT");
    const char* config_id = std::getenv("STRATA_NETWORK_CONFIG_ID");
    strata::core::NetworkExperts net;
    if (!net.open(argv[2], key ? key : "", 1000, read, layers, 512, error,
                  assignment ? assignment : "", config_id ? config_id : "")) return fail(error);
    void* backend = strata_network_create(2, -1, 0);
    if (!backend) return fail(strata_network_error());
    constexpr int H = 2560, k = 4;
    int rc = 0;
    for (int round = 0; round < 4; ++round) {
        const int l = round % layers, nt = round % 2 ? 3 : 1, n = nt * k;
        std::vector<float> x(static_cast<size_t>(nt) * H), got(static_cast<size_t>(n) * H, 19.0f), ref(got.size());
        for (size_t i = 0; i < x.size(); ++i) x[i] = std::sin(static_cast<float>(i) * .13f) * .2f;
        std::vector<int32_t> ids(static_cast<size_t>(n)), kind(static_cast<size_t>(n), -1), rows(static_cast<size_t>(n));
        std::vector<std::vector<uint8_t>> blobs(static_cast<size_t>(n));
        std::vector<const uint8_t*> ptr(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i) {
            ids[static_cast<size_t>(i)] = assignment ? 2 + (i % k) * 128 : i % k + 2;
            rows[static_cast<size_t>(i)] = i;
            const auto* b = read(l, ids[static_cast<size_t>(i)]);
            if (!b) { rc = fail("short expert file"); break; }
            blobs[static_cast<size_t>(i)].assign(b, b + layout.blob_bytes(l));
            ptr[static_cast<size_t>(i)] = blobs[static_cast<size_t>(i)].data();
        }
        // Mark one row as a local GPU hit; the network tier must leave it untouched.
        kind[0] = 0;
        const auto& f = layout.fmt[static_cast<size_t>(l)];
        if (!strata_network_compute(backend, f.gu_type, f.d_type, l, x.data(), nt, n, rows.data(), ids.data(), ptr.data(), k, ref.data())) {
            rc = fail(strata_network_error()); break;
        }
        if (!net.begin(l, x.data(), ids.data(), nt, k, kind.data(), error) || !net.finish(got.data(), error)) {
            rc = fail(error); break;
        }
        double delta = 0, scale = 0;
        for (size_t i = H; i < got.size(); ++i) {
            delta = std::max(delta, std::fabs(double(got[i]) - ref[i]));
            scale = std::max(scale, std::fabs(double(ref[i])));
        }
        const bool untouched = std::all_of(got.begin(), got.begin() + H, [](float v) { return v == 19.0f; });
        if (!untouched || delta > 1e-5 + .02 * scale) { rc = fail("worker/local parity failed"); break; }
        std::printf("round=%d tokens=%d max_abs=%.9g reference_max=%.9g local_hit_preserved=%d\n", round, nt, delta, scale, untouched);
    }
    net.report();
    strata_network_destroy(backend);
    if (!expected.empty() && rc == 0 && error.empty()) return fail("expected failure did not occur");
    return rc;
}
