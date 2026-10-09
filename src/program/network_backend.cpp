// C ABI for the LAN worker. No model state, tokenizer, or network logic lives here.
#include "strata/kernels/cpu/expert_layout.hpp"
#include "strata/kernels/cpu/pool.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <vector>

#ifdef STRATA_NETWORK_GPU
#include "strata/kernels/iq_kernels.hpp"
#include <cuda_runtime.h>
#endif
#ifdef _WIN32
#define EXPORT extern "C" __declspec(dllexport)
#else
#define EXPORT extern "C" __attribute__((visibility("default")))
#endif
namespace cpu = strata::kernels::cpu;
namespace {
thread_local std::string last_error;
#ifdef STRATA_NETWORK_GPU
void check(cudaError_t rc) {
    if (rc != cudaSuccess) throw std::runtime_error(cudaGetErrorString(rc));
}
struct DeviceBuffer {
    void* p = nullptr;
    ~DeviceBuffer() { if (p) cudaFree(p); }
    void allocate(size_t n) { check(cudaMalloc(&p, n)); }
    void copy(const void* src, size_t n, cudaStream_t stream) {
        check(cudaMemcpyAsync(p, src, n, cudaMemcpyHostToDevice, stream));
    }
};
#endif
struct Backend {
    std::unique_ptr<cpu::ExpertPool> pool;
    std::vector<cpu::ActQ> act{cpu::MAXT};
    std::vector<uint8_t> nact = std::vector<uint8_t>(cpu::MAXT * cpu::kNativeActBytes);
#ifdef STRATA_NETWORK_GPU
    struct Cached {
        std::unique_ptr<DeviceBuffer> buffer;
        size_t bytes = 0;
        uint64_t used = 0;
    };
    bool gpu = false;
    int device = 0;
    size_t budget = 0, cached_bytes = 0;
    uint64_t epoch = 0;
    cudaStream_t stream = nullptr;
    DeviceBuffer x, xq, scratch, result, pointers, starts, counts, destinations, tokens;
    std::unordered_map<uint64_t, Cached> cache;
    ~Backend() {
        if (stream) { cudaStreamSynchronize(stream); cudaStreamDestroy(stream); }
    }
    void init_gpu(int dev, size_t mib) {
        gpu = true; device = dev; budget = mib << 20;
        check(cudaSetDevice(dev));
        check(cudaStreamCreate(&stream));
        x.allocate(cpu::MAXT * cpu::H * 4);
        xq.allocate(cpu::MAXT * cpu::H / 32 * 36);
        scratch.allocate(strata::kernels::native_expert_scratch_bytes(128, cpu::FF));
        result.allocate(128 * cpu::H * 4);
        pointers.allocate(128 * 8); starts.allocate(129 * 4); counts.allocate(4);
        destinations.allocate(128 * 4); tokens.allocate(128 * 4);
    }
    void run_gpu(const cpu::NativeFmt& f, int layer, const float* input, int nt, int n,
                 const int32_t* row, const int32_t* ids, const uint8_t* const* blobs, int k, float* out) {
        check(cudaSetDevice(device));
        if (!strata::kernels::native_expert_supported(f.gu_type, f.d_type, cpu::H, cpu::FF))
            throw std::runtime_error("GPU backend does not support this expert format");
        ++epoch;
        std::vector<unsigned long long> ptr;
        std::vector<int32_t> start, dst, tok;
        std::vector<int32_t> distinct;
        for (int i = 0; i < n; ++i)
            if (std::find(distinct.begin(), distinct.end(), ids[i]) == distinct.end()) distinct.push_back(ids[i]);
        // Protect every expert needed by this request before evicting any cache entry.
        for (auto e : distinct) {
            const uint64_t key = (uint64_t(layer) << 32) | uint32_t(e);
            auto it = cache.find(key);
            if (it != cache.end()) it->second.used = epoch;
        }
        for (auto e : distinct) {
            const uint64_t key = (uint64_t(layer) << 32) | uint32_t(e);
            if (cache.find(key) == cache.end()) {
                while (cached_bytes + f.bytes > budget) {
                    auto oldest = cache.end();
                    for (auto it = cache.begin(); it != cache.end(); ++it)
                        if (it->second.used != epoch && (oldest == cache.end() || it->second.used < oldest->second.used)) oldest = it;
                    if (oldest == cache.end()) throw std::runtime_error("GPU expert cache too small for one request");
                    cached_bytes -= oldest->second.bytes; cache.erase(oldest);
                }
                Cached entry;
                entry.bytes = f.bytes; entry.used = epoch;
                entry.buffer = std::make_unique<DeviceBuffer>(); entry.buffer->allocate(f.bytes);
                const int first = static_cast<int>(std::find(ids, ids + n, e) - ids);
                entry.buffer->copy(blobs[first], f.bytes, stream);
                cache.emplace(key, std::move(entry)); cached_bytes += f.bytes;
            }
            ptr.push_back(reinterpret_cast<unsigned long long>(cache.at(key).buffer->p));
            start.push_back(static_cast<int32_t>(dst.size()));
            for (int i = 0; i < n; ++i) if (ids[i] == e) { dst.push_back(i); tok.push_back(row[i] / k); }
        }
        start.push_back(static_cast<int32_t>(dst.size()));
        const int32_t groups = static_cast<int32_t>(ptr.size());
        x.copy(input, static_cast<size_t>(nt) * cpu::H * 4, stream);
        pointers.copy(ptr.data(), ptr.size() * 8, stream); starts.copy(start.data(), start.size() * 4, stream);
        counts.copy(&groups, 4, stream); destinations.copy(dst.data(), dst.size() * 4, stream); tokens.copy(tok.data(), tok.size() * 4, stream);
        strata::kernels::quantize_q8_1_rows(static_cast<float*>(x.p), nt, cpu::H, xq.p, stream);
        const auto layout = strata::kernels::native_expert_layout(f.gu_type, f.d_type, cpu::H, cpu::FF);
        strata::kernels::native_expert_grouped(layout, static_cast<unsigned long long*>(pointers.p),
            static_cast<int32_t*>(starts.p), static_cast<int32_t*>(counts.p), static_cast<int32_t*>(destinations.p),
            static_cast<int32_t*>(tokens.p), groups, n, xq.p, scratch.p, static_cast<float*>(result.p), stream);
        check(cudaGetLastError());
        check(cudaMemcpyAsync(out, result.p, static_cast<size_t>(n) * cpu::H * 4, cudaMemcpyDeviceToHost, stream));
        check(cudaStreamSynchronize(stream));
    }
#endif
};
}
EXPORT const char* strata_network_error() { return last_error.c_str(); }
// Raw IEEE-754 words also reject NaN/Inf under fast-math builds and need no
// aligned float pointer. Used for both wire activations and compact results.
EXPORT int strata_network_all_finite(const void* data, uint64_t bytes) {
    if (bytes % sizeof(float) != 0 || (!data && bytes != 0)) return 0;
    const auto* p = static_cast<const uint8_t*>(data);
    for (uint64_t i = 0; i < bytes; i += sizeof(float)) {
        uint32_t bits;
        std::memcpy(&bits, p + i, sizeof(bits));
        if ((bits & 0x7f800000u) == 0x7f800000u) return 0;
    }
    return 1;
}
// Compact a task's routed rows into one weighted FP32 vector per position.
// Accumulate in router order even when a caller supplied entries out of order.
EXPORT int strata_network_reduce(const float* parts, const int32_t* row, const float* weights,
                                 int nt, int n, int k, float* out) {
    try {
        if (!parts || !row || !weights || !out || nt < 1 || nt > cpu::MAXT || k < 1 || k > 10 || n < 1 || n > nt * k)
            throw std::runtime_error("invalid network reduction geometry");
        if (!strata_network_all_finite(parts, uint64_t(n) * cpu::H * sizeof(float)) ||
            !strata_network_all_finite(weights, uint64_t(n) * sizeof(float)))
            throw std::runtime_error("non-finite network reduction input or weight");
        int indices[cpu::MAXT * 10];
        std::fill(indices, indices + nt * k, -1);
        for (int i = 0; i < n; ++i) {
            if (row[i] < 0 || row[i] >= nt * k || indices[row[i]] >= 0 || !std::isfinite(weights[i]))
                throw std::runtime_error("invalid network reduction routing or weight");
            indices[row[i]] = i;
        }
        std::fill(out, out + nt * cpu::H, 0.0f);
        for (int r = 0; r < nt * k; ++r) {
            const int i = indices[r];
            if (i < 0) continue;
            float* dst = out + (r / k) * cpu::H;
            const float* src = parts + i * cpu::H;
            for (int h = 0; h < cpu::H; ++h) dst[h] = std::fma(src[h], weights[i], dst[h]);
        }
        if (!strata_network_all_finite(out, uint64_t(nt) * cpu::H * sizeof(float)))
            throw std::runtime_error("non-finite network reduction result");
        return 1;
    } catch (const std::exception& e) { last_error = e.what(); return 0; }
}
EXPORT void* strata_network_create(int workers, int device, uint64_t cache_mib) {
    try {
        if (!cpu::cpu_avx2_ok()) throw std::runtime_error("network backend requires an AVX2 CPU");
        auto b = std::make_unique<Backend>();
        if (device < 0) b->pool = std::make_unique<cpu::ExpertPool>(workers, true, true);
        else {
#ifdef STRATA_NETWORK_GPU
            if (cache_mib < 64 || cache_mib > 65536) throw std::runtime_error("GPU cache must be 64..65536 MiB");
            b->init_gpu(device, static_cast<size_t>(cache_mib));
#else
            (void) cache_mib;
            throw std::runtime_error("this library was built without CUDA/HIP; use --backend cpu");
#endif
        }
        return b.release();
    } catch (const std::exception& e) { last_error = e.what(); return nullptr; }
}
EXPORT void strata_network_destroy(void* p) { delete static_cast<Backend*>(p); }
EXPORT uint64_t strata_network_format_bytes(int gu, int down) {
    cpu::NativeFmt f;
    if (!cpu::native_fmt(gu, down, cpu::H, cpu::FF, f, last_error)) return 0;
    return f.bytes;
}
EXPORT int strata_network_compute(void* p, int gu, int down, int layer, const float* input, int nt, int n,
                                  const int32_t* row, const int32_t* ids, const uint8_t* const* blobs, int k, float* out) {
    try {
        if (!p || nt < 1 || nt > cpu::MAXT || k < 1 || k > 10 || n < 1 || n > 128 || layer < 0)
            throw std::runtime_error("invalid network task geometry");
        for (int i = 0; i < n; ++i) {
            if (row[i] < 0 || row[i] >= nt * k || ids[i] < 0 || ids[i] >= cpu::NE || !blobs[i])
                throw std::runtime_error("invalid expert entry");
            for (int j = 0; j < i; ++j)
                if (row[j] == row[i] || (ids[j] == ids[i] && row[j] / k == row[i] / k))
                    throw std::runtime_error("duplicate expert entry");
        }
        cpu::NativeFmt f;
        if (!cpu::native_fmt(gu, down, cpu::H, cpu::FF, f, last_error)) return 0;
        auto& b = *static_cast<Backend*>(p);
#ifdef STRATA_NETWORK_GPU
        if (b.gpu) { b.run_gpu(f, layer, input, nt, n, row, ids, blobs, k, out); return 1; }
#endif
        for (int t = 0; t < nt; ++t) {
            if (cpu::q2_native_kernels(gu)) cpu::act_quant_any(input + t * cpu::H, cpu::H, b.act[static_cast<size_t>(t)]);
            else cpu::native_quant_act(f, input + t * cpu::H, b.nact.data() + t * cpu::kNativeActBytes);
        }
        std::vector<cpu::ExpertJobMulti> jobs;
        std::vector<int32_t> distinct;
        for (int i = 0; i < n; ++i) {
            auto it = std::find(distinct.begin(), distinct.end(), ids[i]);
            if (it == distinct.end()) { distinct.push_back(ids[i]); jobs.emplace_back(); jobs.back().blob = blobs[i]; }
            const auto index = static_cast<size_t>(std::find(distinct.begin(), distinct.end(), ids[i]) - distinct.begin());
            auto& job = jobs[index];
            const int t = row[i] / k;
            if (job.nt >= cpu::MAXT) throw std::runtime_error("too many tokens for one expert");
            job.act[job.nt] = &b.act[static_cast<size_t>(t)];
            job.nact[job.nt] = b.nact.data() + t * cpu::kNativeActBytes;
            job.out[job.nt++] = out + i * cpu::H;
        }
        if (jobs.size() > cpu::ExpertPool::kMaxSplitMulti) throw std::runtime_error("too many distinct experts");
        b.pool->run_split_multi_native(f, jobs.data(), static_cast<int>(jobs.size()));
        return 1;
    } catch (const std::exception& e) {
#ifdef STRATA_NETWORK_GPU
        // Transfers may still refer to the caller's buffers when a later allocation or launch fails.
        if (p && static_cast<Backend*>(p)->gpu && static_cast<Backend*>(p)->stream)
            cudaStreamSynchronize(static_cast<Backend*>(p)->stream);
#endif
        last_error = e.what(); return 0;
    }
}
