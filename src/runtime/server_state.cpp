#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <sddl.h> // Requires Windows types and WINAPI_FAMILY_PARTITION above.
#else
#include <fcntl.h>
#include <unistd.h>
#endif
#include "strata/runtime/path.hpp"
#include "strata/runtime/server_api.hpp"
#include "strata/runtime/server_state.hpp"
#include <algorithm>
#include <cmath>
#include <set>
#include <sstream>
namespace strata::runtime {
using J = TextJson;
std::string session_filename(const J &value) {
    if (!value.is_string() || value.get<std::string>().empty())
        throw ApiError(400, "filename must be a nonempty string");
    auto name = value.get<std::string>();
    if (name.size() > 200 || name.front() == '.' || name.back() == '.' || name.back() == ' ' ||
        name.find_first_of("/\\:*?\"<>|") != std::string::npos ||
        std::any_of(name.begin(), name.end(), [](unsigned char c) { return c < 32 || c == 127; }))
        throw ApiError(400, "filename must be a plain file name without control characters");
    auto base = name.substr(0, name.find('.'));
    while (!base.empty() && base.back() == ' ')
        base.pop_back();
    std::transform(base.begin(), base.end(), base.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::set<std::string> devices{"con", "prn", "aux", "nul", "conin$", "conout$"};
    for (auto prefix : {"com", "lpt"})
        for (auto suffix : {"1", "2", "3", "4", "5", "6", "7", "8", "9", "¹", "²", "³"})
            devices.insert(std::string(prefix) + suffix);
    if (devices.count(base))
        throw ApiError(400, "filename names a Windows device");
    return name;
}
void save_server_json(const std::filesystem::path &path, const J &value) {
    auto temp = path;
    temp += utf8_path("." + api_id("tmp"));
    const auto bytes = value.dump(2) + "\n";
#ifdef _WIN32
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;FA;;;OW)", SDDL_REVISION_1, &sd,
                                                              nullptr))
        throw std::runtime_error("cannot protect settings file");
    SECURITY_ATTRIBUTES sa{sizeof(sa), sd, FALSE};
    const auto file =
        CreateFileW(temp.c_str(), GENERIC_WRITE, 0, &sa, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    LocalFree(sd);
    if (file == INVALID_HANDLE_VALUE)
        throw std::runtime_error("cannot create settings temporary file");
    bool ok = true;
    size_t offset = 0;
    while (offset < bytes.size()) {
        DWORD n = 0;
        if (!WriteFile(file, bytes.data() + offset,
                       static_cast<DWORD>(std::min<size_t>(65536, bytes.size() - offset)), &n, nullptr) ||
            !n) {
            ok = false;
            break;
        }
        offset += n;
    }
    ok = FlushFileBuffers(file) && ok;
    CloseHandle(file);
    if (!ok || !MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temp.c_str());
        throw std::runtime_error("cannot save settings atomically");
    }
#else
    const auto file = open(temp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (file < 0)
        throw std::runtime_error("cannot create settings temporary file");
    bool ok = true;
    size_t offset = 0;
    while (offset < bytes.size()) {
        auto n = write(file, bytes.data() + offset, bytes.size() - offset);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            ok = false;
            break;
        }
        offset += n;
    }
    ok = fsync(file) == 0 && ok;
    ::close(file);
    if (!ok || rename(temp.c_str(), path.c_str())) {
        unlink(temp.c_str());
        throw std::runtime_error("cannot save settings atomically");
    }
#endif
}
J shared_defaults(const J &value) {
    if (value.is_null())
        return J::object();
    if (!value.is_object())
        throw ApiError(400, "defaults must be an object");
    const std::set<std::string> allowed{"temperature",
                                        "top_p",
                                        "top_k",
                                        "seed",
                                        "max_tokens",
                                        "reasoning_effort",
                                        "reasoning_budget_tokens",
                                        "experimental_speed_projection"};
    auto out = J::object();
    for (auto it = value.begin(); it != value.end(); ++it) {
        if (!allowed.count(it.key()))
            throw ApiError(400, "unsupported shared setting: " + it.key());
        if (it.value().is_null() || it.value() == "")
            continue;
        out[it.key()] = it.value();
    }
    sampling_keys(out);
    if (out.contains("temperature") && out["temperature"].get<double>() > 2)
        throw ApiError(400, "temperature must be in 0..2");
    if (out.contains("top_p") && out["top_p"].get<double>() <= 0)
        throw ApiError(400, "top_p must be greater than zero");
    if (out.contains("top_k") && (out["top_k"].get<int>() < 1 || out["top_k"].get<int>() > 64))
        throw ApiError(400, "top_k must be in 1..64");
    if (out.contains("reasoning_effort"))
        if (out["reasoning_effort"] != "none" && out["reasoning_effort"] != "low" &&
            out["reasoning_effort"] != "medium" && out["reasoning_effort"] != "high")
            throw ApiError(400, "reasoning_effort must be none, low, medium or high");
    for (auto key : {"seed", "max_tokens"})
        if (out.contains(key) && (!out[key].is_number_integer() || out[key].get<int64_t>() < 1 ||
                                  out[key].get<int64_t>() > 2147483647))
            throw ApiError(400, std::string(key) + " must be a positive integer");
    if (out.contains("reasoning_budget_tokens") &&
        (!out["reasoning_budget_tokens"].is_number_integer() ||
         out["reasoning_budget_tokens"].get<int64_t>() < 0 ||
         out["reasoning_budget_tokens"].get<int64_t>() > 2147483647))
        throw ApiError(400, "reasoning_budget_tokens must be nonnegative integer");
    return out;
}
namespace {
const std::vector<std::pair<std::string, std::string>> editable{{"sampling.temperature", "number"},
                                                                {"sampling.top_p", "number"},
                                                                {"sampling.top_k", "number"},
                                                                {"sampling.min_p", "number"},
                                                                {"fit_max_tokens", "bool"},
                                                                {"anthropic_thinking", "enum"},
                                                                {"aliases", "names"},
                                                                {"lazy_load", "bool"},
                                                                {"engine_silence_s", "number"},
                                                                {"api_monitor", "bool"},
                                                                {"open_browser", "bool"},
                                                                {"reasoning_budget_tokens", "number"},
                                                                {"idle_unload_s", "number"},
                                                                {"effort_position", "enum"},
                                                                {"vram_reserve_mib", "number"}};
J config_value(const J &cfg, const std::string &key) {
    if (key == "vram_reserve_mib") {
        const auto args = cfg.value("args", J::array());
        for (size_t i = 0; i + 1 < args.size(); ++i)
            if (args[i] == "--vram-reserve-mib") {
                auto value = J::parse(args[i + 1].get<std::string>(), nullptr, false);
                return value.is_number() ? value : args[i + 1];
            }
        return nullptr;
    }
    if (key.starts_with("sampling."))
        return cfg.value("sampling", J::object()).value(key.substr(9), J(nullptr));
    return cfg.value(key, J(nullptr));
}
} // namespace
J server_config_view(const J &config, const std::filesystem::path &path) {
    J fields = J::array();
    for (const auto &[key, kind] : editable) {
        J item{{"key", key},
               {"kind", kind},
               {"value", config_value(config, key)},
               {"help", "Used from the next server start."}};
        if (kind == "enum")
            item["choices"] =
                key == "effort_position" ? J::array({"start", "end"}) : J::array({"model", "on_request"});
        fields.push_back(item);
    }
    return {{"file", path_utf8(path.filename())},
            {"keys", fields},
            {"note", "Saved to the run config; used from the next server start."}};
}
J server_config_edit(J config, const J &edits) {
    if (!edits.is_object())
        throw ApiError(400, "set must be an object");
    for (auto it = edits.begin(); it != edits.end(); ++it) {
        const auto found = std::find_if(editable.begin(), editable.end(),
                                        [&](const auto &p) { return p.first == it.key(); });
        if (found == editable.end())
            throw ApiError(400, "config field cannot be edited here: " + it.key());
        auto v = it.value();
        const auto &kind = found->second;
        if (!v.is_null()) {
            if (kind == "bool" && !v.is_boolean())
                throw ApiError(400, it.key() + " must be boolean");
            if (kind == "number" &&
                (!v.is_number() || !std::isfinite(v.get<double>()) || v.get<double>() < 0))
                throw ApiError(400, it.key() + " must be nonnegative");
            if (kind == "names" && v.is_string()) {
                std::istringstream input(v.get<std::string>());
                v = J::array();
                std::string name;
                while (std::getline(input, name, ',')) {
                    name = trim_text(name);
                    if (!name.empty())
                        v.push_back(name);
                }
            }
            if (it.key() == "sampling.top_p" && v.get<double>() <= 0)
                throw ApiError(400, "sampling.top_p must be greater than zero");
            if (it.key() == "sampling.top_k" &&
                (!v.is_number_integer() || v.get<int>() < 1 || v.get<int>() > 64))
                throw ApiError(400, "sampling.top_k must be an integer in 1..64");
            if (it.key() == "reasoning_budget_tokens" && !v.is_number_integer())
                throw ApiError(400, "reasoning_budget_tokens must be an integer");
        }
        if (it.key() == "vram_reserve_mib") {
            if (!v.is_null() && !v.is_number_integer())
                throw ApiError(400, "vram_reserve_mib must be an integer");
            auto args = config.value("args", J::array());
            bool found = false;
            for (size_t i = 0; i + 1 < args.size(); ++i)
                if (args[i] == "--vram-reserve-mib") {
                    if (v.is_null())
                        args.erase(args.begin() + i, args.begin() + i + 2);
                    else
                        args[i + 1] = v.dump();
                    found = true;
                    break;
                }
            if (!found && !v.is_null()) {
                args.push_back("--vram-reserve-mib");
                args.push_back(v.dump());
            }
            config["args"] = args;
        } else if (it.key().starts_with("sampling.")) {
            if (!config.contains("sampling"))
                config["sampling"] = J::object();
            if (v.is_null())
                config["sampling"].erase(it.key().substr(9));
            else
                config["sampling"][it.key().substr(9)] = v;
        } else if (v.is_null())
            config.erase(it.key());
        else
            config[it.key()] = v;
    }
    validate_server_config(config);
    return config;
}
} // namespace strata::runtime
