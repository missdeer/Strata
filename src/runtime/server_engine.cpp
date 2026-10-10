#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <cstdlib>
#endif
#include "strata/runtime/path.hpp"
#include "strata/runtime/server_engine.hpp"
#include <algorithm>
#include <cmath>
#include <set>
#include <sstream>

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
        auto parsed = TextJson::parse(value, nullptr, false);
        result[part.substr(0, p)] = parsed.is_number() ? parsed : TextJson(value);
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
                                          "reasoning_budget_tokens"};
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
        if (arg == "--batch" || arg == "--slots" || arg == "--vision" || arg == "--serve")
            throw std::invalid_argument("native server does not yet support engine option: " +
                                        arg.get<std::string>());
    if (active(cfg, "parallel") && cfg["parallel"] != 1)
        throw std::invalid_argument("native server currently supports parallel=1");
    for (auto key :
         {"api_monitor", "lazy_load", "fit_max_tokens", "tool_call_recovery", "open_browser", "gpus_asked"})
        if (cfg.contains(key) && !cfg[key].is_boolean())
            throw std::invalid_argument(std::string(key) + " must be boolean");
    if (cfg.value("open_browser", false))
        throw std::invalid_argument("native server does not yet support open_browser=true");
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
    if (cfg.contains("gpu") && !(cfg["gpu"].is_number_integer() || cfg["gpu"].is_string()))
        throw std::invalid_argument("gpu must be an index");
    if (cfg.contains("gpu") && cfg["gpu"].is_string() &&
        cfg["gpu"].get<std::string>().find(',') != std::string::npos)
        throw std::invalid_argument("native server does not yet support multi-GPU ordering");
    if (cfg.contains("lib_dirs")) {
        if (!cfg["lib_dirs"].is_array())
            throw std::invalid_argument("lib_dirs must be an array");
        for (const auto &d : cfg["lib_dirs"])
            if (!d.is_string())
                throw std::invalid_argument("lib_dirs must contain strings");
    }
    if (cfg.contains("aliases") && !cfg["aliases"].is_array())
        throw std::invalid_argument("aliases must be an array");
    for (const auto &alias : cfg.value("aliases", TextJson::array()))
        if (!alias.is_string() || alias.get<std::string>().empty())
            throw std::invalid_argument("aliases must contain nonempty strings");
    if (cfg.contains("sampling") && !cfg["sampling"].is_object())
        throw std::invalid_argument("sampling must be an object");
    sampling_keys(cfg.value("sampling", TextJson::object()));
}
std::vector<std::string> server_engine_args(const TextJson &cfg) {
    auto args = cfg.at("args").get<std::vector<std::string>>();
    args.insert(args.begin(), "--serve");
    return args;
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
    if (active(s, "strata_tune"))
        throw ApiError(400, "native server does not yet support strata_tune");
    return out;
}
ServerEngine::ServerEngine(TextJson cfg) : cfg_(std::move(cfg)) {
    state_["info"] = TextJson::object();
    silence_ = cfg_.value("engine_silence_s", 300.);
    ready_timeout_ = timeout("STRATA_ENGINE_READY_S", 900);
    stop_timeout_ = timeout("STRATA_ENGINE_STOP_S", 300);
    const auto args = cfg_["args"].get<std::vector<std::string>>();
    for (size_t i = 0; i + 1 < args.size(); ++i)
        if (args[i] == "--max-context")
            context_ = std::stoi(args[i + 1]);
    if (!cfg_.value("lazy_load", false))
        start();
}
ServerEngine::~ServerEngine() {
    shutdown();
    if (process_)
        process_->finish();
}
void ServerEngine::discard() {
    loaded_ = false;
    process_.reset();
    can_stop_ = false;
}
void ServerEngine::start() {
    discard();
    const auto cwd = std::filesystem::absolute(utf8_path(cfg_.value("cwd", ".")));
    auto exe = utf8_path(cfg_["exe"].get<std::string>());
    if (exe.is_relative())
        exe = cwd / exe;
    auto args = server_engine_args(cfg_);
    args.insert(args.begin(), path_utf8(exe));
    auto log = utf8_path(cfg_.value("log", ""));
    if (!log.empty() && log.is_relative())
        log = cwd / log;
    process_ = std::make_unique<EngineProcess>(args, cwd, log);
    const auto born = Clock::now();
    try {
        for (;;) {
            if (shutting_down_)
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
            loaded_ = true;
            break;
        }
    } catch (...) {
        discard();
        throw;
    }
}
void ServerEngine::enter(const std::function<bool()> &cancelled) {
    std::unique_lock<std::mutex> lock(queue_mutex_);
    if (queue_.size() >= 128)
        throw ApiError(503, "request queue is full", "server_error");
    const auto ticket = next_++;
    queue_.push_back(ticket);
    for (;;) {
        if (cancelled() || shutting_down_) {
            queue_.erase(std::find(queue_.begin(), queue_.end(), ticket));
            queue_changed_.notify_all();
            throw ApiError(499, "request cancelled", "server_error");
        }
        if (!occupied_ && queue_.front() == ticket) {
            queue_.pop_front();
            occupied_ = true;
            return;
        }
        queue_changed_.wait_for(lock, std::chrono::milliseconds(100));
    }
}
void ServerEngine::leave() {
    std::lock_guard<std::mutex> l(queue_mutex_);
    occupied_ = false;
    queue_changed_.notify_all();
}
std::unique_ptr<ServerEngine::Lease> ServerEngine::acquire(const std::function<bool()> &cancelled) {
    enter(cancelled);
    return std::unique_ptr<Lease>(new Lease(this));
}
TextJson ServerEngine::snapshot() {
    std::lock_guard<std::mutex> q(queue_mutex_);
    std::lock_guard<std::mutex> s(state_mutex_);
    auto out = state_;
    out["busy"] = occupied_;
    out["queued"] = queue_.size();
    out["loaded"] = loaded();
    return out;
}
TextJson ServerEngine::generate(const std::vector<int> &ids, int max_new, const TextJson &sampling,
                                const std::function<bool(int)> &token, const std::function<bool()> &cancelled,
                                const Lease *lease) {
    const auto keys = sampling_keys(sampling);
    auto owned = lease ? nullptr : acquire(cancelled);
    if (lease && lease->owner_ != this)
        throw std::invalid_argument("engine lease belongs to another engine");
    bool sent = false, stopping = false;
    auto heard = Clock::now(), beat = heard, stopped_at = heard;
    auto allow = silence_ + std::min<size_t>(ids.size(), 32768) / 50.;
    try {
        if (!loaded())
            start();
        if (static_cast<int64_t>(ids.size()) + max_new + 8 > context())
            throw ApiError(400, "prompt and max_tokens exceed engine context");
        std::string command = "GEN " + std::to_string(max_new) + keys + " ";
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
        process_->send(command);
        sent = true;
        heard = beat = Clock::now();
        int count = 0, read_to = 0;
        auto first_token = Clock::now();
        for (;;) {
            if (!stopping && (cancelled() || shutting_down_)) {
                stopping = true;
                stopped_at = Clock::now();
                if (can_stop_)
                    process_->send("STOP");
                else {
                    discard();
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
            if (!process_->receive(line, std::chrono::milliseconds(100))) {
                if (!stopping && elapsed(beat) >= 10) {
                    beat = Clock::now();
                    if (!token(-1)) {
                        stopping = true;
                        stopped_at = Clock::now();
                        if (can_stop_)
                            process_->send("STOP");
                        else
                            discard();
                    }
                }
                if (!process_)
                    return {{"finish", "cancel"},
                            {"generated", count},
                            {"prompt_tokens", ids.size()},
                            {"reused", 0}};
                continue;
            }
            heard = beat = Clock::now();
            std::istringstream in(line);
            std::string kind;
            in >> kind;
            if (kind == "ERR")
                throw std::runtime_error("engine request failed; see engine log");
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
                        process_->send("STOP");
                    else {
                        discard();
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
                        process_->send("STOP");
                }
            } else if (kind == "RESUME") {
                in >> read_to;
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
        if (sent)
            discard();
        throw;
    } catch (...) {
        discard();
        throw ApiError(
            503,
            "the engine stopped or failed; this request was not replayed, the next request can restart it",
            "server_error");
    }
}
TextJson ServerEngine::control(const std::string &action, const std::function<bool()> &cancelled) {
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (occupied_ || !queue_.empty())
            throw ApiError(409, "a request is running or queued", "server_error", {}, "model_busy");
        if (cancelled() || shutting_down_)
            throw ApiError(499, "request cancelled", "server_error");
        occupied_ = true;
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
        if (process_)
            process_->finish();
        discard();
        return {{"loaded", false}, {"status", "unloaded"}};
    }
    throw ApiError(400, "unsupported engine control");
}
void ServerEngine::shutdown() {
    shutting_down_ = true;
    queue_changed_.notify_all();
}
} // namespace strata::runtime
