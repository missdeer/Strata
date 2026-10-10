#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <cstdlib>
#endif
#include "strata/runtime/path.hpp"
#include "strata/runtime/server_engine.hpp"
#include "strata/runtime/server_mcp.hpp"
#include "strata/runtime/server_security.hpp"
#include <algorithm>
#include <cmath>
#include <set>
#include <sstream>
#include <map>
#include <iostream>
#include <fstream>
#include <regex>
#include <limits>

namespace strata::runtime {
std::string server_environment(const char *name) {
#ifdef _WIN32
    const auto key = utf8_path(name).native();
    const auto size = GetEnvironmentVariableW(key.c_str(), nullptr, 0);
    if (!size)
        return {};
    std::wstring value(size, L'\0');
    const auto written = GetEnvironmentVariableW(key.c_str(), value.data(), size);
    if (written >= size)
        throw std::runtime_error("environment changed while reading");
    value.resize(written);
    return path_utf8(std::filesystem::path(value));
#else
    const auto *p = std::getenv(name);
    return p ? p : "";
#endif
}
namespace {
using Clock = std::chrono::steady_clock;
double elapsed(Clock::time_point t) { return std::chrono::duration<double>(Clock::now() - t).count(); }
bool active(const TextJson &cfg, const std::string &key) {
    if (!cfg.contains(key))
        return false;
    const auto &v = cfg[key];
    return !(v.is_null() || v == false || v == 0 || v == "" || (v.is_structured() && v.empty()));
}
void env(const std::string &key, const std::string &val) {
    if (key.empty() || key.find('=') != std::string::npos || key.find('\0') != std::string::npos ||
        val.find('\0') != std::string::npos)
        throw std::invalid_argument("invalid engine environment");
#ifdef _WIN32
    if (!SetEnvironmentVariableW(utf8_path(key).c_str(), utf8_path(val).c_str()))
        throw std::runtime_error("cannot set engine environment");
#else
    if (setenv(key.c_str(), val.c_str(), 1))
        throw std::runtime_error("cannot set engine environment");
#endif
}
double timeout(const char *name, double fallback) {
    const auto raw = server_environment(name);
    if (raw.empty())
        return fallback;
    try {
        size_t end = 0;
        const auto v = std::stod(raw, &end);
        if (end != raw.size() || !std::isfinite(v) || v < 0)
            throw std::exception();
        return v;
    } catch (...) {
        throw std::invalid_argument(std::string(name) + " must be a nonnegative number");
    }
}
TextJson facts(std::istringstream &line) {
    auto result = TextJson::object();
    std::string part;
    while (line >> part) {
        const auto p = part.find('=');
        if (p == std::string::npos)
            continue;
        const auto value = part.substr(p + 1);
        const auto begin = !value.empty() && value.front() == '-' ? size_t(1) : size_t(0);
        bool integer =
            begin < value.size() && std::all_of(
                                        value.begin() + static_cast<std::ptrdiff_t>(begin), value.end(),
                                        [](unsigned char c) { return c >= '0' && c <= '9'; });
        if (integer) {
            try {
                result[part.substr(0, p)] = std::stoll(value);
            } catch (...) {
                result[part.substr(0, p)] = value;
            }
        } else
            result[part.substr(0, p)] = value;
    }
    return result;
}
} // namespace
TextJson ApiError::body(bool anthropic) const {
    TextJson error{{"type", kind}, {"message", what()}};
    if (!anthropic) {
        error["param"] = param.empty() ? TextJson(nullptr) : TextJson(param);
        error["code"] = code.empty() ? TextJson(nullptr) : TextJson(code);
    }
    error.update(details);
    return anthropic ? TextJson{{"type", "error"}, {"error", error}} : TextJson{{"error", error}};
}
void validate_server_config(const TextJson &cfg) {
    if (!cfg.is_object())
        throw std::invalid_argument("server config must be an object");
    const std::set<std::string> supported{"exe",
                                          "args",
                                          "cwd",
                                          "tokenizer",
                                          "model_name",
                                          "log",
                                          "lib_dirs",
                                          "port",
                                          "host",
                                          "gpu",
                                          "gpus_asked",
                                          "backend",
                                          "env",
                                          "open_browser",
                                          "api_key",
                                          "aliases",
                                          "fit_max_tokens",
                                          "anthropic_thinking",
                                          "sampling",
                                          "tool_call_recovery",
                                          "engine_silence_s",
                                          "api_monitor",
                                          "lazy_load",
                                          "parallel",
                                          "web_root",
                                          "template",
                                          "repeat_stop_tokens",
                                          "reasoning_budget_tokens",
                                          "slot_save_path",
                                          "vram_elastic",
                                          "vram_segment_mib",
                                          "mcp_servers",
                                          "mcpServers",
                                          "mcp",
                                          "vision",
                                          "allowed_hosts",
                                          "trusted_origins",
                                          "cors_origins",
                                          "layer_split",
                                          "split_skip_if_fits",
                                          "hip_ordinal",
                                          "gpu_order",
                                          "expert_profile_save",
                                          "expert_profile_save_every",
                                          "idle_unload_s",
                                          "before_load",
                                          "min_free_vram_mib",
                                          "effort_position",
                                          "reasoning_close_retry",
                                          "reasoning_loop_recovery",
                                          "codex_thread_titles",
                                          "codex_compaction_cache"};
    for (auto it = cfg.begin(); it != cfg.end(); ++it)
        if (!supported.count(it.key()))
            throw std::invalid_argument("native server does not yet support config field: " + it.key());
    for (auto key : {"exe", "tokenizer"})
        if (!cfg.contains(key) || !cfg[key].is_string() || cfg[key].get<std::string>().empty())
            throw std::invalid_argument(std::string("server config requires ") + key);
    if (!cfg.contains("args") || !cfg["args"].is_array())
        throw std::invalid_argument("server config requires args array");
    for (const auto &arg : cfg["args"])
        if (!arg.is_string() || arg.get<std::string>().find('\0') != std::string::npos)
            throw std::invalid_argument("engine args must be strings without NUL");
    for (const auto &arg : cfg["args"])
        if (arg == "--serve")
            throw std::invalid_argument("native server does not yet support engine option: " +
                                        arg.get<std::string>());
    if (active(cfg, "parallel") && (!cfg["parallel"].is_number_integer() || cfg["parallel"].get<int>() < 1 ||
                                    cfg["parallel"].get<int>() > 256))
        throw std::invalid_argument("parallel must be an integer in 1..256");
    for (auto key :
         {"api_monitor", "lazy_load", "fit_max_tokens", "tool_call_recovery", "open_browser", "gpus_asked"})
        if (cfg.contains(key) && !cfg[key].is_boolean())
            throw std::invalid_argument(std::string(key) + " must be boolean");
    for (auto key : {"vram_segment_mib", "min_free_vram_mib"})
        if (cfg.contains(key) && (!cfg[key].is_number_integer() || cfg[key].get<int64_t>() < 0 ||
                                  cfg[key].get<int64_t>() > 2147483647))
            throw std::invalid_argument(std::string(key) + " must be a nonnegative integer");
    for (const auto &arg : cfg["args"])
        if (arg.get<std::string>().find_first_of("\r\n") != std::string::npos)
            throw std::invalid_argument("engine args must not contain line breaks");
    if (cfg.contains("anthropic_thinking") && cfg["anthropic_thinking"] != "model" &&
        cfg["anthropic_thinking"] != "on_request")
        throw std::invalid_argument("anthropic_thinking must be model or on_request");
    if (cfg.contains("engine_silence_s") &&
        (!cfg["engine_silence_s"].is_number() || cfg["engine_silence_s"].get<double>() < 0))
        throw std::invalid_argument("engine_silence_s must be nonnegative");
    if (cfg.contains("repeat_stop_tokens") &&
        (!cfg["repeat_stop_tokens"].is_number_integer() || cfg["repeat_stop_tokens"].get<int>() < 0))
        throw std::invalid_argument("repeat_stop_tokens must be nonnegative integer");
    if (cfg.contains("reasoning_budget_tokens") && !cfg["reasoning_budget_tokens"].is_number_integer())
        throw std::invalid_argument("reasoning_budget_tokens must be an integer");
    if (cfg.contains("env") && !cfg["env"].is_object())
        throw std::invalid_argument("env must be an object");
    if (cfg.value("env", TextJson::object()).contains("STRATA_API_KEY") ||
        cfg.value("env", TextJson::object()).contains("STRATA_NETWORK_KEY"))
        throw std::invalid_argument(
            "Strata secrets must be supplied through the environment, not config env");
    server_gpu_indices(cfg);
    server_layer_split(cfg);
    for (auto key : {"idle_unload_s", "min_free_vram_mib"})
        if (cfg.contains(key) &&
            (!cfg[key].is_number() || !std::isfinite(cfg[key].get<double>()) || cfg[key].get<double>() < 0))
            throw std::invalid_argument(std::string(key) + " must be nonnegative");
    for (auto key : {"reasoning_close_retry", "codex_thread_titles", "codex_compaction_cache",
                     "split_skip_if_fits", "vram_elastic"})
        if (cfg.contains(key) && !cfg[key].is_boolean())
            throw std::invalid_argument(std::string(key) + " must be boolean");
    if (cfg.contains("effort_position") && cfg["effort_position"] != "start" &&
        cfg["effort_position"] != "end")
        throw std::invalid_argument("effort_position must be start or end");
    if (cfg.contains("reasoning_loop_recovery") && cfg["reasoning_loop_recovery"] != false &&
        cfg["reasoning_loop_recovery"] != true && cfg["reasoning_loop_recovery"] != "stop" &&
        cfg["reasoning_loop_recovery"] != "recover")
        throw std::invalid_argument("reasoning_loop_recovery must be false, stop or recover");
    if (cfg.contains("before_load") && !cfg["before_load"].is_null() && !cfg["before_load"].is_string() &&
        !cfg["before_load"].is_array())
        throw std::invalid_argument("before_load must be command string or argv array");
    if (cfg.contains("lib_dirs")) {
        if (!cfg["lib_dirs"].is_array())
            throw std::invalid_argument("lib_dirs must be an array");
        for (const auto &d : cfg["lib_dirs"])
            if (!d.is_string())
                throw std::invalid_argument("lib_dirs must contain strings");
    }
    server_aliases(cfg);
    if (cfg.contains("sampling") && !cfg["sampling"].is_object())
        throw std::invalid_argument("sampling must be an object");
    const auto defaults = cfg.value("sampling", TextJson::object());
    const std::set<std::string> known{"temperature",
                                      "top_p",
                                      "top_k",
                                      "min_p",
                                      "presence_penalty",
                                      "repetition_penalty",
                                      "frequency_penalty",
                                      "penalty_last_n",
                                      "seed",
                                      "experimental_speed_projection"};
    for (auto it = defaults.begin(); it != defaults.end(); ++it)
        if (!known.count(it.key()))
            throw std::invalid_argument("unknown sampling default: " + it.key());
    sampling_keys(cfg.value("sampling", TextJson::object()));
    validate_mcp(cfg);
    validate_security(cfg);
    if (cfg.contains("vision") && !cfg["vision"].is_null()) {
        if (!cfg["vision"].is_object())
            throw std::invalid_argument("vision must be an object");
        for (auto key : {"exe", "mmproj", "model"})
            if (!cfg["vision"].contains(key) || !cfg["vision"][key].is_string() ||
                cfg["vision"][key].get<std::string>().empty())
                throw std::invalid_argument(std::string("vision requires ") + key);
    }
}
std::map<std::string, std::string> child_environment(const TextJson &cfg) {
    std::map<std::string, std::string> out;
    auto indices = server_ordered_gpus(cfg);
    if (cfg.value("backend", "") == "hip" && indices.size() <= 1 && cfg.contains("hip_ordinal")) {
        try {
            indices = {std::stoi(cfg["hip_ordinal"].is_string() ? cfg["hip_ordinal"].get<std::string>()
                                                                : cfg["hip_ordinal"].dump())};
        } catch (...) {
        }
    }
    if (!indices.empty()) {
        std::string gpu;
        for (int i : indices) {
            if (!gpu.empty())
                gpu += ',';
            gpu += std::to_string(i);
        }
        if (cfg.value("backend", "") == "hip")
            out["HIP_VISIBLE_DEVICES"] = gpu;
        else {
            out["CUDA_VISIBLE_DEVICES"] = gpu;
            out["CUDA_DEVICE_ORDER"] = "PCI_BUS_ID";
        }
    }
    const auto configured = cfg.value("env", TextJson::object());
    for (auto it = configured.begin(); it != configured.end(); ++it)
        out[it.key()] = it.value().is_string() ? it.value().get<std::string>() : it.value().dump();
#ifdef _WIN32
    const auto key = "PATH", sep = ";";
#else
    const auto key = "LD_LIBRARY_PATH", sep = ":";
#endif
    std::string prefix;
    for (const auto &dir : cfg.value("lib_dirs", TextJson::array()))
        if (std::filesystem::is_directory(utf8_path(dir.get<std::string>())))
            prefix += dir.get<std::string>() + sep;
    if (!prefix.empty())
        out[key] = prefix + (out.count(key) ? out[key] : server_environment(key));
    if (cfg.value("backend", "") == "hip") {
        const auto writable = [](const std::filesystem::path &directory) {
            try {
                if (!std::filesystem::is_directory(directory))
                    return false;
                const auto path =
                    directory /
                    utf8_path("strata-probe-" +
                              std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
                std::ofstream file(path, std::ios::binary);
                if (!file)
                    return false;
                file.close();
                std::error_code ignored;
                std::filesystem::remove(path, ignored);
                return true;
            } catch (...) {
                return false;
            }
        };
        std::vector<std::string> bad;
        for (auto variable : {"TEMP", "TMP", "TMPDIR"}) {
            const auto value = out.count(variable) ? out[variable] : server_environment(variable);
            if (!value.empty() && !writable(utf8_path(value)))
                bad.push_back(variable);
        }
        if (!bad.empty()) {
            const auto base = std::filesystem::absolute(utf8_path(cfg.value("log", cfg.value("cwd", "."))));
            for (const auto &directory : std::vector<std::filesystem::path>{
                     base.parent_path() / "tmp", utf8_path(cfg.value("cwd", ".")) / "tmp"}) {
                std::error_code error;
                std::filesystem::create_directories(directory, error);
                if (writable(directory)) {
                    for (const auto &variable : bad)
                        out[variable] = path_utf8(directory);
                    break;
                }
            }
        }
    }
    return out;
}
std::vector<std::string> child_environment_removed(const TextJson &cfg) {
    std::vector<std::string> result;
    if (cfg.value("backend", "") != "hip")
        return result;
    const auto values = child_environment(cfg);
    for (auto key : {"HIP_PATH", "HIP_DEVICE_LIB_PATH", "LLVM_PATH"}) {
        const auto value = values.count(key) ? values.at(key) : server_environment(key);
        if (!value.empty() && !std::filesystem::exists(utf8_path(value)))
            result.push_back(key);
    }
    return result;
}
void apply_engine_environment(const TextJson &cfg) {
    if (cfg.contains("gpu")) {
        const auto gpu = cfg["gpu"].is_string() ? cfg["gpu"].get<std::string>() : cfg["gpu"].dump();
        if (cfg.value("backend", "") == "hip")
            env("HIP_VISIBLE_DEVICES", gpu);
        else {
            env("CUDA_DEVICE_ORDER", "PCI_BUS_ID");
            env("CUDA_VISIBLE_DEVICES", gpu);
        }
    }
    const auto settings = cfg.value("env", TextJson::object());
    for (auto it = settings.begin(); it != settings.end(); ++it) {
        if (it.key() == "STRATA_API_KEY" || it.key() == "STRATA_NETWORK_KEY")
            throw std::invalid_argument("secrets must be supplied through environment, not config env");
        env(it.key(), it.value().is_string() ? it.value().get<std::string>() : it.value().dump());
    }
#ifdef _WIN32
    const auto variable = "PATH";
    const auto sep = ";";
#else
    const auto variable = "LD_LIBRARY_PATH";
    const auto sep = ":";
#endif
    std::string paths;
    for (const auto &d : cfg.value("lib_dirs", TextJson::array()))
        if (std::filesystem::is_directory(utf8_path(d.get<std::string>())))
            paths += d.get<std::string>() + sep;
    if (!paths.empty()) {
        paths += server_environment(variable);
        env(variable, paths);
    }
}
std::string sampling_keys(const TextJson &s) {
    std::string out;
    auto number = [&](const char *key, double lo, double hi, bool whole = false) -> std::optional<double> {
        if (!s.contains(key) || s[key].is_null())
            return {};
        if (!s[key].is_number() || (whole && !s[key].is_number_integer()))
            throw ApiError(400, std::string(key) + " must be a number", "invalid_request_error", key);
        auto v = s[key].get<double>();
        if (!std::isfinite(v) || v < lo || v > hi)
            throw ApiError(400, std::string(key) + " is outside supported range", "invalid_request_error",
                           key);
        return v;
    };
    auto add = [&](const char *api, const char *engine, double lo, double hi, double absent,
                   bool whole = false) {
        if (auto v = number(api, lo, hi, whole); v && *v != absent)
            out += " " + std::string(engine) + "=" +
                   (whole ? std::to_string(static_cast<int64_t>(*v)) : s[api].dump());
    };
    add("temperature", "temperature", 0, 1e6, 0);
    add("top_p", "top_p", 0, 1, 1);
    add("min_p", "min_p", 0, 1, 0);
    if (auto k = number("top_k", 0, 2147483647, true))
        out += " top_k=" + std::to_string(*k >= 1 && *k <= 64 ? static_cast<int>(*k) : 64);
    add("repetition_penalty", "penalty_repeat", 0, 1e6, 1);
    add("frequency_penalty", "penalty_freq", -2, 2, 0);
    add("presence_penalty", "penalty_present", -2, 2, 0);
    if (out.find("penalty_") != std::string::npos) {
        auto n = number("penalty_last_n", 1, 2147483647, true);
        out += " penalty_last_n=" + std::to_string(n ? static_cast<int>(*n) : 64);
    }
    add("seed", "seed", 0, 9007199254740991., 0, true);
    if (s.contains("experimental_speed_projection")) {
        if (!s["experimental_speed_projection"].is_boolean())
            throw ApiError(400, "experimental_speed_projection must be boolean");
        out += s["experimental_speed_projection"].get<bool>() ? " cvec=1" : " cvec=0";
    }
    if (s.value("strata_checkpoint", TextJson(true)) == false)
        out += " ckpt=0";
    const auto prefix = s.value("strata_prefix", TextJson(nullptr));
    if (prefix.is_object() && prefix.value("tokens", TextJson(nullptr)).is_number_integer() &&
        prefix["tokens"].get<int64_t>() > 0)
        out += " pin=" + prefix["tokens"].dump();
    const auto tune = s.value("strata_tune", TextJson(nullptr));
    if (tune.is_object())
        for (auto key : {"pcie_frac", "spec_min_p"})
            if (tune.contains(key) && tune[key].is_number() && tune[key].get<double>() >= 0 &&
                tune[key].get<double>() <= 1)
                out += " " + std::string(key) + "=" + tune[key].dump();
    return out;
}
ServerEngine::ServerEngine(TextJson cfg, std::function<void()> before_start,
                           std::function<void()> after_unload)
    : cfg_(std::move(cfg)), before_start_(std::move(before_start)), after_unload_(std::move(after_unload)) {
    unloaded_ = cfg_.value("lazy_load", false);
    state_["info"] = TextJson::object();
    silence_ = cfg_.value("engine_silence_s", 300.);
    ready_timeout_ = timeout("STRATA_ENGINE_READY_S", 900);
    stop_timeout_ = timeout("STRATA_ENGINE_STOP_S", 300);
    stall_timeout_ = timeout("STRATA_ENGINE_STALL_S", 90);
    const auto args = cfg_["args"].get<std::vector<std::string>>();
    requested_batch_ = cfg_.value("parallel", 1) > 1 ? cfg_["parallel"].get<int>() : 0;
    for (size_t i = 0; i + 1 < args.size(); ++i)
        if (args[i] == "--batch" || args[i] == "--slots")
            requested_batch_ = std::stoi(args[i + 1]);
    for (size_t i = 0; i + 1 < args.size(); ++i)
        if (args[i] == "--max-context")
            context_ = std::stoi(args[i + 1]);
    last_activity_ = std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
    if (!cfg_.value("lazy_load", false))
        start();
    const auto idle = cfg_.value("idle_unload_s", 0.);
    if (idle > 0)
        idle_thread_ = std::thread([this, idle] {
            std::unique_lock<std::mutex> lock(idle_mutex_);
            while (!shutting_down_) {
                idle_changed_.wait_for(lock,
                                       std::chrono::duration<double>(std::max(1., std::min(30., idle / 4))));
                if (shutting_down_)
                    break;
                lock.unlock();
                try {
                    control("unload", [] { return false; }, true);
                } catch (const ApiError &) {
                }
                lock.lock();
            }
        });
}
ServerEngine::~ServerEngine() {
    shutdown();
    if (idle_thread_.joinable())
        idle_thread_.join();
    if (process_)
        process_->finish();
    if (after_unload_)
        after_unload_();
}
void ServerEngine::discard(const std::shared_ptr<EngineChannel> &expected) {
    std::lock_guard<std::recursive_mutex> lifecycle(lifecycle_mutex_);
    if (expected && expected != process_)
        return;
    loaded_ = false;
    unloaded_ = false;
    if (process_)
        process_->close();
    process_.reset();
    can_stop_ = false;
    slots_changed_.notify_all();
}
void ServerEngine::start(const std::function<bool()> &cancelled) {
    std::lock_guard<std::recursive_mutex> lifecycle(lifecycle_mutex_);
    if (loaded())
        return;
    const bool was_unloaded = unloaded_;
    discard();
    if (started_once_ || cfg_.value("lazy_load", false)) {
        const auto hook = cfg_.value("before_load", TextJson(nullptr));
        if (!hook.is_null() && hook != "" && hook != TextJson::array()) {
            try {
                std::vector<std::string> command;
                if (hook.is_array()) {
                    for (const auto &arg : hook)
                        command.push_back(arg.is_string() ? arg.get<std::string>() : arg.dump());
                } else {
#ifdef _WIN32
                    auto shell = server_environment("COMSPEC");
                    if (shell.empty())
                        shell = "C:/Windows/System32/cmd.exe";
                    command = {shell, "/d", "/s", "/c", hook.get<std::string>()};
#else
                    command = {"/bin/sh", "-c", hook.get<std::string>()};
#endif
                }
                EngineProcess helper(command, utf8_path(cfg_.value("cwd", ".")),
                                     utf8_path(cfg_.value("log", "")));
                const auto end = Clock::now() + std::chrono::seconds(120);
                while (helper.alive() && Clock::now() < end && !shutting_down_ &&
                       !(cancelled && cancelled())) {
                    std::string ignored;
                    try {
                        helper.receive(ignored, std::chrono::milliseconds(100));
                    } catch (...) {
                        break;
                    }
                }
                if (helper.alive())
                    helper.close();
            } catch (...) {
                std::cerr << "before_load command failed; loading anyway\n";
            }
        }
    }
    if (cancelled && cancelled())
        throw ApiError(499, "model load cancelled", "server_error");
    try {
        if (before_start_)
            before_start_();
    } catch (...) {
        unloaded_ = was_unloaded;
        if (after_unload_)
            after_unload_();
        throw;
    }
    {
        std::lock_guard<std::mutex> state(state_mutex_);
        state_["info"] = TextJson::object();
    }
    const auto cwd = std::filesystem::absolute(utf8_path(cfg_.value("cwd", ".")));
    auto exe = utf8_path(cfg_["exe"].get<std::string>());
    if (exe.is_relative())
        exe = cwd / exe;
    auto args = server_engine_args(cfg_);
    {
        std::lock_guard<std::mutex> state(state_mutex_);
        try {
            state_["info"]["version"] =
                read_text_json(exe.parent_path() / "BUILD.json").value("version", TextJson(nullptr));
        } catch (...) {
            state_["info"]["version"] = nullptr;
        }
    }
    args.insert(args.begin(), path_utf8(exe));
    auto log = utf8_path(cfg_.value("log", ""));
    if (!log.empty() && log.is_relative())
        log = cwd / log;
    {
        std::lock_guard<std::mutex> cache(cache_mutex_);
        log_path_ = log;
        std::error_code e;
        log_start_ = log.empty() ? 0 : std::filesystem::file_size(log, e);
        if (e)
            log_start_ = 0;
        log_position_ = log_start_;
        ++log_epoch_;
        cache_state_ = {{"parked", 0},
                        {"bytes", 0},
                        {"evictions", 0},
                        {"parks", 0},
                        {"restores", 0},
                        {"last_event", nullptr},
                        {"last_tokens", nullptr},
                        {"last_at", nullptr}};
    }
    const auto epoch = ++process_epoch_;
    process_ = std::make_shared<EngineChannel>(
        args, cwd, log, child_environment(cfg_),
        [this, epoch] {
            if (process_epoch_ == epoch)
                loaded_ = false;
        },
        child_environment_removed(cfg_));
    const auto born = Clock::now();
    try {
        for (;;) {
            if (shutting_down_ || (cancelled && cancelled()))
                throw std::runtime_error("server is stopping");
            if (ready_timeout_ > 0 && elapsed(born) >= ready_timeout_)
                throw std::runtime_error("engine READY deadline exceeded");
            std::string line;
            if (!process_->receive(line, std::chrono::milliseconds(100)))
                continue;
            std::istringstream in(line);
            std::string kind;
            in >> kind;
            if (kind == "INFO") {
                auto info = facts(in);
                std::lock_guard<std::mutex> l(state_mutex_);
                state_["info"].update(info);
                if (info.contains("engine"))
                    state_["info"]["version"] =
                        info["engine"].is_string() ? info["engine"] : TextJson(info["engine"].dump());
            }
            if (kind == "ERR")
                throw std::runtime_error("engine failed before READY; see engine log");
            if (kind != "READY")
                continue;
            int ctx = 0;
            in >> ctx;
            if (ctx <= 8)
                throw std::runtime_error("invalid engine READY context");
            std::string flag;
            while (in >> flag)
                if (flag == "stop")
                    can_stop_ = true;
            context_ = ctx;
            {
                std::lock_guard<std::mutex> state(state_mutex_);
                batch_ = state_["info"].value("batch_slots", 0);
                slot_groups_ = std::max(1, state_["info"].value("batch_groups", 1));
            }
            if (batch_ < 0 || batch_ > 256)
                throw std::runtime_error("engine reported invalid batch slot count");
            {
                std::lock_guard<std::mutex> slots(slots_mutex_);
                slots_.clear();
                slots_.resize(static_cast<size_t>(batch_.load()));
            }
            loaded_ = true;
            if (!process_->alive()) {
                loaded_ = false;
                throw std::runtime_error("engine exited before it could serve requests");
            }
            started_once_ = true;
            if (!vram_reserve_.is_null()) {
                process_->send("VRAM " + vram_reserve_.dump());
                const auto end = Clock::now() + std::chrono::seconds(120);
                for (;;) {
                    if (Clock::now() >= end || shutting_down_)
                        throw std::runtime_error("VRAM restore deadline exceeded");
                    std::string reply;
                    if (!process_->receive(reply, std::chrono::milliseconds(100)))
                        continue;
                    std::istringstream fields(reply);
                    std::string kind;
                    fields >> kind;
                    if (kind == "VRAM") {
                        auto facts_ = facts(fields);
                        std::lock_guard<std::mutex> state(state_mutex_);
                        state_["info"].update(facts_);
                        state_["info"]["vram"] = facts_;
                        break;
                    }
                    if (kind == "ERR")
                        break;
                }
            }
            break;
        }
    } catch (...) {
        discard();
        if (after_unload_)
            after_unload_();
        throw;
    }
}
void ServerEngine::enter(const std::function<bool()> &cancelled, size_t prompt_length) {
    last_activity_ = std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
    std::unique_lock<std::mutex> lock(queue_mutex_);
    if (queue_.size() >= 128)
        throw ApiError(503, "request queue is full", "server_error");
    const auto ticket = next_++;
    queue_.push_back(ticket);
    waiting_lengths_[ticket] = prompt_length;
    for (;;) {
        if (cancelled() || shutting_down_) {
            queue_.erase(std::find(queue_.begin(), queue_.end(), ticket));
            waiting_lengths_.erase(ticket);
            queue_changed_.notify_all();
            throw ApiError(499, "request cancelled", "server_error");
        }
        if (!occupied_ && queue_.front() == ticket) {
            queue_.pop_front();
            waiting_lengths_.erase(ticket);
            occupied_ = true;
            return;
        }
        queue_changed_.wait_for(lock, std::chrono::milliseconds(100));
    }
}
void ServerEngine::leave() {
    last_activity_ = std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
    std::lock_guard<std::mutex> l(queue_mutex_);
    occupied_ = false;
    maintenance_ = false;
    queue_changed_.notify_all();
}
std::unique_ptr<ServerEngine::Lease> ServerEngine::acquire(const std::function<bool()> &cancelled) {
    last_activity_ = std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
    if (requested_batch_ > 0 && !loaded())
        start(cancelled);
    if (batch_ > 0) {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        while (maintenance_) {
            if (cancelled() || shutting_down_)
                throw ApiError(499, "request cancelled", "server_error");
            queue_changed_.wait_for(lock, std::chrono::milliseconds(100));
        }
        if (cancelled() || shutting_down_)
            throw ApiError(499, "request cancelled", "server_error");
        ++parallel_leases_;
        return std::unique_ptr<Lease>(new Lease(this, false));
    }
    enter(cancelled);
    return std::unique_ptr<Lease>(new Lease(this));
}
void ServerEngine::ensure_loaded(const Lease *lease, const std::function<bool()> &cancelled) {
    if (!lease || lease->owner_ != this)
        throw std::invalid_argument("engine load requires a request lease");
    if (!loaded()) {
        try {
            start(cancelled);
        } catch (const ApiError &) {
            throw;
        } catch (...) {
            throw ApiError(cancelled() ? 499 : 503, "engine could not be loaded or load was cancelled",
                           "server_error");
        }
    }
}
std::unique_ptr<ServerEngine::Lease> ServerEngine::exclusive(const std::function<bool()> &cancelled) {
    enter(cancelled);
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        maintenance_ = true;
    }
    auto lease = std::unique_ptr<Lease>(new Lease(this));
    {
        std::unique_lock<std::mutex> lock(slots_mutex_);
        while (std::any_of(slots_.begin(), slots_.end(), [](const Slot &s) { return s.engine_active; })) {
            if (cancelled() || shutting_down_)
                throw ApiError(499, "request cancelled", "server_error");
            slots_changed_.wait_for(lock, std::chrono::milliseconds(100));
        }
    }
    lifecycle_mutex_.lock();
    lease->lifecycle_ = true;
    return lease;
}
TextJson ServerEngine::snapshot() {
    std::lock_guard<std::mutex> q(queue_mutex_);
    std::lock_guard<std::mutex> s(state_mutex_);
    auto out = state_;
    out["busy"] = occupied_ || parallel_leases_ > 0;
    out["queued"] = queue_.size() + slot_waiters_.load();
    out["loaded"] = loaded();
    {
        std::lock_guard<std::mutex> lock(slots_mutex_);
        auto view = TextJson::array();
        for (size_t i = 0; i < slots_.size(); ++i) {
            auto row = slots_[i].live.is_null()
                           ? TextJson{{"slot", i}, {"state", "idle"}, {"held_tokens", slots_[i].held.size()}}
                           : slots_[i].live;
            view.push_back(row);
        }
        out["slots"] = view;
        out["parallel"] = batch_.load();
    }
    return out;
}
bool ServerEngine::shorter_waiting(size_t length) {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    return std::any_of(waiting_lengths_.begin(), waiting_lengths_.end(),
                       [&](const auto &item) { return item.second > 0 && item.second * 2 <= length; });
}
TextJson ServerEngine::cache_view() {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    if (log_path_.empty())
        return cache_state_;
    std::error_code error;
    const auto size = std::filesystem::file_size(log_path_, error);
    if (error)
        return cache_state_;
    if (size < log_position_)
        log_position_ = 0;
    if (size - log_position_ > (1u << 20))
        log_position_ = size - (1u << 20);
    std::ifstream file(log_path_, std::ios::binary);
    file.seekg(static_cast<std::streamoff>(log_position_));
    std::string data(static_cast<size_t>(size - log_position_), '\0');
    file.read(data.data(), data.size());
    data.resize(static_cast<size_t>(file.gcount()));
    const auto end = data.rfind('\n');
    if (end == std::string::npos)
        return cache_state_;
    log_position_ += end + 1;
    static const std::regex event("conversation cache: (parked|restored|skipped) ([0-9]+) "
                                  "tokens.*?parked=([0-9]+) bytes=([0-9]+)(?: evictions=([0-9]+))?"),
        dropped("conversation cache: dropped [0-9]+ superseded .*?parked=([0-9]+)");
    std::istringstream lines(data.substr(0, end));
    std::string line;
    std::smatch match;
    while (std::getline(lines, line)) {
        if (std::regex_search(line, match, event)) {
            cache_state_["parked"] = std::stoll(match[3]);
            cache_state_["bytes"] = std::stoll(match[4]);
            if (match[5].matched)
                cache_state_["evictions"] = std::stoll(match[5]);
            if (match[1] != "skipped") {
                const auto key = match[1] == "parked" ? "parks" : "restores";
                cache_state_[key] = cache_state_[key].get<int>() + 1;
                cache_state_["last_event"] = match[1].str();
                cache_state_["last_tokens"] = std::stoll(match[2]);
                cache_state_["last_at"] = std::time(nullptr);
            }
        } else if (std::regex_search(line, match, dropped))
            cache_state_["parked"] = std::stoll(match[1]);
    }
    return cache_state_;
}
TextJson ServerEngine::generate(const std::vector<int> &ids, int max_new, const TextJson &sampling,
                                const std::function<bool(int)> &token, const std::function<bool()> &cancelled,
                                const Lease *lease, const std::string &embeddings) {
    if (requested_batch_ > 0 && !loaded())
        start();
    if (batch_ > 0)
        return generate_batch(ids, max_new, sampling, token, cancelled, embeddings);
    return generate_serial(ids, max_new, sampling, token, cancelled, lease, embeddings);
}
TextJson ServerEngine::generate_serial(const std::vector<int> &ids, int max_new, const TextJson &sampling,
                                       const std::function<bool(int)> &token,
                                       const std::function<bool()> &cancelled, const Lease *lease,
                                       const std::string &embeddings, int *yield_slot,
                                       const std::function<void()> &started) {
    const auto keys = sampling_keys(sampling);
    std::unique_ptr<Lease> owned;
    if (!lease || !lease->exclusive_) {
        enter(cancelled, ids.size());
        owned.reset(new Lease(this));
    }
    if (lease && lease->owner_ != this)
        throw std::invalid_argument("engine lease belongs to another engine");
    std::shared_ptr<EngineChannel> channel;
    try {
        std::lock_guard<std::recursive_mutex> lifecycle(lifecycle_mutex_);
        if (!loaded())
            start();
        channel = process_;
    } catch (const ApiError &) {
        throw;
    } catch (...) {
        throw ApiError(503, "engine could not be restarted; see engine log", "server_error");
    }
    bool sent = false, stopping = false, refused = false;
    Watch watch;
    int yielded = -1;
    int64_t yielded_tokens = 0;
    auto heard = Clock::now(), beat = heard, stopped_at = heard;
    auto allow = silence_ + std::min<size_t>(ids.size(), 32768) / 50.;
    try {
        if (static_cast<int64_t>(ids.size()) + max_new + 8 > context())
            throw ApiError(400, "prompt and max_tokens exceed engine context");
        std::string command = (embeddings.empty() ? "GEN " : "GENI ") + std::to_string(max_new) + keys + " " +
                              (embeddings.empty() ? std::string() : embeddings + " ");
        for (size_t i = 0; i < ids.size(); ++i) {
            if (i)
                command += ',';
            command += std::to_string(ids[i]);
        }
        {
            std::lock_guard<std::mutex> l(state_mutex_);
            state_["prompt_tokens"] = ids.size();
            state_["generated"] = 0;
            state_["progress"] = nullptr;
        }
        if (started)
            started();
        channel->send(command);
        sent = true;
        heard = beat = Clock::now();
        int count = 0, read_to = 0;
        auto first_token = Clock::now();
        for (;;) {
            if (!stopping && (cancelled() || shutting_down_)) {
                stopping = true;
                stopped_at = Clock::now();
                if (can_stop_)
                    channel->send(yield_slot && *yield_slot >= 0 ? "BYIELD " + std::to_string(*yield_slot)
                                                                 : "STOP");
                else {
                    discard(channel);
                    return {{"finish", "cancel"},
                            {"generated", count},
                            {"prompt_tokens", ids.size()},
                            {"reused", 0}};
                }
            }
            if (stopping && elapsed(stopped_at) > stop_timeout_)
                throw std::runtime_error("engine STOP deadline exceeded");
            if (silence_ > 0 && elapsed(heard) > allow)
                throw std::runtime_error("engine silence deadline exceeded");
            std::string line;
            if (!channel->receive(line, std::chrono::milliseconds(100))) {
                check_stall(channel, heard, watch);
                if (!stopping && elapsed(beat) >= 10) {
                    beat = Clock::now();
                    if (!token(-1)) {
                        stopping = true;
                        stopped_at = Clock::now();
                        if (can_stop_)
                            channel->send("STOP");
                        else
                            discard(channel);
                    }
                }
                if (!loaded() || !channel->alive())
                    return {{"finish", "cancel"},
                            {"generated", count},
                            {"prompt_tokens", ids.size()},
                            {"reused", 0}};
                continue;
            }
            heard = beat = Clock::now();
            watch = Watch{};
            std::istringstream in(line);
            std::string kind;
            in >> kind;
            if (kind == "ERR") {
                std::string message;
                std::getline(in, message);
                refused = true;
                throw ApiError(400, trim_text(message));
            }
            if (kind == "T") {
                int id = -1;
                if (!(in >> id) || id < 0)
                    throw std::runtime_error("invalid engine token");
                ++count;
                if (count == 1)
                    first_token = Clock::now();
                {
                    std::lock_guard<std::mutex> l(state_mutex_);
                    state_["generated"] = count;
                    state_["tok_s"] = count / std::max(.25, elapsed(first_token));
                }
                allow = silence_;
                if (!stopping && !token(id)) {
                    stopping = true;
                    stopped_at = Clock::now();
                    if (can_stop_)
                        channel->send("STOP");
                    else {
                        discard(channel);
                        return {{"finish", "cancel"},
                                {"generated", count},
                                {"prompt_tokens", ids.size()},
                                {"reused", 0}};
                    }
                }
            } else if (kind == "PP") {
                int read = 0, total = 0;
                double ms = 0, rate = 0;
                in >> read >> total >> ms >> rate;
                if (silence_ > 0 && rate > 0)
                    allow = std::max(silence_, 3. * (read - read_to) / rate);
                read_to = read;
                {
                    std::lock_guard<std::mutex> l(state_mutex_);
                    state_["progress"] = {{"read", read}, {"total", total}, {"ms", ms}};
                }
                if (!stopping && !token(-1)) {
                    stopping = true;
                    stopped_at = Clock::now();
                    if (can_stop_)
                        channel->send("STOP");
                }
            } else if (kind == "RESUME") {
                in >> read_to;
            } else if (kind == "YIELDED") {
                if (!(in >> yielded >> yielded_tokens) || yielded < 0 || yielded_tokens < 0 ||
                    yielded_tokens > static_cast<int64_t>(ids.size()))
                    throw std::runtime_error("invalid yield result");
            } else if (kind == "DONE") {
                int generated = 0, prompt = 0;
                double pp = 0, decode = 0;
                std::string finish;
                if (!(in >> generated >> prompt >> pp >> decode >> finish) || generated < 0 || prompt < 0 ||
                    !std::isfinite(pp) || !std::isfinite(decode) ||
                    (finish != "stop" && finish != "length" && finish != "cancel"))
                    throw std::runtime_error("invalid engine DONE");
                TextJson last{{"generated", generated},
                              {"prompt_tokens", prompt},
                              {"prompt_ms", pp},
                              {"decode_ms", decode},
                              {"finish", stopping ? "cancel" : finish},
                              {"reused", 0}};
                if (yielded >= 0) {
                    last["_yielded_slot"] = yielded;
                    last["_yielded_tokens"] = yielded_tokens;
                }
                for (auto field : {"drafts_accepted", "drafts_offered", "reused", "hits", "lookups",
                                   "ram_blobs", "file_blobs"}) {
                    int64_t value = 0;
                    if (!(in >> value))
                        break;
                    last[field] = value;
                }
                double mb = 0;
                if (in >> mb)
                    last["file_mb"] = mb;
                int64_t n = 0;
                if (in >> n)
                    last["prompt_read"] = n;
                if (in >> n)
                    last["offloaded"] = n;
                {
                    std::lock_guard<std::mutex> l(state_mutex_);
                    state_["last"] = last;
                }
                return last;
            }
        }
    } catch (const ApiError &) {
        if (sent && !refused)
            discard(channel);
        throw;
    } catch (...) {
        discard(channel);
        throw ApiError(
            503,
            "the engine stopped or failed; this request was not replayed, the next request can restart it",
            "server_error");
    }
}
TextJson ServerEngine::control(const std::string &action, const std::function<bool()> &cancelled, bool idle) {
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (occupied_ || !queue_.empty() || parallel_leases_ > 0)
            throw ApiError(409, "a request is running or queued", "server_error", {}, "model_busy");
        if (cancelled() || shutting_down_)
            throw ApiError(499, "request cancelled", "server_error");
        if (idle && (!loaded() || std::chrono::duration<double>(Clock::now().time_since_epoch()).count() -
                                          last_activity_.load() <
                                      cfg_.value("idle_unload_s", 0.)))
            return {{"status", "busy"}};
        occupied_ = true;
        maintenance_ = true;
    }
    struct Release {
        ServerEngine *e;
        ~Release() { e->leave(); }
    } release{this};
    if (action == "load") {
        try {
            if (!loaded())
                start();
        } catch (...) {
            throw ApiError(503, "engine could not be loaded; see engine log", "server_error");
        }
        return {{"loaded", true}, {"status", "loaded"}};
    }
    if (action == "unload") {
        const bool was_loaded = loaded(), was_unloaded = unloaded_;
        if (process_)
            process_->finish();
        discard();
        unloaded_ = was_loaded || was_unloaded;
        if (after_unload_)
            after_unload_();
        return {{"loaded", false}, {"status", was_loaded ? "unloaded" : "not loaded"}};
    }
    throw ApiError(400, "unsupported engine control");
}
void ServerEngine::shutdown() {
    shutting_down_ = true;
    idle_changed_.notify_all();
    queue_changed_.notify_all();
}
void ServerEngine::check_stall(const std::shared_ptr<EngineChannel> &channel, Clock::time_point heard,
                               Watch &watch) {
    if (stall_timeout_ <= 0)
        return;
    if (!watch.baseline) {
        watch.baseline = channel->activity();
        watch.next = stall_timeout_;
        return;
    }
    const auto age = elapsed(heard);
    if (age < watch.next)
        return;
    const auto now = channel->activity();
    bool busy = false;
    try {
        if (gpu_busy_)
            busy = gpu_busy_();
    } catch (...) {
    }
    if (now && !busy && now->first - watch.baseline->first <= .5 && now->second >= watch.baseline->second &&
        now->second - watch.baseline->second <= (1u << 20))
        throw std::runtime_error("engine is frozen without CPU, I/O or GPU activity");
    watch.baseline = now;
    watch.next = now ? age + stall_timeout_ : std::numeric_limits<double>::infinity();
    if (now && !watch.warned) {
        std::cerr << "engine is silent but still working; continuing to wait\n";
        watch.warned = true;
    }
}
TextJson ServerEngine::exchange(const std::string &command, const std::string &want,
                                const std::function<bool()> &cancelled) {
    if (batch_ > 0 && want != "VRAM")
        throw ApiError(501, "disk sessions are unavailable with parallel requests", "server_error");
    if (batch_ > 0 && parallel_leases_ > 0)
        throw ApiError(409, "a request is running or queued", "server_error", {}, "model_busy");
    enter(cancelled);
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        maintenance_ = true;
    }
    auto lease = std::unique_ptr<Lease>(new Lease(this));
    std::lock_guard<std::recursive_mutex> lifecycle(lifecycle_mutex_);
    if (batch_ > 0 && want != "VRAM")
        throw ApiError(501, "disk sessions are unavailable with parallel requests", "server_error");
    if (!loaded()) {
        if (want == "VRAM") {
            vram_reserve_ = command == "VRAM" ? TextJson(nullptr) : parse_text_json(command.substr(5));
            return {{"status", "not loaded"},
                    {"reserve_mib", vram_reserve_},
                    {"note", "applied when the model loads"}};
        }
        throw ApiError(503, "the model is not loaded", "server_error", {}, "model_unloaded");
    }
    auto heard = Clock::now();
    double allow = silence_;
    uint64_t last_done = 0;
    try {
        process_->send(command);
        for (;;) {
            if (cancelled() || shutting_down_)
                throw std::runtime_error("engine control cancelled");
            if (allow > 0 && elapsed(heard) >= allow)
                throw std::runtime_error("engine control deadline exceeded");
            std::string line;
            if (!process_->receive(line, std::chrono::milliseconds(100)))
                continue;
            std::istringstream in(line);
            std::string kind;
            in >> kind;
            const auto count = [](const std::string &s) -> uint64_t {
                if (s.empty() ||
                    !std::all_of(s.begin(), s.end(), [](unsigned char c) { return c >= '0' && c <= '9'; }))
                    throw std::runtime_error("invalid engine control count");
                return std::stoull(s);
            };
            if (kind == "INFO" || kind == "WARN") {
                heard = Clock::now();
                continue;
            }
            if (kind == "SESSION") {
                std::string a, b, extra;
                if (!(in >> a >> b) || in >> extra)
                    throw std::runtime_error("invalid SESSION response");
                const auto n = count(a), total = count(b);
                if (n > total || n < last_done)
                    throw std::runtime_error("invalid session progress");
                last_done = n;
                heard = Clock::now();
                allow = silence_;
                continue;
            }
            if (kind == "SWAIT") {
                std::string phase, seconds, extra;
                if (!(in >> phase >> seconds) || in >> extra ||
                    !std::all_of(phase.begin(), phase.end(), [](unsigned char c) {
                        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
                    }))
                    throw std::runtime_error("invalid session wait");
                const auto n = count(seconds);
                if (n < 1 || n > 3600)
                    throw std::runtime_error("invalid session deadline");
                allow = silence_ > 0 ? std::max(silence_, double(n)) : 0;
                heard = Clock::now();
                continue;
            }
            if (kind == "SERR") {
                std::string error, published, message;
                if (!(in >> error >> published) || (published != "0" && published != "1"))
                    throw std::runtime_error("invalid session error");
                const std::map<std::string, int> codes{
                    {"invalid", 400}, {"storage", 507}, {"memory", 503}, {"io", 500}};
                if (!codes.count(error))
                    throw std::runtime_error("invalid session error kind");
                std::getline(in, message);
                ApiError e(codes.at(error), trim_text(message),
                           codes.at(error) < 500 ? "invalid_request_error" : "server_error");
                e.details = {{"kind", error}, {"code", codes.at(error)}};
                if (published == "1")
                    e.details["published"] = true;
                throw e;
            }
            if (kind == "ERR" && want == "VRAM") {
                std::string message;
                std::getline(in, message);
                throw ApiError(400, trim_text(message));
            }
            if (kind != want)
                throw std::runtime_error("engine control lost protocol synchronization");
            if (want == "VRAM") {
                auto result = facts(in);
                vram_reserve_ = command == "VRAM" ? TextJson(nullptr) : parse_text_json(command.substr(5));
                {
                    std::lock_guard<std::mutex> lock(state_mutex_);
                    state_["info"].update(result);
                    state_["info"]["vram"] = result;
                }
                return result;
            }
            std::string tokens, bytes, ms, extra;
            if (!(in >> tokens >> bytes >> ms) || in >> extra)
                throw std::runtime_error("invalid session result");
            size_t used = 0;
            const auto time = std::stod(ms, &used);
            if (used != ms.size() || !std::isfinite(time) || time < 0)
                throw std::runtime_error("invalid session duration");
            return {{"tokens", count(tokens)}, {"bytes", count(bytes)}, {"ms", time}};
        }
    } catch (const ApiError &) {
        throw;
    } catch (...) {
        discard();
        throw ApiError(want == "VRAM" ? 503 : 500,
                       "the engine stopped or lost step during control; the next request restarts it",
                       "server_error", {}, "engine_died");
    }
}
TextJson ServerEngine::vram(const TextJson &reserve, const std::function<bool()> &cancelled) {
    if (!reserve.is_null() &&
        (!reserve.is_number_integer() || reserve.get<int64_t>() < 0 || reserve.get<int64_t>() > 2147483647))
        throw ApiError(400, "reserve_mib must be a nonnegative integer or null");
    auto result =
        exchange("VRAM" + (reserve.is_null() ? std::string() : " " + reserve.dump()), "VRAM", cancelled);
    if (!result.contains("status"))
        result["status"] = "ok";
    return result;
}
TextJson ServerEngine::session_file(const std::string &action, const std::filesystem::path &path,
                                    const std::function<bool()> &cancelled) {
    const auto name = path_utf8(path);
    if ((action != "save" && action != "restore") || name.find_first_of("\r\n") != std::string::npos ||
        name.find('\0') != std::string::npos)
        throw ApiError(400, "invalid session command");
    return exchange((action == "save" ? "SAVE " : "RESTORE ") + name, action == "save" ? "SAVED" : "RESTORED",
                    cancelled);
}
} // namespace strata::runtime
