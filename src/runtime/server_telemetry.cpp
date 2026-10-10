#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#include "strata/runtime/path.hpp"
#include "strata/runtime/server_telemetry.hpp"
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>
namespace strata::runtime {
using J = TextJson;
struct ServerTelemetry::Impl {
    // NVML's public C ABI (memory v1, utilization): loaded from the installed
    // driver. No toolkit, Python package, or subprocess is required.
    struct Memory {
        unsigned long long total, free, used;
    };
    struct Utilization {
        unsigned int gpu, memory;
    };
#ifdef _WIN32
    HMODULE library = nullptr;
#else
    void *library = nullptr;
#endif
    void *gpu = nullptr;
    bool initialized = false, stopping = false;
    int (*init)() = nullptr, (*shutdown)() = nullptr;
    int (*handle)(unsigned int, void **) = nullptr;
    int (*name)(void *, char *, unsigned int) = nullptr;
    int (*memory)(void *, Memory *) = nullptr;
    int (*util)(void *, Utilization *) = nullptr;
    int (*temperature)(void *, int, unsigned int *) = nullptr;
    int (*power)(void *, unsigned int *) = nullptr;
    int (*pcie)(void *, int, unsigned int *) = nullptr;
    std::mutex mutex;
    std::condition_variable changed;
    std::thread sampler;
    std::function<J()> extra;
    J now = J::object(), history = J::object(), fixed = J::object();
    uint64_t previous_total = 0, previous_idle = 0;
    template <class T> void symbol(T &function, const char *key) {
#ifdef _WIN32
        function = reinterpret_cast<T>(GetProcAddress(library, key));
#else
        function = reinterpret_cast<T>(dlsym(library, key));
#endif
    }
    Impl(int index, bool amd, std::function<J()> callback) : extra(std::move(callback)) {
        fixed = {{"gpu_name", nullptr},
                 {"gpu_count", 0},
                 {"cpu_name", nullptr},
                 {"threads", std::thread::hardware_concurrency()},
                 {"native", true}};
#ifdef _WIN32
        wchar_t cpu[256]{};
        DWORD bytes = sizeof(cpu);
        if (RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                         L"ProcessorNameString", RRF_RT_REG_SZ, nullptr, cpu, &bytes) == ERROR_SUCCESS)
            fixed["cpu_name"] = trim_text(path_utf8(std::filesystem::path(cpu)));
        if (!amd)
            library = LoadLibraryExW(L"nvml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (amd)
            fixed["gpu_note"] = "no GPU load or VRAM readings for AMD cards on Windows yet; the engine's "
                                "VRAM figures are in its log";
#else
        std::ifstream cpu("/proc/cpuinfo");
        std::string line;
        while (std::getline(cpu, line))
            if (line.starts_with("model name")) {
                fixed["cpu_name"] = trim_text(line.substr(line.find(':') + 1));
                break;
            }
        if (!amd)
            library = dlopen("libnvidia-ml.so.1", RTLD_NOW | RTLD_LOCAL);
#endif
        if (library) {
            symbol(init, "nvmlInit_v2");
            symbol(shutdown, "nvmlShutdown");
            symbol(handle, "nvmlDeviceGetHandleByIndex_v2");
            symbol(name, "nvmlDeviceGetName");
            symbol(memory, "nvmlDeviceGetMemoryInfo");
            symbol(util, "nvmlDeviceGetUtilizationRates");
            symbol(temperature, "nvmlDeviceGetTemperature");
            symbol(power, "nvmlDeviceGetPowerUsage");
            symbol(pcie, "nvmlDeviceGetPcieThroughput");
            initialized = init && shutdown && init() == 0;
            if (initialized && handle && index >= 0 && handle(static_cast<unsigned int>(index), &gpu) == 0) {
                char text[256]{};
                if (name && name(gpu, text, sizeof(text)) == 0)
                    fixed["gpu_name"] = text;
                fixed["gpu_count"] = 1;
            }
        }
        sampler = std::thread([this] {
            for (;;) {
                auto values = sample();
                std::unique_lock<std::mutex> lock(mutex);
                now = values;
                for (auto key : {"gpu_util", "gpu_mem_used", "gpu_temp", "gpu_power", "gpu_pcie_rx_mb", "cpu",
                                 "ram_used", "disk_read_mb", "tok_s", "prefill_tok_s_mean"}) {
                    if (!history.contains(key))
                        history[key] = J::array();
                    auto &h = history[key];
                    h.push_back(values.value(key, J(nullptr)));
                    if (h.size() > 120)
                        h.erase(h.begin());
                }
                if (changed.wait_for(lock, std::chrono::seconds(1), [this] { return stopping; }))
                    break;
            }
        });
    }
    J sample() {
        J s = J::object();
        uint64_t total = 0, idle = 0;
#ifdef _WIN32
        MEMORYSTATUSEX ram{};
        ram.dwLength = sizeof(ram);
        if (GlobalMemoryStatusEx(&ram)) {
            s["ram_used"] = ram.ullTotalPhys - ram.ullAvailPhys;
            s["ram_total"] = ram.ullTotalPhys;
        }
        FILETIME a{}, b{}, c{};
        const auto count = [](const FILETIME &t) {
            return (uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime;
        };
        if (GetSystemTimes(&a, &b, &c)) {
            idle = count(a);
            total = count(b) + count(c);
        }
#else
        std::ifstream stat("/proc/stat");
        std::string label, line;
        std::getline(stat, line);
        std::istringstream in(line);
        in >> label;
        uint64_t value = 0;
        for (int i = 0; in >> value; ++i) {
            total += value;
            if (i == 3 || i == 4)
                idle += value;
        }
        std::ifstream mem("/proc/meminfo");
        uint64_t capacity = 0, available = 0;
        while (std::getline(mem, line)) {
            std::istringstream row(line);
            row >> label >> value;
            if (label == "MemTotal:")
                capacity = value * 1024;
            if (label == "MemAvailable:")
                available = value * 1024;
        }
        if (capacity) {
            s["ram_total"] = capacity;
            s["ram_used"] = capacity - available;
        }
#endif
        if (previous_total && total > previous_total && idle >= previous_idle)
            s["cpu"] = 100. * (1. - double(idle - previous_idle) / double(total - previous_total));
        previous_total = total;
        previous_idle = idle;
        if (gpu) {
            Memory m{};
            Utilization u{};
            unsigned int n = 0;
            if (memory && memory(gpu, &m) == 0) {
                s["gpu_mem_used"] = m.used;
                s["gpu_mem_total"] = m.total;
            }
            if (util && util(gpu, &u) == 0)
                s["gpu_util"] = u.gpu;
            if (temperature && temperature(gpu, 0, &n) == 0)
                s["gpu_temp"] = n;
            if (power && power(gpu, &n) == 0)
                s["gpu_power"] = n / 1000.;
            if (pcie && pcie(gpu, 1, &n) == 0)
                s["gpu_pcie_rx_mb"] = n / 1024.;
            if (pcie && pcie(gpu, 0, &n) == 0)
                s["gpu_pcie_tx_mb"] = n / 1024.;
        }
        try {
            if (extra)
                s.update(extra());
        } catch (...) { /* monitoring must not interrupt inference */
        }
        return s;
    }
    ~Impl() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopping = true;
            changed.notify_all();
        }
        if (sampler.joinable())
            sampler.join();
        if (initialized)
            shutdown();
#ifdef _WIN32
        if (library)
            FreeLibrary(library);
#else
        if (library)
            dlclose(library);
#endif
    }
};
ServerTelemetry::ServerTelemetry(int gpu, bool amd, std::function<J()> extra)
    : impl_(std::make_unique<Impl>(gpu, amd, std::move(extra))) {}
ServerTelemetry::~ServerTelemetry() = default;
J ServerTelemetry::snapshot() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return {{"now", impl_->now}, {"history", impl_->history}, {"static", impl_->fixed}};
}
} // namespace strata::runtime
