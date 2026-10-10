#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <pdh.h>
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
#include <algorithm>
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
    PDH_HQUERY disk_query = nullptr;
    PDH_HCOUNTER disk_read = nullptr, disk_write = nullptr;
#else
    void *library = nullptr;
#endif
    void *gpu = nullptr;
    std::vector<std::pair<int, void *>> devices;
#ifndef _WIN32
    std::vector<std::pair<int, std::filesystem::path>> amd_devices;
    uint64_t last_disk_read = 0, last_disk_write = 0;
    std::chrono::steady_clock::time_point disk_at{};
#endif
    bool initialized = false, stopping = false;
    int (*init)() = nullptr, (*shutdown)() = nullptr;
    int (*handle)(unsigned int, void **) = nullptr;
    int (*name)(void *, char *, unsigned int) = nullptr;
    int (*memory)(void *, Memory *) = nullptr;
    int (*util)(void *, Utilization *) = nullptr;
    int (*temperature)(void *, int, unsigned int *) = nullptr;
    int (*power)(void *, unsigned int *) = nullptr;
    int (*power_limit)(void *, unsigned int *) = nullptr;
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
    Impl(int index, bool amd, std::function<J()> callback, const std::vector<int> &requested)
        : extra(std::move(callback)) {
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
        DWORD logical_bytes = 0;
        GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &logical_bytes);
        std::vector<unsigned char> logical(logical_bytes);
        if (logical_bytes &&
            GetLogicalProcessorInformationEx(
                RelationProcessorCore,
                reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(logical.data()), &logical_bytes)) {
            int cores = 0;
            for (size_t at = 0; at + 8 <= logical_bytes;) {
                auto *entry = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(logical.data() + at);
                if (entry->Size < 8 || at + entry->Size > logical_bytes)
                    break;
                ++cores;
                at += entry->Size;
            }
            fixed["cores"] = cores;
        }
        if (PdhOpenQueryW(nullptr, 0, &disk_query) == ERROR_SUCCESS) {
            PdhAddEnglishCounterW(disk_query, L"\\PhysicalDisk(_Total)\\Disk Read Bytes/sec", 0, &disk_read);
            PdhAddEnglishCounterW(disk_query, L"\\PhysicalDisk(_Total)\\Disk Write Bytes/sec", 0,
                                  &disk_write);
            PdhCollectQueryData(disk_query);
        }
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
        if (amd) {
            std::error_code error;
            std::vector<std::filesystem::path> cards;
            for (const auto &entry : std::filesystem::directory_iterator("/sys/class/drm", error)) {
                const auto name = entry.path().filename().string();
                if (name.starts_with("card") && name.find('-') == std::string::npos &&
                    std::filesystem::exists(entry.path() / "device/mem_info_vram_total"))
                    cards.push_back(entry.path() / "device");
            }
            std::sort(cards.begin(), cards.end());
            const auto selected = requested.empty() ? std::vector<int>{index} : requested;
            for (int i : selected)
                if (i >= 0 && static_cast<size_t>(i) < cards.size())
                    amd_devices.emplace_back(i, cards[i]);
        }
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
            symbol(power_limit, "nvmlDeviceGetEnforcedPowerLimit");
            symbol(pcie, "nvmlDeviceGetPcieThroughput");
            initialized = init && shutdown && init() == 0;
            if (initialized && handle) {
                const auto selected = requested.empty() ? std::vector<int>{index} : requested;
                std::string names;
                for (int i : selected) {
                    void *dev = nullptr;
                    if (i >= 0 && handle(static_cast<unsigned int>(i), &dev) == 0) {
                        devices.emplace_back(i, dev);
                        char text[256]{};
                        if (name && name(dev, text, sizeof(text)) == 0) {
                            if (!names.empty())
                                names += " + ";
                            names += text;
                        }
                    }
                }
                if (!devices.empty()) {
                    gpu = devices.front().second;
                    fixed["gpu_count"] = devices.size();
                    if (!names.empty())
                        fixed["gpu_name"] = names;
                }
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
        if (disk_query && PdhCollectQueryData(disk_query) == ERROR_SUCCESS) {
            PDH_FMT_COUNTERVALUE value{};
            if (disk_read &&
                PdhGetFormattedCounterValue(disk_read, PDH_FMT_DOUBLE, nullptr, &value) == ERROR_SUCCESS &&
                value.CStatus == ERROR_SUCCESS)
                s["disk_read_mb"] = value.doubleValue / (1u << 20);
            if (disk_write &&
                PdhGetFormattedCounterValue(disk_write, PDH_FMT_DOUBLE, nullptr, &value) == ERROR_SUCCESS &&
                value.CStatus == ERROR_SUCCESS)
                s["disk_write_mb"] = value.doubleValue / (1u << 20);
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
        std::ifstream disks("/proc/diskstats");
        std::string disk_line;
        uint64_t reads = 0, writes = 0;
        while (std::getline(disks, disk_line)) {
            std::istringstream row(disk_line);
            int major = 0, minor = 0;
            std::string device;
            uint64_t nread = 0, merged = 0, sectors = 0, time = 0, nwrite = 0, wmerge = 0, wsectors = 0;
            if (row >> major >> minor >> device >> nread >> merged >> sectors >> time >> nwrite >> wmerge >>
                wsectors) {
                if (!std::filesystem::exists(std::filesystem::path("/sys/class/block") / device /
                                             "partition")) {
                    reads += sectors * 512;
                    writes += wsectors * 512;
                }
            }
        }
        const auto now_ = std::chrono::steady_clock::now();
        if (disk_at.time_since_epoch().count() != 0) {
            const auto dt = std::chrono::duration<double>(now_ - disk_at).count();
            if (dt > 0 && reads >= last_disk_read && writes >= last_disk_write) {
                s["disk_read_mb"] = (reads - last_disk_read) / dt / (1u << 20);
                s["disk_write_mb"] = (writes - last_disk_write) / dt / (1u << 20);
            }
        }
        last_disk_read = reads;
        last_disk_write = writes;
        disk_at = now_;
#endif
        if (previous_total && total > previous_total && idle >= previous_idle)
            s["cpu"] = 100. * (1. - double(idle - previous_idle) / double(total - previous_total));
        previous_total = total;
        previous_idle = idle;
        J gpu_readings = J::array();
        for (const auto &[index, dev] : devices) {
            const auto gpu = dev;
            J card{{"index", index}};
            Memory m{};
            Utilization u{};
            unsigned int n = 0;
            if (memory && memory(gpu, &m) == 0) {
                card["mem_used"] = m.used;
                card["mem_total"] = m.total;
                card["mem_free"] = m.free;
            }
            if (util && util(gpu, &u) == 0)
                card["util"] = u.gpu;
            if (temperature && temperature(gpu, 0, &n) == 0)
                card["temp"] = n;
            if (power && power(gpu, &n) == 0)
                card["power"] = n / 1000.;
            if (power_limit && power_limit(gpu, &n) == 0)
                card["power_limit"] = n / 1000.;
            if (pcie && pcie(gpu, 1, &n) == 0)
                card["pcie_rx_mb"] = n / 1024.;
            if (pcie && pcie(gpu, 0, &n) == 0)
                card["pcie_tx_mb"] = n / 1024.;
            gpu_readings.push_back(card);
        }
#ifndef _WIN32
        for (const auto &[index, path] : amd_devices) {
            auto read = [](const std::filesystem::path &file) -> J {
                std::ifstream in(file);
                double value = 0;
                if (in >> value)
                    return value;
                return nullptr;
            };
            J card{{"index", index},
                   {"util", read(path / "gpu_busy_percent")},
                   {"mem_used", read(path / "mem_info_vram_used")},
                   {"mem_total", read(path / "mem_info_vram_total")}};
            if (card["mem_total"].is_number() && card["mem_used"].is_number())
                card["mem_free"] = card["mem_total"].get<double>() - card["mem_used"].get<double>();
            std::error_code error;
            for (const auto &hw : std::filesystem::directory_iterator(path / "hwmon", error)) {
                auto temperature = read(hw.path() / "temp1_input"),
                     power_ = read(hw.path() / "power1_average"), limit = read(hw.path() / "power1_cap");
                if (temperature.is_number())
                    card["temp"] = temperature.get<double>() / 1000.;
                if (power_.is_number())
                    card["power"] = power_.get<double>() / 1000000.;
                if (limit.is_number())
                    card["power_limit"] = limit.get<double>() / 1000000.;
                break;
            }
            gpu_readings.push_back(card);
        }
#endif
        if (!gpu_readings.empty()) {
            if (gpu_readings.size() > 1)
                s["gpus"] = gpu_readings;
            s["gpu_mem_free"] = gpu_readings[0].value("mem_free", J(nullptr));
            for (auto key : {"util", "mem_used", "mem_total", "temp", "power", "power_limit", "pcie_rx_mb",
                             "pcie_tx_mb"}) {
                double sum = 0, max = -1;
                int count = 0;
                for (const auto &card : gpu_readings) {
                    const auto v = card.value(key, J(nullptr));
                    if (v.is_number()) {
                        const auto n = v.get<double>();
                        sum += n;
                        max = std::max(max, n);
                        ++count;
                    }
                }
                if (count)
                    s["gpu_" + std::string(key)] = std::string(key) == "util"   ? sum / count
                                                   : std::string(key) == "temp" ? max
                                                                                : sum;
            }
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
        if (disk_query)
            PdhCloseQuery(disk_query);
        if (library)
            FreeLibrary(library);
#else
        if (library)
            dlclose(library);
#endif
    }
};
ServerTelemetry::ServerTelemetry(int gpu, bool amd, std::function<J()> extra, const std::vector<int> &indices)
    : impl_(std::make_unique<Impl>(gpu, amd, std::move(extra), indices)) {}
ServerTelemetry::~ServerTelemetry() = default;
void ServerTelemetry::close() {
    if (!impl_)
        return;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->stopping = true;
        impl_->changed.notify_all();
    }
    if (impl_->sampler.joinable())
        impl_->sampler.join();
}
J ServerTelemetry::snapshot() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return {{"now", impl_->now}, {"history", impl_->history}, {"static", impl_->fixed}};
}
} // namespace strata::runtime
