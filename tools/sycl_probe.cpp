// Standalone Intel GPU probe: no model, oneMKL, or Strata build required.
#include <sycl/sycl.hpp>
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <vector>

static void print_selector(const char* name) {
#if defined(_MSC_VER)
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, name) == 0 && value)
        std::cout << name << '=' << value << " (may restrict visible devices)\n";
    std::free(value);
#else
    if (const char* value = std::getenv(name))
        std::cout << name << '=' << value << " (may restrict visible devices)\n";
#endif
}

static void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

static void buffer_test(sycl::queue& q) {
    constexpr std::size_t n = 65536;
    std::vector<float> input(n), output(n);
    for (std::size_t i = 0; i < n; ++i) input[i] = float(i % 1024);
    {
        sycl::buffer<float> a(input.data(), sycl::range<1>(n));
        sycl::buffer<float> b(output.data(), sycl::range<1>(n));
        q.submit([&](sycl::handler& h) {
            auto src = a.get_access<sycl::access::mode::read>(h);
            auto dst = b.get_access<sycl::access::mode::write>(h);
            h.parallel_for(sycl::range<1>(n), [=](sycl::id<1> i) {
                dst[i] = src[i] * 2.0f + 7.0f;
            });
        });
        q.wait_and_throw();
    } // Copy the result back before checking it.
    for (std::size_t i = 0; i < n; ++i)
        require(output[i] == input[i] * 2.0f + 7.0f, "FP32 result mismatch");
    std::cout << "  PASS FP32 buffer kernel: " << n << " results checked\n";
}

// Keep USM alive until all submitted work has finished, including on failure.
struct allocation {
    sycl::queue& q;
    float* ptr;
    ~allocation() {
        try { q.wait(); } catch (...) {}
        sycl::free(ptr, q);
    }
};

static void usm_test(sycl::queue& q, bool shared) {
    constexpr std::size_t n = 4096;
    allocation a{q, shared ? sycl::malloc_shared<float>(n, q)
                           : sycl::malloc_device<float>(n, q)};
    require(a.ptr != nullptr, "USM allocation failed");
    std::vector<float> host(n);
    for (std::size_t i = 0; i < n; ++i) host[i] = float(i);
    q.memcpy(a.ptr, host.data(), n * sizeof(float)).wait_and_throw();
    float* p = a.ptr;
    q.parallel_for(sycl::range<1>(n), [=](sycl::id<1> i) {
        p[i] = p[i] + 3.0f;
    }).wait_and_throw();
    if (shared) std::copy(p, p + n, host.begin());
    else q.memcpy(host.data(), p, n * sizeof(float)).wait_and_throw();
    for (std::size_t i = 0; i < n; ++i)
        require(host[i] == float(i) + 3.0f, "USM result mismatch");
    std::cout << "  PASS USM " << (shared ? "shared" : "device")
              << ": " << n << " results checked\n";
}

static void fp16_test(sycl::queue& q) {
    std::vector<float> output(64);
    {
        sycl::buffer<float> b(output.data(), sycl::range<1>(output.size()));
        q.submit([&](sycl::handler& h) {
            auto dst = b.get_access<sycl::access::mode::write>(h);
            h.parallel_for(sycl::range<1>(64), [=](sycl::id<1> i) {
                sycl::half x = sycl::half(float(i[0] + 1));
                dst[i] = float(x * sycl::half(2.0f));
            });
        });
        q.wait_and_throw();
    }
    for (std::size_t i = 0; i < output.size(); ++i)
        require(output[i] == float(2 * (i + 1)), "FP16 result mismatch");
    std::cout << "  PASS FP16 kernel: 64 results checked\n";
}

static void subgroup_test(sycl::queue& q) {
    std::vector<int> output(256);
    {
        sycl::buffer<int> b(output.data(), sycl::range<1>(output.size()));
        q.submit([&](sycl::handler& h) {
            auto dst = b.get_access<sycl::access::mode::write>(h);
            h.parallel_for(sycl::nd_range<1>(256, 32),
                [=](sycl::nd_item<1> item) [[sycl::reqd_sub_group_size(32)]] {
                    auto sg = item.get_sub_group();
                    int value = int(sg.get_local_linear_id()) + 1;
                    int sum = sycl::reduce_over_group(sg, value, sycl::plus<int>());
                    dst[item.get_global_id(0)] = sum;
                });
        });
        q.wait_and_throw();
    }
    for (int result : output) require(result == 528, "32-lane reduction mismatch");
    std::cout << "  PASS 32-lane sub-group reduction: 256 results checked\n";
}

static bool probe(const sycl::device& device) {
    using namespace sycl::info;
    std::cout << "\nIntel GPU: " << device.get_info<device::name>() << '\n'
              << "  Platform: " << device.get_platform().get_info<platform::name>() << '\n'
              << "  Driver: " << device.get_info<device::driver_version>() << '\n'
              << "  Reported global memory: "
              << device.get_info<device::global_mem_size>() / (1024 * 1024) << " MiB\n"
              << "    (Runtime limit; not a measurement of free/dedicated VRAM.)\n"
              << "  Local memory: " << device.get_info<device::local_mem_size>() / 1024 << " KiB\n"
              << "  Max work-group size: " << device.get_info<device::max_work_group_size>() << '\n';
    auto sizes = device.get_info<device::sub_group_sizes>();
    std::cout << "  Supported sub-group sizes:";
    for (auto size : sizes) std::cout << ' ' << size;
    std::cout << '\n';
    const auto aspect = [&](const char* name, sycl::aspect value) {
        std::cout << "  " << name << ": " << (device.has(value) ? "yes" : "no") << '\n';
    };
    aspect("FP16", sycl::aspect::fp16);
    aspect("FP64", sycl::aspect::fp64);
    aspect("USM device", sycl::aspect::usm_device_allocations);
    aspect("USM shared", sycl::aspect::usm_shared_allocations);
    aspect("USM host", sycl::aspect::usm_host_allocations);
    aspect("USM atomic shared", sycl::aspect::usm_atomic_shared_allocations);
    aspect("USM atomic host", sycl::aspect::usm_atomic_host_allocations);
#ifdef SYCL_EXT_ONEAPI_MATRIX
    try {
        auto combinations = device.get_info<sycl::ext::oneapi::experimental::info::device::matrix_combinations>();
        std::cout << "  joint_matrix combinations: " << combinations.size() << " (query only)\n";
    } catch (const sycl::exception& e) {
        std::cout << "  joint_matrix query unavailable: " << e.what() << '\n';
    }
#endif
    sycl::queue q(device, [](sycl::exception_list errors) {
        // SYCL's default handler can terminate the process; propagate to our test report instead.
        if (errors.begin() != errors.end()) std::rethrow_exception(*errors.begin());
    });
    bool ok = true;
    const auto run = [&](const char* name, auto test) {
        try { test(); }
        catch (const std::exception& e) {
            std::cout << "  FAIL " << name << ": " << e.what() << '\n';
            ok = false;
        }
    };
    run("FP32 buffer", [&] { buffer_test(q); });
    if (device.has(sycl::aspect::usm_device_allocations))
        run("USM device", [&] { usm_test(q, false); });
    else std::cout << "  SKIP USM device: unsupported\n";
    if (device.has(sycl::aspect::usm_shared_allocations))
        run("USM shared", [&] { usm_test(q, true); });
    else std::cout << "  SKIP USM shared: unsupported\n";
    if (device.has(sycl::aspect::fp16)) run("FP16", [&] { fp16_test(q); });
    else std::cout << "  SKIP FP16: unsupported\n";
    if (std::find(sizes.begin(), sizes.end(), 32) != sizes.end() &&
        device.get_info<device::max_work_group_size>() >= 32)
        run("32-lane sub-group", [&] { subgroup_test(q); });
    else std::cout << "  SKIP 32-lane sub-group: unsupported (Strata kernels require it)\n";
    return ok;
}

int main() {
    std::cout << "Intel GPU SYCL probe (small allocations; no model required)\n";
    for (const char* name : {"ONEAPI_DEVICE_SELECTOR", "SYCL_DEVICE_FILTER"})
        print_selector(name);
    try {
        int found = 0, failed = 0;
        for (const auto& platform : sycl::platform::get_platforms()) {
            for (const auto& device : platform.get_devices()) {
                std::cout << "Visible device: " << device.get_info<sycl::info::device::name>()
                          << " [" << platform.get_info<sycl::info::platform::name>() << "]\n";
                if (!device.is_gpu() || device.get_info<sycl::info::device::vendor_id>() != 0x8086) continue;
                ++found;
                try { if (!probe(device)) ++failed; }
                catch (const std::exception& e) {
                    std::cout << "  FAIL device probe: " << e.what() << '\n';
                    ++failed;
                }
            }
        }
        if (!found) {
            std::cout << "FAIL: no Intel GPU visible to SYCL. Check driver and device selectors.\n";
            return 2;
        }
        std::cout << "\nIntel GPU backend entries tested: " << found << ", failed: " << failed << '\n'
                  << "The same GPU can appear through both Level Zero and OpenCL.\n"
                  << "PASS means the executed tests passed; unsupported features are skipped.\n"
                  << "This does not validate Strata inference, XMX kernels, or model memory capacity.\n";
        return failed ? 1 : 0;
    } catch (const std::exception& e) {
        std::cerr << "SYCL enumeration failed: " << e.what() << '\n';
        return 3;
    }
}
