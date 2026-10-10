#include "strata/runtime/host_commands.hpp"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#else
#include <csignal>
#include <unistd.h>
#include <sys/wait.h>
#endif
#include "httplib.h"
#include "strata/runtime/path.hpp"
#include "strata/runtime/server_api.hpp"
#include "strata/runtime/server_state.hpp"
#include "strata/runtime/server_telemetry.hpp"
#include "strata/runtime/server_mcp.hpp"
#include "strata/runtime/server_vision.hpp"
#include "strata/runtime/server_security.hpp"
#include "strata/runtime/server_metrics.hpp"
#include "strata/runtime/server_recovery.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <iostream>
#include <set>
#include <thread>
#include <future>
#include <cmath>
#include <limits>

namespace {
using namespace strata::runtime;
using J = TextJson;
std::atomic<bool> stopping{false};
#ifdef _WIN32
BOOL WINAPI signal_handler(DWORD signal) {
    if (signal == CTRL_C_EVENT || signal == CTRL_BREAK_EVENT || signal == CTRL_CLOSE_EVENT ||
        signal == CTRL_SHUTDOWN_EVENT) {
        stopping = true;
        return TRUE;
    }
    return FALSE;
}
#else
void signal_handler(int) { stopping = true; }
#endif
void json(httplib::Response &res, int status, const J &body) {
    res.status = status;
    res.set_content(body.dump(), "application/json; charset=utf-8");
}
std::vector<std::string> keys(const J &value) {
    std::vector<std::string> out;
    if (value.is_null())
        return out;
    if (value.is_array()) {
        if (value.empty())
            throw std::invalid_argument("an API key was given but it is empty");
        for (const auto &key : value) {
            if (!key.is_string() || key.get<std::string>().empty())
                throw std::invalid_argument("api_key must contain nonempty strings");
            const auto normalized = trim_text(key.get<std::string>());
            if (normalized.empty())
                throw std::invalid_argument("an API key was given but it is empty");
            out.push_back(normalized);
        }
        return out;
    }
    if (!value.is_string())
        throw std::invalid_argument("api_key must be a string or array");
    std::istringstream in(value.get<std::string>());
    std::string key;
    while (std::getline(in, key, ',')) {
        key = trim_text(key);
        if (!key.empty())
            out.push_back(key);
    }
    if (out.empty() && !value.get<std::string>().empty())
        throw std::invalid_argument("an API key was given but it is empty");
    return out;
}
std::string capture_text(const std::string &text, size_t maximum = 262144) {
    size_t at = 0, count = 0;
    while (at < text.size() && count < maximum) {
        ++at;
        while (at < text.size() && (static_cast<unsigned char>(text[at]) & 0xc0) == 0x80)
            ++at;
        ++count;
    }
    return text.substr(0, at);
}
bool matches(const std::string &a, const std::string &b) {
    size_t diff = a.size() ^ b.size();
    for (size_t i = 0; i < std::max(a.size(), b.size()); ++i)
        diff |= (i < a.size() ? static_cast<unsigned char>(a[i]) : 0) ^
                (i < b.size() ? static_cast<unsigned char>(b[i]) : 0);
    return diff == 0;
}
std::optional<std::string> latin1_key(const std::string &key) {
    std::string out;
    for (size_t i = 0; i < key.size();) {
        const auto c = static_cast<unsigned char>(key[i++]);
        if (c < 128)
            out += char(c);
        else if ((c == 0xc2 || c == 0xc3) && i < key.size()) {
            const auto next = static_cast<unsigned char>(key[i++]);
            if ((next & 0xc0) != 0x80)
                return {};
            out += char(((c & 31) << 6) | (next & 63));
        } else
            return {};
    }
    return out;
}
bool authorized(const httplib::Request &req, const std::vector<std::string> &allowed) {
    if (allowed.empty())
        return true;
    auto given = req.get_header_value("Authorization");
    auto prefix = given.substr(0, 7);
    std::transform(prefix.begin(), prefix.end(), prefix.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (prefix == "bearer ") {
        given = given.substr(7);
        const auto space = [](unsigned char c) {
            return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f' || c == 0x85 ||
                   c == 0xa0;
        };
        while (!given.empty() && space(static_cast<unsigned char>(given.front())))
            given.erase(given.begin());
        while (!given.empty() && space(static_cast<unsigned char>(given.back())))
            given.pop_back();
    } else
        given = req.get_header_value("x-api-key");
    bool found = false;
    for (const auto &key : allowed) {
        found = matches(given, key) || found;
        if (const auto latin = latin1_key(key))
            found = matches(given, *latin) || found;
    }
    return found;
}
bool loopback(const std::string &host) { return host == "127.0.0.1" || host == "localhost" || host == "::1"; }
std::filesystem::path web_path(const J &cfg) {
    if (cfg.contains("web_root"))
        return std::filesystem::absolute(utf8_path(cfg["web_root"].get<std::string>()));
    std::filesystem::path executable;
#ifdef _WIN32
    std::wstring path(32768, L'\0');
    const auto n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (n && n < path.size()) {
        path.resize(n);
        executable = std::filesystem::path(path).parent_path();
    }
#else
    std::array<char, 32768> path{};
    const auto n = readlink("/proc/self/exe", path.data(), path.size());
    if (n > 0 && static_cast<size_t>(n) < path.size())
        executable = utf8_path(std::string(path.data(), static_cast<size_t>(n))).parent_path();
#endif
    for (const auto &candidate : std::vector<std::filesystem::path>{
             executable / "web", executable / "serve/web", std::filesystem::current_path() / "serve/web",
             utf8_path(cfg.value("cwd", ".")) / "serve/web"})
        if (std::filesystem::is_regular_file(candidate / "index.html"))
            return std::filesystem::absolute(candidate);
    return std::filesystem::absolute("serve/web");
}
struct History {
    std::mutex mutex;
    J last_timings = nullptr;
    int64_t last_at = 0;
    J requests = J::array(), records = J::array(),
      totals = {{"since", std::time(nullptr)},
                {"requests", 0},
                {"prompt_tokens", 0},
                {"output_tokens", 0},
                {"reused", 0},
                {"prompt_ms", 0.},
                {"decode_ms", 0.},
                {"drafts_offered", 0},
                {"drafts_accepted", 0}};
    void upsert_unlocked(const J &record) {
        if (record.is_null())
            return;
        for (auto &r : records)
            if (r["id"] == record["id"]) {
                r = record;
                return;
            }
        records.insert(records.begin(), record);
        if (records.size() > 100)
            records.erase(records.end() - 1);
    }
    void update(const J &record) {
        if (record.is_null())
            return;
        std::lock_guard<std::mutex> l(mutex);
        upsert_unlocked(record);
    }
    void add(J row, J record) {
        std::lock_guard<std::mutex> l(mutex);
        last_at = std::time(nullptr);
        if (row.value("timings", J(nullptr)).is_object()) {
            last_timings = row["timings"];
            last_timings["at"] = last_at;
        }
        totals["requests"] = totals["requests"].get<int>() + 1;
        {
            const auto u = row.value("usage", J::object());
            totals["prompt_tokens"] =
                totals["prompt_tokens"].get<int64_t>() +
                row.value("prompt_tokens", u.value("prompt_tokens", u.value("input_tokens", 0)));
            totals["output_tokens"] =
                totals["output_tokens"].get<int64_t>() +
                row.value("output_tokens", u.value("completion_tokens", u.value("output_tokens", 0)));
            totals["reused"] =
                totals["reused"].get<int64_t>() +
                row.value("reused",
                          u.value("prompt_tokens_details", u.value("input_tokens_details", J::object()))
                              .value("cached_tokens", u.value("cache_read_input_tokens", 0)));
        }
        for (auto field : {"prompt_ms", "decode_ms", "drafts_offered", "drafts_accepted"})
            totals[field] = totals[field].get<double>() + row.value(field, 0.);
        requests.insert(requests.begin(), row);
        if (requests.size() > 500)
            requests.erase(requests.end() - 1);
        upsert_unlocked(record);
    }
};
int run(const std::vector<std::string> &argv) {
    try {
        std::string config, host_override, key_override, web_override;
        J overrides = J::object();
        std::string mcp_file, tokenizer_override;
        bool open_requested = false;
        int port_override = 0;
        bool validate = false;
        std::set<std::string> seen;
        for (size_t i = 1; i < argv.size(); ++i) {
            const auto &arg = argv[i];
            if (!seen.insert(arg).second)
                throw std::invalid_argument("duplicate option: " + arg);
            if (arg == "--help") {
                std::cout << "strata-host serve --engine strata --config server.json [--host IP] [--port N] "
                             "[--api-key KEY] [--web-root DIR] [--validate]\n";
                return 0;
            }
            if (arg == "--validate") {
                validate = true;
                continue;
            }
            if (arg == "--open") {
                open_requested = true;
                continue;
            }
            if (arg == "--lazy" || arg == "--api-monitor" || arg == "--fit-max-tokens") {
                overrides[arg == "--lazy"          ? "lazy_load"
                          : arg == "--api-monitor" ? "api_monitor"
                                                   : "fit_max_tokens"] = true;
                continue;
            }
            if (i + 1 == argv.size())
                throw std::invalid_argument("missing option value");
            const auto value = argv[++i];
            if (arg == "--config")
                config = value;
            else if (arg == "--host")
                host_override = value;
            else if (arg == "--port")
                port_override = std::stoi(value);
            else if (arg == "--api-key")
                key_override = value;
            else if (arg == "--web-root")
                web_override = value;
            else if (arg == "--mcp-config")
                mcp_file = value;
            else if (arg == "--tokenizer")
                tokenizer_override = value;
            else if (arg == "--gpu")
                overrides["gpu"] = value;
            else if (arg == "--before-load")
                overrides["before_load"] = value;
            else if (arg == "--slot-save-path")
                overrides["slot_save_path"] = path_utf8(std::filesystem::absolute(utf8_path(value)));
            else if (arg == "--idle-unload")
                overrides["idle_unload_s"] = std::stod(value);
            else if (arg == "--min-free-vram-mib")
                overrides["min_free_vram_mib"] = std::stoi(value);
            else if (arg != "--engine" || value != "strata")
                throw std::invalid_argument("unsupported option: " + arg);
        }
        if (config.empty())
            throw std::invalid_argument("--config is required");
        auto cfg = read_text_json(utf8_path(config));
        cfg.update(overrides);
        if (!tokenizer_override.empty() && !cfg.contains("tokenizer"))
            cfg["tokenizer"] = tokenizer_override;
        if (!mcp_file.empty()) {
            const auto file = read_text_json(utf8_path(mcp_file));
            const auto block = file.value("mcpServers", file.value("mcp_servers", J(nullptr)));
            if (!block.is_object())
                throw std::invalid_argument("mcp-config requires mcpServers object");
            if (!cfg.contains("mcpServers"))
                cfg["mcpServers"] = J::object();
            cfg["mcpServers"].update(block);
        }
        if (!web_override.empty())
            cfg["web_root"] = web_override;
        validate_server_config(cfg);
        const auto host = host_override.empty() ? cfg.value("host", "127.0.0.1") : host_override;
        const auto port = port_override ? port_override : cfg.value("port", 8095);
        if (port < 1 || port > 65535)
            throw std::invalid_argument("port outside 1..65535");
        J key = cfg.value("api_key", J(nullptr));
        const auto environment_key = server_environment("STRATA_API_KEY");
        if (!environment_key.empty())
            key = environment_key;
        if (!key_override.empty())
            key = key_override;
        const auto api_keys = keys(key);
        if (!loopback(host) && api_keys.empty())
            throw std::invalid_argument("non-loopback listener requires an API key");
        const auto web = web_path(cfg);
        if (!std::filesystem::is_regular_file(web / "index.html"))
            throw std::invalid_argument("web resources missing; set --web-root");
        const auto tokenizer = Tokenizer::load(utf8_path(cfg["tokenizer"].get<std::string>()));
        auto template_path = cfg.value("template", "");
        if (template_path.empty()) {
            const auto local =
                std::filesystem::path(utf8_path(cfg["tokenizer"].get<std::string>())) / "chat_template.jinja";
            template_path = path_utf8(
                std::filesystem::is_regular_file(local) ? local : web.parent_path() / "chat_template.jinja");
        }
        const ChatTemplate chat(utf8_path(template_path));
        bool effort_end = false;
        if (cfg.value("effort_position", "start") == "end") {
            const auto role = tokenizer.encode("system", true);
            auto exe = utf8_path(cfg["exe"].get<std::string>());
            if (exe.is_relative())
                exe = std::filesystem::absolute(utf8_path(cfg.value("cwd", ".")) / exe);
            try {
                if (role.size() == 1 && read_text(exe).find("--tail-role-token") != std::string::npos) {
                    cfg["args"].push_back("--tail-role-token");
                    cfg["args"].push_back(std::to_string(role[0]));
                    effort_end = true;
                }
            } catch (...) {
            }
        }
        if (validate) {
            std::cout << J{{"valid", true},
                           {"native", true},
                           {"dialects", J::array({"chat", "responses", "anthropic"})},
                           {"engine_args", server_engine_args(cfg)}}
                             .dump()
                      << std::endl;
            return 0;
        }
        httplib::Server http;
        ServerSecurity security(cfg, host);
        size_t body_limit = 256u << 20;
        try {
            const auto setting = server_environment("STRATA_MAX_BODY_MIB");
            if (!setting.empty()) {
                size_t consumed = 0;
                const auto n = std::stoll(setting, &consumed);
                if (consumed == setting.size() && n > 0 &&
                    uint64_t(n) <= std::numeric_limits<size_t>::max() / (1u << 20))
                    body_limit = static_cast<size_t>(n) * (1u << 20);
            }
        } catch (...) {
        }
        http.set_payload_max_length(body_limit);
        http.set_read_timeout(30, 0);
        http.set_write_timeout(30, 0);
        http.set_keep_alive_max_count(20);
        http.new_task_queue = [] { return new httplib::ThreadPool(16, 16, 128); };
        if (!http.bind_to_port(host, port))
            throw std::runtime_error("HTTP port unavailable");
        McpHub mcp(cfg);
        std::unique_ptr<NativeVision> vision;
        if (cfg.contains("vision") && cfg["vision"].is_object())
            vision = std::make_unique<NativeVision>(cfg);
        std::atomic<ServerEngine *> engine_view{nullptr};
        const auto gpu_indices = server_gpu_indices(cfg);
        ServerTelemetry telemetry(
            gpu_indices.empty() ? 0 : gpu_indices.front(), cfg.value("backend", "") == "hip",
            [&] {
                auto *instance = engine_view.load();
                if (!instance)
                    return J::object();
                const auto s = instance->snapshot();
                return J{{"tok_s", s.value("tok_s", J(nullptr))}};
            },
            gpu_indices);
        ServerEngine engine(
            cfg,
            [&] {
                const auto minimum = cfg.value("min_free_vram_mib", 0);
                const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(15);
                while (minimum > 0) {
                    const auto tel = telemetry.snapshot();
                    const auto free = tel["now"].value("gpu_mem_free", J(nullptr));
                    if (!free.is_number() || free.get<double>() / (1u << 20) >= minimum)
                        break;
                    if (stopping)
                        throw ApiError(499, "loading cancelled", "server_error");
                    if (std::chrono::steady_clock::now() >= end)
                        throw ApiError(503, "GPU free VRAM is below min_free_vram_mib", "server_error");
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                }
                if (vision)
                    vision->ensure_loaded([] { return stopping.load(); });
            },
            [&] {
                if (vision)
                    vision->unload();
            });
        engine_view = &engine;
        engine.set_gpu_busy([&] { return telemetry.snapshot()["now"].value("gpu_util", 0.) > 0; });
        struct StopTelemetry {
            ServerTelemetry &telemetry;
            ~StopTelemetry() { telemetry.close(); }
        } stop_telemetry{telemetry};
        History history;
        const auto started_at = std::time(nullptr);
        NativeLatencies latencies;
        std::mutex settings_mutex, config_mutex;
        std::mutex codex_mutex;
        J codex_owner = nullptr, codex_tools = nullptr;
        J shared = J::object();
        auto shared_path = std::filesystem::absolute(utf8_path(config));
        shared_path.replace_extension("");
        shared_path += L".shared-settings.json";
        if (std::filesystem::exists(shared_path))
            shared = shared_defaults(read_text_json(shared_path));
        const auto model = cfg.value("model_name", "qwen3.8-flash-next");
        std::vector<std::string> models{model};
        for (const auto &alias : server_aliases(cfg))
            models.push_back(alias);
        auto defaults = [&] {
            std::lock_guard<std::mutex> l(settings_mutex);
            auto d = cfg.value("sampling", J::object());
            if (cfg.contains("reasoning_budget_tokens"))
                d["reasoning_budget_tokens"] = cfg["reasoning_budget_tokens"];
            d.update(shared);
            return d;
        };
        auto status_view = [&]() -> J {
            const auto s = engine.snapshot(), tel = telemetry.snapshot(), info = s.value("info", J::object());
            const auto &hw = tel["now"];
            const auto &fixed = tel["static"];
            const auto scaled = [](const J &v, double scale, int precision = 0) -> J {
                if (!v.is_number())
                    return nullptr;
                const auto p = std::pow(10., precision);
                return std::round(v.get<double>() / scale * p) / p;
            };
            std::lock_guard<std::mutex> lock(history.mutex);
            auto vram = info.value("vram", J::object());
            const auto elastic = info.value("vram_elastic", J(false));
            vram["elastic"] = elastic.is_number() ? elastic.get<double>() != 0
                              : elastic.is_boolean()
                                  ? elastic.get<bool>()
                                  : elastic.is_string() && !elastic.get_ref<const std::string &>().empty();
            J gpu = nullptr, ram = nullptr;
            if (fixed.value("gpu_name", J(nullptr)).is_string())
                gpu = {{"name", fixed["gpu_name"]},
                       {"used_mib", scaled(hw.value("gpu_mem_used", J(nullptr)), 1u << 20)},
                       {"total_mib", scaled(hw.value("gpu_mem_total", J(nullptr)), 1u << 20)},
                       {"util_pct", hw.value("gpu_util", J(nullptr))},
                       {"temp_c", hw.value("gpu_temp", J(nullptr))},
                       {"power_w", hw.value("gpu_power", J(nullptr))}};
            if (hw.value("ram_total", J(nullptr)).is_number())
                ram = {{"used_gib", scaled(hw.value("ram_used", J(nullptr)), 1u << 30, 1)},
                       {"total_gib", scaled(hw["ram_total"], 1u << 30, 1)}};
            return {
                {"service", "strata"},
                {"model", model},
                {"loaded", engine.loaded()},
                {"auto_load", true},
                {"structured_output",
                 {{"formats", J::array({"json_object", "json_schema"})},
                  {"method", "prompt_and_validate"},
                  {"constrained_decoding", false},
                  {"stream_buffered", true}}},
                {"engine", info.value("version", J(nullptr))},
                {"started", started_at},
                {"uptime_s", std::time(nullptr) - started_at},
                {"cache_max_tokens", engine.context()},
                {"context", {{"native", engine.context()}, {"max_positions", engine.context()}}},
                {"concurrency",
                 {{"serving", std::max(1, engine.parallel())},
                  {"requested", std::max(1, engine.parallel())}}},
                {"dialects", J::array({"/v1/chat/completions", "/v1/messages", "/v1/responses"})},
                {"vision", {{"enabled", bool(vision)}, {"available", bool(vision)}, {"error", nullptr}}},
                {"activity",
                 {{"requests", history.totals["requests"].get<int64_t>() + (s.value("busy", false) ? 1 : 0)},
                  {"in_flight", s.value("queued", 0) + (s.value("busy", false) ? 1 : 0)},
                  {"last_request_at", history.last_at ? J(history.last_at) : J(nullptr)}}},
                {"last_timings", history.last_timings},
                {"vram", vram},
                {"machine", {{"at", std::time(nullptr)}, {"gpu", gpu}, {"ram", ram}}}};
        };
        auto protect = [&](const httplib::Request &req, httplib::Response &res) {
            if (!authorized(req, api_keys)) {
                json(res, 401,
                     ApiError(401, "invalid API key", "authentication_error")
                         .body(req.path.starts_with("/v1/messages")));
                return false;
            }
            return true;
        };
        http.set_pre_routing_handler([&](const httplib::Request &req, httplib::Response &res) {
            res.set_header("X-Content-Type-Options", "nosniff");
            res.set_header("Referrer-Policy", "no-referrer");
            const auto authority = req.get_header_value("Host"), origin = req.get_header_value("Origin"),
                       type = req.get_header_value("Content-Type");
            if (!security.host_allowed(authority, !api_keys.empty())) {
                json(res, 403, ApiError(403, "Host is not allowed").body());
                return httplib::Server::HandlerResponse::Handled;
            }
            if (req.path.starts_with("/v1/"))
                for (const auto &[key, value] :
                     security.cors(origin, req.get_header_value("Access-Control-Request-Headers")))
                    res.set_header(key, value);
            if (req.method == "OPTIONS") {
                res.status = 204;
                return httplib::Server::HandlerResponse::Handled;
            }
            if (req.method == "POST" && req.path.starts_with("/v1/") && api_keys.empty() && !origin.empty()) {
                if (!security.origin_allowed(origin, authority)) {
                    json(res, 403,
                         ApiError(403, "foreign web pages may not use this server without an API key",
                                  "forbidden")
                             .body());
                    return httplib::Server::HandlerResponse::Handled;
                }
                if (!type.starts_with("application/json")) {
                    json(res, 415, ApiError(415, "send application/json").body());
                    return httplib::Server::HandlerResponse::Handled;
                }
            }
            if (req.method == "POST" &&
                (req.path == "/settings" || req.path == "/config" || req.path == "/load" ||
                 req.path == "/unload" || req.path == "/v1/load" || req.path == "/v1/unload" ||
                 req.path == "/v1/vram" || req.path.starts_with("/slots/"))) {
                if (!protect(req, res))
                    return httplib::Server::HandlerResponse::Handled;
                security.own_page(origin, authority, type);
            }
            return httplib::Server::HandlerResponse::Unhandled;
        });
        http.set_exception_handler(
            [](const httplib::Request &req, httplib::Response &res, std::exception_ptr ptr) {
                try {
                    std::rethrow_exception(ptr);
                } catch (const ApiError &e) {
                    json(res, e.status, e.body(req.path.starts_with("/v1/messages")));
                } catch (const J::exception &) {
                    json(res, 400,
                         ApiError(400, "invalid JSON field type").body(req.path.starts_with("/v1/messages")));
                } catch (const std::invalid_argument &e) {
                    json(res, 400, ApiError(400, e.what()).body(req.path.starts_with("/v1/messages")));
                } catch (...) {
                    json(res, 500,
                         ApiError(500, "native server operation failed", "server_error")
                             .body(req.path.starts_with("/v1/messages")));
                }
            });
        http.set_error_handler([](const httplib::Request &req, httplib::Response &res) {
            if (res.body.empty())
                json(res, res.status,
                     ApiError(res.status, res.status == 404 ? "not found" : "HTTP request rejected")
                         .body(req.path.starts_with("/v1/messages")));
        });
        auto static_file = [&](const httplib::Request &req, httplib::Response &res) {
            auto name = req.path == "/" ? "index.html"
                        : req.path == "/api-monitor"
                            ? "monitor.html"
                            : req.path.substr(req.path.starts_with("/fonts/") ? 7 : 5);
            if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos ||
                name.find("..") != std::string::npos)
                throw ApiError(404, "not found");
            const auto ext = std::filesystem::path(name).extension().string();
            const std::map<std::string, std::string> mime{{".html", "text/html; charset=utf-8"},
                                                          {".js", "text/javascript; charset=utf-8"},
                                                          {".css", "text/css; charset=utf-8"},
                                                          {".svg", "image/svg+xml"},
                                                          {".woff2", "font/woff2"}};
            if (!mime.count(ext))
                throw ApiError(404, "not found");
            auto path = req.path.starts_with("/fonts/") ? web / "fonts" / name : web / name;
            if (!std::filesystem::is_regular_file(path))
                throw ApiError(404, "not found");
            res.set_content(read_text(path), mime.at(ext));
            res.set_header("Cache-Control", "no-cache");
        };
        http.Get("/", static_file);
        http.Get("/web/[^/]+", static_file);
        http.Get("/fonts/[^/]+", static_file);
        if (cfg.value("api_monitor", false))
            http.Get("/api-monitor", static_file);
        auto health = [&](const httplib::Request &, httplib::Response &res) {
            json(res, 200,
                 {{"status", "ok"},
                  {"max_context", engine.context()},
                  {"model", model},
                  {"images", bool(vision)},
                  {"api_key", !api_keys.empty()},
                  {"loaded", engine.loaded()},
                  {"service", "strata"}});
        };
        http.Get("/health", health);
        http.Get("/api/health", health);
        auto list_models = [&](const httplib::Request &req, httplib::Response &res) {
            if (!protect(req, res))
                return;
            J data = J::array();
            if (!engine.loaded() && !engine.unloaded() && cfg.value("idle_unload_s", 0.) <= 0) {
                json(res, 200, {{"object", "list"}, {"data", data}});
                return;
            }
            for (const auto &name : models) {
                J m{{"id", name},
                    {"object", "model"},
                    {"status", {{"value", engine.loaded() ? "loaded" : "unloaded"}}},
                    {"meta", {{"n_ctx", engine.context()}}},
                    {"architecture",
                     {{"input_modalities", vision ? J::array({"text", "image"}) : J::array({"text"})},
                      {"output_modalities", J::array({"text"})}}}};
                if (name != model)
                    m["alias_of"] = model;
                if (models.size() > 1)
                    m["aliases"] = std::vector<std::string>(models.begin() + 1, models.end());
                data.push_back(m);
            }
            json(res, 200, {{"object", "list"}, {"data", data}});
        };
        http.Get("/v1/models", list_models);
        http.Get("/models", list_models);
        http.Get("/status", [&](const httplib::Request &req, httplib::Response &res) {
            if (protect(req, res))
                json(res, 200, engine.snapshot());
        });
        http.Get("/slots", [&](const httplib::Request &req, httplib::Response &res) {
            if (protect(req, res)) {
                auto s = engine.snapshot();
                if (engine.parallel() > 0) {
                    auto slots = J::array();
                    for (const auto &b : s["slots"])
                        slots.push_back(
                            {{"id", b["slot"]},
                             {"n_ctx", engine.context()},
                             {"is_processing", b["state"] != "idle"},
                             {"n_prompt_tokens", b.value("prompt_tokens", b.value("held_tokens", 0))}});
                    json(res, 200, slots);
                    return;
                }
                json(res, 200,
                     engine.loaded() ? J::array({J{{"id", 0},
                                                   {"n_ctx", engine.context()},
                                                   {"is_processing", s["busy"]},
                                                   {"n_prompt_tokens", s.value("prompt_tokens", 0)}}})
                                     : J::array());
            }
        });
        http.Get("/mcp", [&](const httplib::Request &req, httplib::Response &res) {
            if (protect(req, res))
                json(res, 200, mcp.status());
        });
        http.Get("/props", [&](const httplib::Request &req, httplib::Response &res) {
            if (!protect(req, res))
                return;
            const auto name = req.get_param_value("model");
            if (!name.empty() && std::find(models.begin(), models.end(), name) == models.end())
                throw ApiError(404, "model not found");
            auto configured = defaults();
            J params = J::object();
            const std::set<std::string> sampling_names{"temperature",
                                                       "top_p",
                                                       "top_k",
                                                       "min_p",
                                                       "seed",
                                                       "repetition_penalty",
                                                       "presence_penalty",
                                                       "frequency_penalty",
                                                       "penalty_last_n"};
            for (auto it = configured.begin(); it != configured.end(); ++it)
                if (sampling_names.count(it.key()))
                    params[it.key()] = it.value();
            if (params.contains("repetition_penalty")) {
                params["repeat_penalty"] = params["repetition_penalty"];
                params.erase("repetition_penalty");
            }
            if (params.contains("penalty_last_n")) {
                params["repeat_last_n"] = params["penalty_last_n"];
                params.erase("penalty_last_n");
            }
            params["n_predict"] = configured.value("max_tokens", -1);
            if (!engine.loaded() && !engine.unloaded())
                throw ApiError(503, "the engine is not running", "server_error");
            J props = {{"default_generation_settings", {{"n_ctx", engine.context()}, {"params", params}}},
                       {"total_slots", std::max(1, engine.parallel())},
                       {"model_alias", model},
                       {"chat_template", read_text(utf8_path(template_path))},
                       {"chat_template_caps", chat.capabilities()},
                       {"modalities", {{"vision", bool(vision)}}},
                       {"models_autoload", true},
                       {"is_sleeping", !engine.loaded()}};
            std::string model_path = "pack/full", native;
            const auto args = cfg["args"].get<std::vector<std::string>>();
            for (size_t i = 0; i + 1 < args.size(); ++i) {
                if (args[i] == "--pack")
                    model_path = args[i + 1];
                if (args[i] == "--native")
                    native = args[i + 1];
            }
            props["model_path"] = native.empty() ? model_path : native;
            const auto info = engine.snapshot().value("info", J::object());
            if (info.value("version", J(nullptr)).is_string() && !info["version"].get<std::string>().empty())
                props["build_info"] = "Strata " + info["version"].get<std::string>();
            json(res, 200, props);
        });
        http.Get("/v1/status", [&](const httplib::Request &req, httplib::Response &res) {
            if (!protect(req, res))
                return;
            json(res, 200, status_view());
        });
        http.Get("/metrics", [&](const httplib::Request &req, httplib::Response &res) {
            if (!protect(req, res))
                return;
            const auto s = engine.snapshot();
            std::lock_guard<std::mutex> l(history.mutex);
            J rows = history.requests;
            if (req.get_param_value("requests") != "all" && rows.size() > 12)
                rows.erase(rows.begin() + 12, rows.end());
            auto info = s.value("info", J::object());
            info.update(J{{"model", model},
                          {"max_context", engine.context()},
                          {"images", bool(vision)},
                          {"native", true}});
            const auto tel = telemetry.snapshot();
            J metrics = {
                {"engine", info},
                {"live",
                 {{"state",
                   s.value("busy", false) ? s.value("generated", 0) > 0 ? "generating" : "reading" : "idle"},
                  {"queued", s.value("queued", 0)},
                  {"generated", s.value("generated", 0)},
                  {"prompt_tokens", s.value("prompt_tokens", 0)},
                  {"prompt_read",
                   s.value("progress", J(nullptr)).is_object() ? s["progress"].value("read", 0) : 0},
                  {"prompt_total", s.value("prompt_tokens", 0)},
                  {"tok_s", s.value("tok_s", J(nullptr))}}},
                {"requests", rows},
                {"requests_kept", history.requests.size()},
                {"totals", history.totals},
                {"hardware", tel["now"]},
                {"hardware_static", tel["static"]},
                {"history", tel["history"]},
                {"time", std::time(nullptr)}};
            if (!engine.loaded())
                metrics["live"]["state"] = "unloaded";
            if (engine.parallel() > 0) {
                metrics["live"]["slots"] = s["slots"];
                metrics["live"]["parallel"] = engine.parallel();
                int running = 0;
                for (const auto &slot : s["slots"])
                    if (slot["state"] != "idle")
                        ++running;
                metrics["live"]["running"] = running;
            }
            metrics["conversation_cache"] =
                conversation_cache(info, history.requests, history.totals, engine.cache_view());
            if (req.get_header_value("Accept").find("text/plain") != std::string::npos ||
                req.get_header_value("Accept").find("openmetrics") != std::string::npos ||
                req.get_param_value("format") == "prometheus")
                res.set_content(prometheus_metrics(metrics, latencies.snapshot()),
                                "text/plain; version=0.0.4; charset=utf-8");
            else
                json(res, 200, metrics);
        });
        if (cfg.value("api_monitor", false))
            http.Get("/api/requests", [&](const httplib::Request &req, httplib::Response &res) {
                if (!protect(req, res))
                    return;
                std::lock_guard<std::mutex> l(history.mutex);
                const auto id = req.get_param_value("id");
                if (id.empty()) {
                    auto summaries = history.records;
                    for (auto &summary : summaries) {
                        for (auto field : {"input", "output", "reasoning", "response"})
                            summary.erase(field);
                        if (!summary.contains("wallclock_s"))
                            summary["wallclock_s"] =
                                std::max(0., double(std::time(nullptr)) -
                                                 summary.value("started_at", double(std::time(nullptr))));
                    }
                    json(res, 200,
                         {{"requests", summaries},
                          {"retention", 100},
                          {"persistent", false},
                          {"loaded", engine.loaded()},
                          {"auto_load", true}});
                    return;
                }
                for (const auto &record : history.records)
                    if (record["id"] == id) {
                        json(res, 200, record);
                        return;
                    }
                throw ApiError(404, "request no longer retained");
            });
        http.Get("/settings", [&](const httplib::Request &req, httplib::Response &res) {
            if (!protect(req, res))
                return;
            std::lock_guard<std::mutex> l(settings_mutex);
            json(res, 200, {{"shared", !shared.empty()}, {"defaults", shared}});
        });
        http.Post("/settings", [&](const httplib::Request &req, httplib::Response &res) {
            if (!protect(req, res))
                return;
            if (!req.get_header_value("Content-Type").starts_with("application/json"))
                throw ApiError(415, "send application/json");
            const auto body = parse_text_json(req.body);
            if (!body.contains("defaults") || (!body["defaults"].is_null() && !body["defaults"].is_object()))
                throw ApiError(400, "body must contain defaults object or null");
            auto d = shared_defaults(body["defaults"]);
            std::lock_guard<std::mutex> l(settings_mutex);
            save_server_json(shared_path, d);
            shared = d;
            json(res, 200, {{"shared", !shared.empty()}, {"defaults", shared}});
        });
        http.Get("/config", [&](const httplib::Request &req, httplib::Response &res) {
            if (!protect(req, res))
                return;
            std::lock_guard<std::mutex> l(config_mutex);
            json(res, 200, server_config_view(read_text_json(utf8_path(config)), utf8_path(config)));
        });
        http.Post("/config", [&](const httplib::Request &req, httplib::Response &res) {
            if (!protect(req, res))
                return;
            if (!req.get_header_value("Content-Type").starts_with("application/json"))
                throw ApiError(415, "send application/json");
            const auto body = parse_text_json(req.body);
            if (!body.is_object() || !body.contains("set"))
                throw ApiError(400, "body must contain set");
            std::lock_guard<std::mutex> l(config_mutex);
            const auto original = read_text_json(utf8_path(config));
            const auto edited = server_config_edit(original, body["set"]);
            auto backup = utf8_path(config);
            backup += L".bak";
            save_server_json(backup, original);
            save_server_json(utf8_path(config), edited);
            json(res, 200, server_config_view(edited, utf8_path(config)));
        });
        for (const auto path : {"/load", "/unload", "/v1/load", "/v1/unload"})
            // Register engine controls before the catch-all generation error route.
            http.Post(path, [&, path](const httplib::Request &req, httplib::Response &res) {
                if (protect(req, res)) {
                    if (!req.get_header_value("Content-Type").starts_with("application/json"))
                        throw ApiError(415, "send application/json");
                    if (req.body.size() > 65536)
                        throw ApiError(413, "control body exceeds 64 KiB");
                    const auto body = req.body.empty() ? J::object() : parse_text_json(req.body);
                    if (!body.is_object())
                        throw ApiError(400, "control body must be an object");
                    if (body.contains("model") && !body["model"].is_null() && body["model"] != model)
                        throw ApiError(404, "model not found");
                    auto result = engine.control(std::string(path).ends_with("unload") ? "unload" : "load",
                                                 req.is_connection_closed);
                    if (std::string(path).starts_with("/v1/"))
                        result.update(status_view());
                    json(res, 200, result);
                }
            });
        http.Post("/v1/vram", [&](const httplib::Request &req, httplib::Response &res) {
            if (!protect(req, res))
                return;
            if (!req.get_header_value("Content-Type").starts_with("application/json"))
                throw ApiError(415, "send application/json");
            const auto body = parse_text_json(req.body);
            if (!body.is_object())
                throw ApiError(400, "body must be an object");
            json(res, 200, engine.vram(body.value("reserve_mib", J(nullptr)), req.is_connection_closed));
        });
        std::filesystem::path slot_directory;
        if (cfg.contains("slot_save_path")) {
            const auto directory = cfg["slot_save_path"].get<std::string>();
            if (directory.empty() || directory.find_first_of("\r\n") != std::string::npos)
                throw std::invalid_argument("invalid slot_save_path");
            slot_directory = utf8_path(directory);
            if (slot_directory.is_relative())
                slot_directory = utf8_path(cfg.value("cwd", ".")) / slot_directory;
            slot_directory = std::filesystem::absolute(slot_directory).lexically_normal();
            std::filesystem::create_directories(slot_directory);
        }
        http.Post("/slots/([^/]+)", [&](const httplib::Request &req, httplib::Response &res) {
            if (!protect(req, res))
                return;
            try {
                if (!req.get_header_value("Content-Type").starts_with("application/json"))
                    throw ApiError(415, "send application/json");
                if (slot_directory.empty())
                    throw ApiError(
                        501, "slot save/restore is disabled (start the server with --slot-save-path DIR)",
                        "server_error");
                if (engine.parallel() > 0)
                    throw ApiError(
                        501,
                        "slot save/restore is not available with parallel requests (\"parallel\" / --batch)",
                        "server_error");
                if (req.matches[1] != "0")
                    throw ApiError(400, "this server has one slot: 0");
                const auto action = req.get_param_value("action");
                if (action != "save" && action != "restore")
                    throw ApiError(400, "action must be save or restore");
                const auto body = parse_text_json(req.body);
                const auto filename = session_filename(body.value("filename", J(nullptr)));
                const auto path = slot_directory / utf8_path(filename);
                if (action == "restore" && !std::filesystem::exists(std::filesystem::symlink_status(path)))
                    throw ApiError(404, "no saved session named " + filename);
                const auto result = engine.session_file(action, path, req.is_connection_closed);
                json(res, 200,
                     {{"id_slot", 0},
                      {"filename", filename},
                      {action == "save" ? "n_saved" : "n_restored", result["tokens"]},
                      {action == "save" ? "n_written" : "n_read", result["bytes"]},
                      {"timings", {{action == "save" ? "save_ms" : "restore_ms", result["ms"]}}}});
            } catch (const ApiError &e) {
                auto error = e.body()["error"];
                error.erase("param");
                error["code"] = e.status;
                json(res, e.status, {{"error", error}});
            } catch (const std::invalid_argument &e) {
                json(res, 400,
                     {{"error", {{"code", 400}, {"type", "invalid_request_error"}, {"message", e.what()}}}});
            }
        });
        auto check_image_origin = [&](const httplib::Request &req, const TemplateRequest &prompt) {
            if (security.foreign_origin(req.get_header_value("Origin"), req.get_header_value("Host")))
                for (const auto &source : images_of(prompt.messages))
                    if (!source.starts_with("data:") && !source.starts_with("http://") &&
                        !source.starts_with("https://"))
                        throw ApiError(400, "a foreign web page cannot read local image files");
        };
        auto generate = [&](const httplib::Request &req, httplib::Response &res) {
            if (!protect(req, res))
                return;
            const auto dialect = req.path == "/v1/responses"            ? ApiDialect::responses
                                 : req.path.starts_with("/v1/messages") ? ApiDialect::anthropic
                                                                        : ApiDialect::chat;
            auto body = parse_text_json(req.body);
            if (!body.is_object())
                throw ApiError(400, "request must be a JSON object");
            if (body.is_object())
                body.erase("_strata_effort_end");
            if (body.is_object() && effort_end)
                body["_strata_effort_end"] = true;
            const auto given_model = body.value("model", J(nullptr));
            const auto requested =
                given_model.is_string() && std::find(models.begin(), models.end(),
                                                     given_model.get<std::string>()) != models.end()
                    ? given_model.get<std::string>()
                    : model;
            if (req.path == "/v1/messages/count_tokens") {
                const auto shared = defaults();
                for (auto it = shared.begin(); it != shared.end(); ++it)
                    if (!body.contains(it.key()) || body[it.key()].is_null())
                        body[it.key()] = it.value();
                const auto p =
                    anthropic_to_messages(body, cfg.value("anthropic_thinking", "model") == "model");
                const auto r = chat.render_prompt(p, tokenizer.controls());
                stop_strings(body);
                json(res, 200, {{"input_tokens", effort_prompt(p, tokenizer, chat, effort_end).size()}});
                return;
            }
            const auto request_id = api_id("request");
            const auto input = body.dump(2);
            const auto request_started = std::chrono::steady_clock::now();
            J record = cfg.value("api_monitor", false)
                           ? J{{"id", request_id},
                               {"path", req.path},
                               {"model", requested},
                               {"input", capture_text(input)},
                               {"input_truncated", capture_text(input).size() != input.size()},
                               {"output", ""},
                               {"reasoning", ""},
                               {"output_truncated", false},
                               {"reasoning_truncated", false},
                               {"state", "queued"},
                               {"started_at", std::time(nullptr)},
                               {"stream", body.value("stream", J(false)) == true},
                               {"response_format", body.value("response_format", J(nullptr)).is_object()
                                                       ? body["response_format"].value("type", J(nullptr))
                                                       : J(nullptr)},
                               {"queue_s", 0.},
                               {"load_s", 0.},
                               {"first_token_s", nullptr},
                               {"http_status", 200}}
                           : J(nullptr);
            history.update(record);
            auto preparation_failed = [&](const ApiError &error) {
                if (record.is_null())
                    return;
                std::lock_guard<std::mutex> lock(history.mutex);
                for (auto &retained : history.records)
                    if (retained["id"] == request_id && retained["state"] == "queued") {
                        retained["state"] = "error";
                        retained["error"] = error.body()["error"];
                        retained["http_status"] = error.status;
                        retained["wallclock_s"] =
                            std::chrono::duration<double>(std::chrono::steady_clock::now() - request_started)
                                .count();
                        const auto response = error.body().dump();
                        retained["response"] = capture_text(response);
                        retained["response_truncated"] = capture_text(response).size() != response.size();
                    }
            };
            try {
                std::set<std::string> mcp_names;
                if (dialect == ApiDialect::chat && body.value("strata_mcp", false)) {
                    security.own_page(req.get_header_value("Origin"), req.get_header_value("Host"),
                                      req.get_header_value("Content-Type"));
                    mcp.wait(10);
                    std::set<std::string> own;
                    for (const auto &tool : body.value("tools", J::array()))
                        if (tool.is_object()) {
                            const auto fn = tool.value("function", tool);
                            if (fn.value("name", J(nullptr)).is_string())
                                own.insert(fn["name"].get<std::string>());
                        }
                    auto extra = mcp.tools(own);
                    if (!extra.empty()) {
                        if (body.value("tool_choice", J(nullptr)) == "none") {
                            body["tools"] = J::array();
                            body["tool_choice"] = "auto";
                        }
                        if (!body.contains("tools") || body["tools"].is_null())
                            body["tools"] = J::array();
                        for (const auto &fn : extra) {
                            mcp_names.insert(fn["name"].get<std::string>());
                            body["tools"].push_back({{"type", "function"}, {"function", fn}});
                        }
                    }
                }
                auto prepared =
                    prepare_api_request(dialect, body, defaults(), tokenizer, chat, engine.context(),
                                        cfg.value("fit_max_tokens", false),
                                        cfg.value("anthropic_thinking", "model") == "model", true);
                const auto metadata = codex_metadata(body);
                if (dialect == ApiDialect::responses && cfg.value("codex_thread_titles", false) &&
                    metadata.value("thread_source", "") == "thread_title") {
                    prepared.ids.clear();
                    prepared.thinking = false;
                    auto title = python_json(J{{"title", codex_title(body)}});
                    auto retain_title = [&, record, request_started, title](const J &result) mutable {
                        if (record.is_null())
                            return;
                        record["state"] = "completed";
                        record["output"] = capture_text(title);
                        record["usage"] = result["usage"];
                        record["wallclock_s"] =
                            std::chrono::duration<double>(std::chrono::steady_clock::now() - request_started)
                                .count();
                        if (!record["stream"].get<bool>()) {
                            const auto response = result.dump();
                            record["response"] = capture_text(response);
                            record["response_truncated"] = capture_text(response).size() != response.size();
                        }
                        history.update(record);
                    };
                    auto emit = [&](const std::string &name, const J &event) {
                        (void)name;
                        (void)event;
                        return true;
                    };
                    if (!prepared.stream) {
                        ApiOutput out(prepared, requested, emit);
                        out.start();
                        out.event({"content", title, {}});
                        const auto result = out.finish({{"finish", "stop"},
                                                        {"completion_tokens", 0},
                                                        {"prompt_tokens", 0},
                                                        {"reused", 0}},
                                                       {});
                        retain_title(result);
                        json(res, 200, result);
                        return;
                    }
                    res.set_header("Cache-Control", "no-cache");
                    res.set_chunked_content_provider(
                        "text/event-stream",
                        [prepared, requested, title, retain_title](size_t, httplib::DataSink &sink) mutable {
                            ApiOutput out(prepared, requested, [&](const std::string &name, const J &event) {
                                const auto text = "event: " + name + "\ndata: " + event.dump() + "\n\n";
                                return sink.write(text.data(), text.size());
                            });
                            out.start();
                            out.event({"content", title, {}});
                            const auto result = out.finish({{"finish", "stop"},
                                                            {"completion_tokens", 0},
                                                            {"prompt_tokens", 0},
                                                            {"reused", 0}},
                                                           {});
                            retain_title(result);
                            sink.done();
                            return true;
                        });
                    return;
                }
                if (dialect == ApiDialect::responses && cfg.value("codex_compaction_cache", false)) {
                    const auto session = metadata.value("session_id", J(nullptr)),
                               thread = metadata.value("thread_id", J(nullptr));
                    if (session.is_string() && thread.is_string() && session != "" && thread != "") {
                        const auto owner = J::array({session, thread});
                        std::lock_guard<std::mutex> lock(codex_mutex);
                        if (metadata.value("request_kind", "") == "compaction") {
                            if (owner == codex_owner && !codex_tools.is_null() &&
                                (!body.contains("tools") || body["tools"].empty())) {
                                auto rendered = prepared.prompt;
                                rendered.tools = codex_tools;
                                prepared.ids = effort_prompt(rendered, tokenizer, chat, effort_end);
                            }
                        } else {
                            codex_owner = owner;
                            codex_tools = prepared.prompt.tools;
                        }
                    }
                }
                check_image_origin(req, prepared.prompt);
                prepare_vision(prepared, vision.get(), engine, tokenizer, chat, engine.context(),
                               cfg.value("fit_max_tokens", false), req.is_connection_closed);
                if (!mcp_names.empty() && !prepared.force.empty())
                    throw ApiError(400, "a forced tool_choice with MCP tools is not supported");
                auto closed = req.is_connection_closed;
                const auto path = req.path;
                auto execute =
                    [&, prepared, path, requested, mcp_names, record, request_id,
                     closed](const std::function<bool(const std::string &, const J &)> &writer) mutable -> J {
                    std::atomic<bool> disconnected{false};
                    J buffered = J::array();
                    std::string answer;
                    auto last_beat = std::chrono::steady_clock::now();
                    auto emit = [&](const std::string &name, const J &event) {
                        if (prepared.format.enabled() && prepared.dialect != ApiDialect::responses &&
                            !event.contains("error")) {
                            if (event.is_null() || event.contains("prompt_progress")) {
                                last_beat = std::chrono::steady_clock::now();
                                const auto ok = writer(name, event);
                                if (!ok)
                                    disconnected = true;
                                return ok;
                            }
                            buffered.push_back({{"name", name}, {"body", event}});
                            return !closed();
                        }
                        const auto ok = writer(name, event);
                        if (!ok)
                            disconnected = true;
                        return ok;
                    };
                    ApiOutput output(prepared, requested, emit);
                    const auto started = std::chrono::steady_clock::now();
                    auto round_started = started;
                    int generated = 0, reasoning_tokens = 0, repeat = 0, last_id = -1;
                    J completed_work = nullptr, first_token = nullptr;
                    bool round_recorded = false;
                    std::string reasoning_text;
                    std::vector<int> raw_ids;
                    int recoveries = 0, next_loop = 512;
                    double coverage = 0;
                    bool looped = false, close_retried = false, answered = false;
                    bool natural = false, repeated = false, called = false;
                    Utf8Decoder decoder;
                    StopMatcher stops(prepared.stops);
                    auto parser =
                        std::make_unique<OutputParser>(prepared.thinking, prepared.prompt.tools, true,
                                                       cfg.value("tool_call_recovery", false));
                    std::vector<ToolCall> mcp_calls;
                    std::string round_content, round_reasoning;
                    int own_calls = 0;
                    auto deliver = [&](const OutputEvent &e) {
                        if (e.kind == "reasoning" && cfg.value("reasoning_loop_recovery", J(false)) != false)
                            reasoning_text += e.text;
                        if (e.kind == "content" || e.kind == "tool_start" || e.kind == "tool_call")
                            answered = true;
                        if (e.call && mcp_names.count(e.call->name)) {
                            if (e.kind == "tool_start")
                                return output.mcp(
                                    {{"event", "start"}, {"id", e.call->id}, {"name", e.call->name}});
                            if (e.kind == "tool_call")
                                mcp_calls.push_back(*e.call);
                            return true;
                        }
                        if (e.kind == "tool_call")
                            ++own_calls;
                        if (e.kind == "content")
                            round_content += e.text;
                        else if (e.kind == "reasoning")
                            round_reasoning += e.text;
                        if (e.kind == "content")
                            answer += e.text;
                        called = called || e.kind == "tool_start" || e.kind == "tool_call";
                        if (!record.is_null() && (e.kind == "content" || e.kind == "reasoning")) {
                            const auto key = e.kind == "content" ? "output" : "reasoning";
                            auto s = record[key].get<std::string>() + e.text;
                            record[key] = capture_text(s);
                            record[std::string(key) + "_truncated"] =
                                record[std::string(key) + "_truncated"].get<bool>() ||
                                record[key].get_ref<const std::string &>().size() != s.size();
                            history.update(record);
                        }
                        return output.event(e);
                    };
                    auto events = [&](const std::vector<OutputEvent> &evs) {
                        bool ok = true;
                        for (auto e : evs) {
                            if (stops.hit)
                                break;
                            if (e.kind == "content")
                                e.text = stops.push(e.text);
                            else {
                                auto held = stops.flush();
                                if (!held.empty())
                                    ok = deliver({"content", held, {}}) && ok;
                            }
                            ok = deliver(e) && ok;
                        }
                        return ok && !stops.hit;
                    };
                    try {
                        output.start();
                        int mcp_rounds = 0, total_completion = 0;
                        for (;;) {
                            if (!prepared.force.empty() && !prepared.thinking)
                                events(parser->feed(prepared.force));
                            auto cancelled = [&] { return disconnected || closed() || stopping; };
                            auto lease = engine.acquire(cancelled);
                            {
                                std::lock_guard<std::mutex> lock(history.mutex);
                                history.last_at = std::time(nullptr);
                            }
                            if (!record.is_null()) {
                                record["queue_s"] =
                                    std::chrono::duration<double>(std::chrono::steady_clock::now() - started)
                                        .count();
                                history.update(record);
                            }
                            if (!engine.loaded()) {
                                if (!record.is_null()) {
                                    record["state"] = "loading";
                                    history.update(record);
                                }
                                const auto loading = std::chrono::steady_clock::now();
                                try {
                                    engine.ensure_loaded(lease.get(), cancelled);
                                } catch (...) {
                                    if (!record.is_null()) {
                                        record["load_s"] = std::chrono::duration<double>(
                                                               std::chrono::steady_clock::now() - loading)
                                                               .count();
                                        history.update(record);
                                    }
                                    throw;
                                }
                                if (!record.is_null()) {
                                    record["load_s"] = std::chrono::duration<double>(
                                                           std::chrono::steady_clock::now() - loading)
                                                           .count();
                                    record["state"] = "queued";
                                    history.update(record);
                                }
                            }
                            auto prompt = prepared.ids;
                            auto force = prepared.thinking ? prepared.force : std::string();
                            int budget = prepared.thinking ? prepared.reasoning_budget : 0, thought = 0;
                            J done;
                            for (;;) {
                                std::vector<int> segment_ids;
                                std::optional<std::vector<int>> recover_prompt;
                                bool wrap = false, opens = false;
                                auto segment = engine.generate(
                                    prompt, prepared.max_new - generated, prepared.sampling,
                                    [&](int id) {
                                        if (disconnected || closed())
                                            return false;
                                        if (id < 0)
                                            return output.ping(
                                                engine.snapshot().value("progress", J(nullptr)));
                                        ++generated;
                                        if (first_token.is_null())
                                            first_token =
                                                std::chrono::duration<double>(
                                                    std::chrono::steady_clock::now() - round_started)
                                                    .count();
                                        if (!record.is_null() && record["first_token_s"].is_null()) {
                                            record["first_token_s"] =
                                                std::chrono::duration<double>(
                                                    std::chrono::steady_clock::now() - started)
                                                    .count();
                                            record["state"] = "generating";
                                            history.update(record);
                                        }
                                        if (static_cast<size_t>(id) >= tokenizer.size())
                                            throw std::runtime_error(
                                                "engine token outside tokenizer vocabulary");
                                        const auto raw = tokenizer.token_bytes(id);
                                        raw_ids.push_back(id);
                                        if (raw == "<|im_end|>" || raw == "<|endoftext|>") {
                                            natural = true;
                                            return false;
                                        }
                                        segment_ids.push_back(id);
                                        reasoning_tokens +=
                                            parser->state() == "reasoning" || parser->state() == "rcall";
                                        repeat = id == last_id ? repeat + 1 : 1;
                                        last_id = id;
                                        const auto limit = cfg.value("repeat_stop_tokens", 256);
                                        if (limit > 0 && repeat >= limit) {
                                            repeated = true;
                                            return false;
                                        }
                                        const auto ok = events(parser->feed(decoder.push(raw)));
                                        if (!ok)
                                            return false;
                                        if (prepared.format.enabled() &&
                                            prepared.dialect == ApiDialect::chat &&
                                            std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                                                          last_beat)
                                                    .count() >= 1 &&
                                            !output.ping())
                                            return false;
                                        const auto recovery = cfg.value("reasoning_loop_recovery", J(false));
                                        if (recovery != false && recoveries == 0 &&
                                            parser->state() == "reasoning" && generated >= next_loop &&
                                            parser->boundary() && !decoder.pending()) {
                                            coverage = reasoning_repeat_coverage(reasoning_text);
                                            next_loop = generated + 512;
                                            if (coverage >= .25) {
                                                if (recovery == "stop") {
                                                    looped = true;
                                                    return false;
                                                }
                                                if (!prepared.vision) {
                                                    recover_prompt = focused_recovery_prompt(
                                                        tokenizer, prepared.ids, raw_ids);
                                                    if (recover_prompt)
                                                        return false;
                                                }
                                            }
                                        }
                                        if (budget && parser->state() == "reasoning") {
                                            ++thought;
                                            if (thought >= budget && parser->boundary() &&
                                                !decoder.pending()) {
                                                wrap = true;
                                                return false;
                                            }
                                        }
                                        if (!force.empty()) {
                                            if (called)
                                                force.clear();
                                            else if (parser->state() == "content" && parser->boundary() &&
                                                     !decoder.pending()) {
                                                opens = true;
                                                return false;
                                            }
                                        }
                                        return true;
                                    },
                                    cancelled, lease.get(),
                                    prepared.vision ? prepared.vision->engine_path : std::string());
                                if (done.is_null())
                                    done = segment;
                                else {
                                    for (auto field : {"generated", "prompt_ms", "decode_ms",
                                                       "drafts_offered", "drafts_accepted", "hits", "lookups",
                                                       "offloaded", "ram_blobs", "file_blobs", "file_mb"})
                                        if (segment.contains(field))
                                            done[field] =
                                                done.value(field, 0.) + segment[field].get<double>();
                                    done["finish"] = segment["finish"];
                                }
                                completed_work = done;
                                if (recover_prompt && !cancelled() && generated < prepared.max_new) {
                                    ++recoveries;
                                    prepared.sampling["temperature"] =
                                        std::max(1., prepared.sampling.value("temperature", 0.));
                                    prepared.sampling["presence_penalty"] =
                                        std::max(1.5, prepared.sampling.value("presence_penalty", 0.));
                                    prompt = *recover_prompt;
                                    continue;
                                }
                                if (cfg.value("reasoning_close_retry", false) && prepared.thinking &&
                                    natural && !close_retried && !answered && !wrap && !opens &&
                                    parser->state() == "reasoning" && !stops.hit && !cancelled()) {
                                    close_retried = true;
                                    const auto extra = tokenizer.encode("\n</think>\n\n", true);
                                    if (prepared.max_new - generated - static_cast<int>(extra.size()) >= 1) {
                                        for (int id : extra) {
                                            ++generated;
                                            raw_ids.push_back(id);
                                            reasoning_tokens +=
                                                parser->state() == "reasoning" || parser->state() == "rcall";
                                            events(parser->feed(decoder.push(tokenizer.token_bytes(id))));
                                        }
                                        prompt.insert(prompt.end(), segment_ids.begin(), segment_ids.end());
                                        prompt.insert(prompt.end(), extra.begin(), extra.end());
                                        natural = false;
                                        done["finish"] = "length";
                                        continue;
                                    }
                                }
                                if (!(wrap || opens) || cancelled())
                                    break;
                                const auto insertion =
                                    wrap ? std::string("\n\nI have thought about this long enough; "
                                                       "time to give my answer.\n</think>\n\n") +
                                               force
                                         : std::string("\n\n") + force;
                                force.clear();
                                if (wrap)
                                    budget = 0;
                                const auto extra = tokenizer.encode(insertion, true);
                                if (prepared.max_new - generated - static_cast<int>(extra.size()) < 1) {
                                    done["finish"] = "length";
                                    break;
                                }
                                for (int id : extra) {
                                    ++generated;
                                    reasoning_tokens +=
                                        parser->state() == "reasoning" || parser->state() == "rcall";
                                    events(parser->feed(decoder.push(tokenizer.token_bytes(id))));
                                }
                                if (stops.hit)
                                    break;
                                prompt.insert(prompt.end(), segment_ids.begin(), segment_ids.end());
                                prompt.insert(prompt.end(), extra.begin(), extra.end());
                            }
                            if (!stops.hit)
                                events(parser->feed(decoder.push({}, true)));
                            if (stops.hit || natural)
                                done["finish"] = "stop";
                            if (repeated || looped)
                                done["finish"] = "length";
                            events(parser->finish(done.value("finish", "stop")));
                            if (!stops.hit) {
                                auto held = stops.flush();
                                if (!held.empty())
                                    deliver({"content", held, {}});
                            }
                            done["completion_tokens"] = generated;
                            done["reasoning_tokens"] = reasoning_tokens;
                            total_completion += generated;
                            lease.reset();
                            bool run_tools = !mcp_calls.empty() && own_calls == 0 &&
                                             done.value("finish", "") == "stop" && !cancelled();
                            if (run_tools && mcp_rounds >= mcp.max_rounds()) {
                                output.mcp({{"event", "limit"}, {"max_rounds", mcp.max_rounds()}});
                                run_tools = false;
                            }
                            if (run_tools) {
                                const auto elapsed = std::chrono::duration<double>(
                                                         std::chrono::steady_clock::now() - round_started)
                                                         .count();
                                latencies.observe(first_token, generated, done.value("decode_ms", 0.) / 1000.,
                                                  elapsed);
                                auto row = done;
                                row.update(
                                    J{{"id", request_id},
                                      {"path", path},
                                      {"model", requested},
                                      {"time", std::time(nullptr)},
                                      {"duration_s", elapsed},
                                      {"output_tokens", generated},
                                      {"timings", request_timings(static_cast<int>(prepared.ids.size()),
                                                                  generated, done)}});
                                history.add(row, nullptr);
                                completed_work = nullptr;
                                round_recorded = true;
                                ++mcp_rounds;
                                auto messages = prepared.prompt.messages;
                                J calls = J::array(), results = J::array();
                                for (const auto &call : mcp_calls) {
                                    J server = nullptr;
                                    std::string tool = call.name;
                                    const auto state = mcp.status();
                                    for (const auto &s : state["servers"])
                                        for (const auto &t : s["tools"])
                                            if (t["name"] == call.name) {
                                                server = s["name"];
                                                tool = t["tool"].get<std::string>();
                                            }
                                    if (!output.mcp({{"event", "call"},
                                                     {"id", call.id},
                                                     {"name", call.name},
                                                     {"server", server},
                                                     {"tool", tool},
                                                     {"arguments", call.arguments},
                                                     {"round", mcp_rounds}}))
                                        disconnected = true;
                                    auto work = std::async(std::launch::async, [&, call] {
                                        return mcp.call(call.name, call.arguments, cancelled);
                                    });
                                    while (work.wait_for(std::chrono::seconds(1)) !=
                                           std::future_status::ready)
                                        if (!output.ping())
                                            disconnected = true;
                                    auto result = work.get();
                                    auto activity = result;
                                    activity["event"] = "result";
                                    activity["id"] = call.id;
                                    output.mcp(activity);
                                    calls.push_back(
                                        {{"function", {{"name", call.name}, {"arguments", call.arguments}}}});
                                    results.push_back({{"role", "tool"}, {"content", result["text"]}});
                                }
                                if (cancelled()) {
                                    done["finish"] = "cancel";
                                } else {
                                    J assistant{{"role", "assistant"},
                                                {"content", trim_text(round_content)},
                                                {"tool_calls", calls}};
                                    if (!round_reasoning.empty())
                                        assistant["reasoning_content"] = trim_text(round_reasoning);
                                    messages.push_back(assistant);
                                    for (const auto &result : results)
                                        messages.push_back(result);
                                    for (auto &message : messages)
                                        if (message.value("content", J(nullptr)).is_array())
                                            for (auto &part : message["content"])
                                                if (part.is_object() && part.value("type", "") == "image")
                                                    part = {{"type", "image_url"},
                                                            {"image_url", {{"url", part["source"]}}}};
                                    auto next = prepared.wire;
                                    next["messages"] = messages;
                                    prepared = prepare_api_request(
                                        ApiDialect::chat, next, defaults(), tokenizer, chat, engine.context(),
                                        cfg.value("fit_max_tokens", false),
                                        cfg.value("anthropic_thinking", "model") == "model", true);
                                    prepare_vision(prepared, vision.get(), engine, tokenizer, chat,
                                                   engine.context(), cfg.value("fit_max_tokens", false),
                                                   cancelled);
                                    parser = std::make_unique<OutputParser>(
                                        prepared.thinking, prepared.prompt.tools, true,
                                        cfg.value("tool_call_recovery", false));
                                    decoder = Utf8Decoder();
                                    stops = StopMatcher(prepared.stops);
                                    generated = reasoning_tokens = repeat = 0;
                                    last_id = -1;
                                    natural = repeated = called = looped = close_retried = answered = false;
                                    mcp_calls.clear();
                                    round_content.clear();
                                    round_reasoning.clear();
                                    reasoning_text.clear();
                                    raw_ids.clear();
                                    recoveries = 0;
                                    next_loop = 512;
                                    own_calls = 0;
                                    first_token = nullptr;
                                    round_recorded = false;
                                    round_started = std::chrono::steady_clock::now();
                                    continue;
                                }
                            } else
                                for (const auto &call : mcp_calls)
                                    output.mcp({{"event", "result"},
                                                {"id", call.id},
                                                {"ok", false},
                                                {"skipped", true},
                                                {"text", "not run"},
                                                {"chars", 0},
                                                {"truncated", false},
                                                {"ms", 0}});
                            done["completion_tokens"] = total_completion;
                            if (prepared.format.enabled() &&
                                !(prepared.dialect == ApiDialect::responses && called)) {
                                try {
                                    const auto canonical =
                                        prepared.format.validate(answer, done.value("finish", "stop"));
                                    output.canonical_content(canonical);
                                } catch (const std::exception &e) {
                                    throw ApiError(502, e.what(), "structured_output_failed", {},
                                                   "structured_output_failed");
                                }
                            }
                            auto result = output.finish(done, stops.hit);
                            if (prepared.format.enabled() && prepared.dialect == ApiDialect::chat &&
                                !buffered.empty()) {
                                const auto first = buffered.front();
                                auto middle = first["body"];
                                middle["choices"][0]["delta"] = {
                                    {"content", result["choices"][0]["message"]["content"]}};
                                const auto reasoning =
                                    result["choices"][0]["message"].value("reasoning_content", J(nullptr));
                                if (!reasoning.is_null())
                                    middle["choices"][0]["delta"]["reasoning_content"] = reasoning;
                                if (!writer(first["name"].get<std::string>(), first["body"]) ||
                                    !writer(first["name"].get<std::string>(), middle) ||
                                    !writer(buffered.back()["name"].get<std::string>(),
                                            buffered.back()["body"]))
                                    disconnected = true;
                            }
                            const auto duration =
                                std::chrono::duration<double>(std::chrono::steady_clock::now() - started)
                                    .count();
                            if (!round_recorded)
                                latencies.observe(first_token, generated, done.value("decode_ms", 0.) / 1000.,
                                                  std::chrono::duration<double>(
                                                      std::chrono::steady_clock::now() - round_started)
                                                      .count());
                            if (!record.is_null()) {
                                record["usage"] = result.value("usage", J(nullptr));
                                record["state"] = disconnected || closed() ? "disconnected" : "completed";
                                record["wallclock_s"] = duration;
                                record["timings"] = result.value("timings", J(nullptr));
                                if (!prepared.stream) {
                                    const auto bytes = result.dump();
                                    record["response"] = capture_text(bytes);
                                    record["response_truncated"] =
                                        record["response"].get_ref<const std::string &>().size() !=
                                        bytes.size();
                                }
                            }
                            if (!round_recorded)
                                history.add(
                                    {{"id", request_id},
                                     {"path", path},
                                     {"model", requested},
                                     {"time", std::time(nullptr)},
                                     {"prompt_tokens", prepared.ids.size()},
                                     {"output_tokens", generated},
                                     {"finish", done["finish"]},
                                     {"usage", result.value("usage", J::object())},
                                     {"duration_s", duration},
                                     {"prompt_ms", done.value("prompt_ms", 0.)},
                                     {"decode_ms", done.value("decode_ms", 0.)},
                                     {"reused", done.value("reused", 0)},
                                     {"timings", request_timings(static_cast<int>(prepared.ids.size()),
                                                                 generated, done)},
                                     {"decode_tok_s", done.value("decode_ms", 0.) > 0
                                                          ? J(1000. * done.value("generated", generated) /
                                                              done["decode_ms"].get<double>())
                                                          : J(nullptr)},
                                     {"drafts_offered", done.value("drafts_offered", 0)},
                                     {"drafts_accepted", done.value("drafts_accepted", 0)}},
                                    record);
                            else
                                history.update(record);
                            return result;
                        }
                    } catch (const ApiError &e) {
                        if (!record.is_null()) {
                            record["state"] = "error";
                            record["error"] = e.body()["error"];
                            record["http_status"] = prepared.stream ? 200 : e.status;
                            record["wallclock_s"] =
                                std::chrono::duration<double>(std::chrono::steady_clock::now() - started)
                                    .count();
                        }
                        J failed = completed_work.is_null() ? J::object() : completed_work;
                        failed.update(J{{"id", request_id},
                                        {"path", path},
                                        {"error", e.body()},
                                        {"finish", "error"},
                                        {"output_tokens", generated},
                                        {"prompt_tokens", prepared.ids.size()}});
                        if (!round_recorded) {
                            history.add(failed, record);
                            latencies.observe(first_token, generated, failed.value("decode_ms", 0.) / 1000.,
                                              std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                                                            round_started)
                                                  .count());
                        } else
                            history.update(record);
                        if (prepared.stream) {
                            output.failed(e);
                            return nullptr;
                        }
                        throw;
                    } catch (const std::exception &) {
                        ApiError e(500, "native generation failed", "server_error");
                        if (!record.is_null()) {
                            record["state"] = "error";
                            record["error"] = e.body()["error"];
                            record["http_status"] = prepared.stream ? 200 : e.status;
                            record["wallclock_s"] =
                                std::chrono::duration<double>(std::chrono::steady_clock::now() - started)
                                    .count();
                        }
                        if (!round_recorded) {
                            J failed = completed_work.is_null() ? J::object() : completed_work;
                            failed.update(J{{"id", request_id},
                                            {"path", path},
                                            {"error", e.body()},
                                            {"finish", "error"},
                                            {"output_tokens", generated},
                                            {"prompt_tokens", prepared.ids.size()}});
                            history.add(failed, record);
                            latencies.observe(first_token, generated, failed.value("decode_ms", 0.) / 1000.,
                                              std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                                                            round_started)
                                                  .count());
                        } else
                            history.update(record);
                        if (prepared.stream) {
                            output.failed(e);
                            return nullptr;
                        }
                        throw e;
                    }
                };
                if (!prepared.stream) {
                    json(res, 200, execute([](const std::string &, const J &) { return true; }));
                    return;
                }
                res.set_header("Cache-Control", "no-cache");
                res.set_header("X-Accel-Buffering", "no");
                res.set_chunked_content_provider(
                    "text/event-stream", [execute, dialect](size_t, httplib::DataSink &sink) mutable {
                        auto writer = [&](const std::string &name, const J &event) {
                            if (!sink.is_writable())
                                return false;
                            const auto bytes =
                                event.is_null() ? std::string(": keep-alive\n\n")
                                                : (name.empty() ? std::string() : "event: " + name + "\n") +
                                                      "data: " + event.dump() + "\n\n";
                            return sink.write(bytes.data(), bytes.size());
                        };
                        execute(writer);
                        if (dialect == ApiDialect::chat) {
                            const std::string end = "data: [DONE]\n\n";
                            sink.write(end.data(), end.size());
                        }
                        sink.done();
                        return true;
                    });
            } catch (const ApiError &e) {
                preparation_failed(e);
                throw;
            } catch (const J::exception &) {
                preparation_failed(ApiError(400, "invalid JSON field type"));
                throw;
            } catch (const std::invalid_argument &e) {
                preparation_failed(ApiError(400, e.what()));
                throw;
            } catch (...) {
                preparation_failed(ApiError(500, "native server operation failed", "server_error"));
                throw;
            }
        };
        http.Post("/v1/chat/completions/?", generate);
        http.Post("/v1/messages/?", generate);
        http.Post("/v1/responses/?", generate);
        http.Post("/v1/messages/count_tokens/?", generate);
        http.Post("/v1/responses/.*", [&](const httplib::Request &req, httplib::Response &res) {
            if (protect(req, res))
                json(res, 404,
                     ApiError(
                         404,
                         "this server keeps no responses; send the whole conversation to POST /v1/responses",
                         "invalid_request_error", {}, "not_found")
                         .body());
        });
        http.Get("/v1/responses/.*", [&](const httplib::Request &req, httplib::Response &res) {
            if (protect(req, res))
                json(res, 404,
                     ApiError(404, "this server keeps no responses", "invalid_request_error", {}, "not_found")
                         .body());
        });
        http.Delete("/v1/responses/.*", [&](const httplib::Request &req, httplib::Response &res) {
            if (protect(req, res))
                json(res, 404,
                     ApiError(404, "this server keeps no responses", "invalid_request_error", {}, "not_found")
                         .body());
        });
        http.Post(".*", [&](const httplib::Request &req, httplib::Response &res) {
            if (protect(req, res))
                json(res, 404, ApiError(404, "not found", "invalid_request_error", {}, "not_found").body());
        });
#ifdef _WIN32
        SetConsoleCtrlHandler(signal_handler, TRUE);
#else
        signal(SIGINT, signal_handler);
        signal(SIGTERM, signal_handler);
        signal(SIGPIPE, SIG_IGN);
#endif
        std::thread shutdown([&] {
            while (!stopping)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            engine.shutdown();
            http.stop();
        });
        struct JoinShutdown {
            std::thread &worker;
            ~JoinShutdown() {
                stopping = true;
                if (worker.joinable())
                    worker.join();
            }
        } join_shutdown{shutdown};
        std::cout << "ready: http://" << host << ':' << port << "/v1 (native server, context "
                  << engine.context() << ")" << std::endl;
        if (open_requested && cfg.value("open_browser", true)) {
            const auto here = host == "0.0.0.0" || host == "::" ? std::string("127.0.0.1") : host;
            const auto url = "http://" + (here.find(':') != std::string::npos ? "[" + here + "]" : here) +
                             ":" + std::to_string(port) + "/";
#ifdef _WIN32
            ShellExecuteW(nullptr, L"open", utf8_path(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
            const auto child = fork();
            if (child == 0) {
                execlp("xdg-open", "xdg-open", url.c_str(), static_cast<char *>(nullptr));
                _exit(127);
            }
            if (child > 0)
                std::thread([child] { waitpid(child, nullptr, 0); }).detach();
#endif
        }
        const auto ok = http.listen_after_bind();
        stopping = true;
        shutdown.join();
        return ok ? 0 : 2;
    } catch (const J::exception &) {
        std::cerr << "server startup failed: invalid JSON configuration or field type\n";
        return 2;
    } catch (const std::exception &e) {
        std::cerr << "server startup failed: " << e.what() << '\n';
        return 2;
    }
}
} // namespace
int strata::runtime::server_main(const std::vector<std::string>& args) { return run(args); }
