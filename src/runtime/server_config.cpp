#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#include "strata/runtime/server_engine.hpp"
#include "strata/runtime/path.hpp"
#include <algorithm>
#include <array>
#include <fstream>
#include <sstream>
namespace strata::runtime {
namespace {
using J = TextJson;
bool has(const std::vector<std::string> &args, const char *key) {
    return std::find(args.begin(), args.end(), key) != args.end();
}
std::string joined(const std::vector<int> &values) {
    std::string out;
    for (int v : values) {
        if (!out.empty())
            out += ',';
        out += std::to_string(v);
    }
    return out;
}
std::optional<std::pair<uint32_t, uint32_t>> profile(const std::filesystem::path &path) {
    std::ifstream file(path, std::ios::binary);
    std::array<unsigned char, 24> bytes{};
    file.read(reinterpret_cast<char *>(bytes.data()), bytes.size());
    if (!file || !std::equal(bytes.begin(), bytes.begin() + 4, "STRP"))
        return {};
    auto u32 = [&](size_t p) {
        return uint32_t(bytes[p]) | (uint32_t(bytes[p + 1]) << 8) | (uint32_t(bytes[p + 2]) << 16) |
               (uint32_t(bytes[p + 3]) << 24);
    };
    std::error_code error;
    const auto n = std::filesystem::file_size(path, error);
    if (error || n < 24 + uint64_t(u32(20)) * 4)
        return {};
    return std::pair{u32(8), u32(12)};
}
std::map<int, uint64_t> cuda_scores(const std::vector<int> &indices) {
    std::map<int, uint64_t> result;
#ifdef _WIN32
    auto library = LoadLibraryExW(L"nvcuda.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!library)
        return result;
    using Init = int(__stdcall *)(unsigned int);
    using Device = int(__stdcall *)(int *, int);
    using Attr = int(__stdcall *)(int *, int, int);
    auto init = reinterpret_cast<Init>(GetProcAddress(library, "cuInit"));
    auto device = reinterpret_cast<Device>(GetProcAddress(library, "cuDeviceGet"));
    auto attr = reinterpret_cast<Attr>(GetProcAddress(library, "cuDeviceGetAttribute"));
#else
    auto library = dlopen("libcuda.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!library)
        return result;
    using Init = int (*)(unsigned int);
    using Device = int (*)(int *, int);
    using Attr = int (*)(int *, int, int);
    auto init = reinterpret_cast<Init>(dlsym(library, "cuInit"));
    auto device = reinterpret_cast<Device>(dlsym(library, "cuDeviceGet"));
    auto attr = reinterpret_cast<Attr>(dlsym(library, "cuDeviceGetAttribute"));
#endif
    if (init && device && attr && init(0) == 0)
        for (int i : indices) {
            int d = 0, sm = 0, clock = 0;
            if (device(&d, i) == 0 && attr(&sm, 16, d) == 0 && attr(&clock, 13, d) == 0)
                result[i] = uint64_t(sm) * clock;
        }
#ifdef _WIN32
    FreeLibrary(library);
#else
    dlclose(library);
#endif
    return result;
}
} // namespace
std::vector<int> server_gpu_indices(const J &cfg) {
    auto given = cfg.value("gpu", J(nullptr));
    if (given.is_null() || given == "")
        return {};
    if (!given.is_array()) {
        if (given.is_string()) {
            std::istringstream in(given.get<std::string>());
            given = J::array();
            std::string item;
            while (std::getline(in, item, ','))
                if (!trim_text(item).empty())
                    given.push_back(trim_text(item));
        } else
            given = J::array({given});
    }
    std::vector<int> result;
    for (const auto &item : given) {
        if (!item.is_number_integer() && !item.is_string())
            throw std::invalid_argument("GPU indices must be integers");
        const auto raw = item.is_string() ? item.get<std::string>() : item.dump();
        size_t used = 0;
        const auto n = std::stoi(raw, &used);
        if (used != raw.size() || n < 0)
            throw std::invalid_argument("invalid GPU index");
        result.push_back(n);
    }
    return result;
}
std::vector<std::string> server_aliases(const J &cfg) {
    auto aliases = cfg.value("aliases", J::array());
    if (aliases.is_null())
        return {};
    if (aliases.is_string()) {
        std::istringstream in(aliases.get<std::string>());
        aliases = J::array();
        std::string name;
        while (std::getline(in, name, ','))
            aliases.push_back(name);
    }
    if (!aliases.is_array())
        throw std::invalid_argument("aliases must be an array or comma-separated string");
    std::vector<std::string> result;
    for (const auto &alias : aliases) {
        if (!alias.is_string())
            throw std::invalid_argument("aliases must contain strings");
        const auto name = trim_text(alias.get<std::string>());
        if (!name.empty() && name != cfg.value("model_name", "qwen3.8-flash-next") &&
            std::find(result.begin(), result.end(), name) == result.end())
            result.push_back(name);
    }
    return result;
}
std::string server_layer_split(const J &cfg) {
    auto value = cfg.value("layer_split", J(nullptr));
    if (value.is_null() || value == "auto" || value == "")
        return "auto";
    auto parts = value.is_array() ? value : J::array();
    if (!value.is_array()) {
        const auto raw = value.is_string() ? value.get<std::string>() : value.dump();
        std::istringstream in(raw);
        std::string item;
        while (std::getline(in, item, ','))
            parts.push_back(trim_text(item));
    }
    std::vector<int> layers;
    for (const auto &part : parts) {
        if (!part.is_number_integer() && !part.is_string())
            throw std::invalid_argument("layer_split must contain rising first-layer indices");
        const auto s = part.is_string() ? part.get<std::string>() : part.dump();
        size_t used = 0;
        const auto n = std::stoi(s, &used);
        if (used != s.size() || n < 2 || (!layers.empty() && n <= layers.back()))
            throw std::invalid_argument("layer_split must contain rising first-layer indices from 2");
        layers.push_back(n);
    }
    const auto gpus = server_gpu_indices(cfg);
    if (layers.empty() || (gpus.size() > 1 && layers.size() != gpus.size() - 1))
        throw std::invalid_argument("layer_split requires one first-layer index per later GPU");
    return joined(layers);
}
std::vector<int> server_ordered_gpus(const J &cfg) {
    auto indices = server_gpu_indices(cfg);
    const auto args = cfg.at("args").get<std::vector<std::string>>();
    if (indices.size() < 2 || cfg.value("gpu_order", "") == "as_given" || has(args, "--peer-device") ||
        has(args, "--layer-split") || server_layer_split(cfg) != "auto")
        return indices;
    std::map<int, uint64_t> scores;
    if (cfg.value("backend", "") == "hip") {
#ifndef _WIN32
        const auto root = std::filesystem::path("/sys/class/kfd/kfd/topology/nodes");
        std::error_code error;
        std::vector<std::filesystem::path> nodes;
        for (const auto &entry : std::filesystem::directory_iterator(root, error))
            nodes.push_back(entry.path());
        std::sort(nodes.begin(), nodes.end(), [](const auto &a, const auto &b) {
            return std::stoi(a.filename().string()) < std::stoi(b.filename().string());
        });
        int ordinal = 0;
        for (const auto &node : nodes) {
            std::ifstream file(node / "properties");
            std::map<std::string, uint64_t> values;
            std::string key;
            uint64_t n = 0;
            while (file >> key >> n)
                values[key] = n;
            if (!values["simd_count"])
                continue;
            scores[ordinal++] = values["simd_count"] * values["max_engine_clk_fcompute"];
        }
#endif
    } else
        scores = cuda_scores(indices);
    if (std::all_of(indices.begin(), indices.end(), [&](int i) { return scores.count(i) && scores[i] > 0; }))
        std::stable_sort(indices.begin(), indices.end(), [&](int a, int b) { return scores[a] < scores[b]; });
    return indices;
}
std::vector<std::string> server_engine_args(const J &cfg) {
    auto args = cfg.at("args").get<std::vector<std::string>>();
    auto add = [&](const char *option, const std::string &value) {
        if (!has(args, option)) {
            args.push_back(option);
            args.push_back(value);
        }
    };
    if (cfg.contains("vision") && cfg["vision"].is_object()) {
        if (!has(args, "--vision"))
            args.push_back("--vision");
        if (cfg["vision"].value("gpu", false))
            add("--vram-reserve-mib", "700");
    }
    if (server_gpu_indices(cfg).size() > 1 && !has(args, "--peer-device")) {
        add("--layer-split", server_layer_split(cfg));
        if (cfg.value("split_skip_if_fits", false) && !has(args, "--split-skip-if-fits"))
            args.push_back("--split-skip-if-fits");
    }
    if (cfg.value("vram_elastic", false)) {
        if (!has(args, "--vram-elastic"))
            args.push_back("--vram-elastic");
        if (cfg.contains("vram_segment_mib") && cfg["vram_segment_mib"].is_number_integer() &&
            cfg["vram_segment_mib"].get<int>() > 0)
            add("--vram-segment-mib", cfg["vram_segment_mib"].dump());
    }
    if (!has(args, "--batch") && !has(args, "--slots") && cfg.value("parallel", 1) > 1) {
        args.push_back("--batch");
        args.push_back(std::to_string(cfg["parallel"].get<int>()));
    }
    if (cfg.contains("expert_profile_save") && cfg["expert_profile_save"].is_string() &&
        !trim_text(cfg["expert_profile_save"].get<std::string>()).empty() &&
        !has(args, "--expert-profile-save")) {
        const auto save = cfg["expert_profile_save"].get<std::string>();
        args.push_back("--expert-profile-save");
        args.push_back(save);
        if (cfg.contains("expert_profile_save_every") && cfg["expert_profile_save_every"].is_number() &&
            cfg["expert_profile_save_every"].get<double>() >= 0) {
            args.push_back("--expert-profile-save-every");
            args.push_back(cfg["expert_profile_save_every"].dump());
        }
        auto absolute = [&](const std::string &p) {
            auto path = utf8_path(p);
            return path.is_relative() ? utf8_path(cfg.value("cwd", ".")) / path : path;
        };
        const auto learned = profile(absolute(save));
        auto it = std::find(args.begin(), args.end(), "--expert-profile");
        if (it != args.end() && std::next(it) != args.end() && learned &&
            learned == profile(absolute(*std::next(it))))
            *std::next(it) = save;
    }
    args.insert(args.begin(), "--serve");
    return args;
}
} // namespace strata::runtime
