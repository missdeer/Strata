#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <csignal>
#endif
#include "httplib.h"
#include "strata/runtime/path.hpp"
#include "strata/runtime/server_api.hpp"
#include "strata/runtime/server_state.hpp"
#include "strata/runtime/server_telemetry.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <iostream>
#include <set>
#include <thread>

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
        for (const auto &key : value) {
            if (!key.is_string() || key.get<std::string>().empty())
                throw std::invalid_argument("api_key must contain nonempty strings");
            out.push_back(key.get<std::string>());
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
    return out;
}
bool matches(const std::string &a, const std::string &b) {
    size_t diff = a.size() ^ b.size();
    for (size_t i = 0; i < std::max(a.size(), b.size()); ++i)
        diff |= (i < a.size() ? static_cast<unsigned char>(a[i]) : 0) ^
                (i < b.size() ? static_cast<unsigned char>(b[i]) : 0);
    return diff == 0;
}
bool authorized(const httplib::Request &req, const std::vector<std::string> &allowed) {
    if (allowed.empty())
        return true;
    auto given = req.get_header_value("Authorization");
    if (given.starts_with("Bearer "))
        given = given.substr(7);
    else
        given = req.get_header_value("x-api-key");
    bool found = false;
    for (const auto &key : allowed)
        found = matches(given, key) || found;
    return found;
}
bool loopback(const std::string &host) { return host == "127.0.0.1" || host == "localhost" || host == "::1"; }
std::filesystem::path web_path(const J &cfg) {
    if (cfg.contains("web_root"))
        return std::filesystem::absolute(utf8_path(cfg["web_root"].get<std::string>()));
    return std::filesystem::absolute("serve/web");
}
struct History {
    std::mutex mutex;
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
        totals["requests"] = totals["requests"].get<int>() + 1;
        if (row.contains("usage")) {
            const auto &u = row["usage"];
            totals["prompt_tokens"] =
                totals["prompt_tokens"].get<int64_t>() + u.value("prompt_tokens", u.value("input_tokens", 0));
            totals["output_tokens"] = totals["output_tokens"].get<int64_t>() +
                                      u.value("completion_tokens", u.value("output_tokens", 0));
            totals["reused"] = totals["reused"].get<int64_t>() +
                               u.value("prompt_tokens_details", u.value("input_tokens_details", J::object()))
                                   .value("cached_tokens", u.value("cache_read_input_tokens", 0));
        }
        for (auto field : {"prompt_ms", "decode_ms", "drafts_offered", "drafts_accepted"})
            totals[field] = totals[field].get<double>() + row.value(field, 0.);
        requests.insert(requests.begin(), row);
        if (requests.size() > 100)
            requests.erase(requests.end() - 1);
        upsert_unlocked(record);
    }
};
int run(const std::vector<std::string> &argv) {
    try {
        std::string config, host_override, key_override, web_override;
        int port_override = 0;
        bool validate = false;
        std::set<std::string> seen;
        for (size_t i = 1; i < argv.size(); ++i) {
            const auto &arg = argv[i];
            if (!seen.insert(arg).second)
                throw std::invalid_argument("duplicate option: " + arg);
            if (arg == "--help") {
                std::cout << "strata-server --engine strata --config server.json [--host IP] [--port N] "
                             "[--api-key KEY] [--web-root DIR] [--validate]\n";
                return 0;
            }
            if (arg == "--validate") {
                validate = true;
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
            else if (arg != "--engine" || value != "strata")
                throw std::invalid_argument("unsupported option: " + arg);
        }
        if (config.empty())
            throw std::invalid_argument("--config is required");
        auto cfg = read_text_json(utf8_path(config));
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
        http.set_payload_max_length(256u << 20);
        http.set_read_timeout(30, 0);
        http.set_write_timeout(30, 0);
        http.set_keep_alive_max_count(20);
        http.new_task_queue = [] { return new httplib::ThreadPool(16, 16, 128); };
        if (!http.bind_to_port(host, port))
            throw std::runtime_error("HTTP port unavailable");
        apply_engine_environment(cfg);
        ServerEngine engine(cfg);
        const auto gpu = cfg.value("gpu", J(0));
        ServerTelemetry telemetry(gpu.is_string() ? std::stoi(gpu.get<std::string>()) : gpu.get<int>(),
                                  cfg.value("backend", "") == "hip", [&] {
                                      const auto s = engine.snapshot();
                                      return J{{"tok_s", s.value("tok_s", J(nullptr))}};
                                  });
        History history;
        std::mutex settings_mutex, config_mutex;
        J shared = J::object();
        auto shared_path = std::filesystem::absolute(utf8_path(config));
        shared_path.replace_extension("");
        shared_path += L".shared-settings.json";
        if (std::filesystem::exists(shared_path))
            shared = shared_defaults(read_text_json(shared_path));
        const auto model = cfg.value("model_name", "qwen3.8-flash-next");
        std::vector<std::string> models{model};
        for (const auto &alias : cfg.value("aliases", J::array()))
            models.push_back(alias.get<std::string>());
        auto defaults = [&] {
            std::lock_guard<std::mutex> l(settings_mutex);
            auto d = cfg.value("sampling", J::object());
            if (cfg.contains("reasoning_budget_tokens"))
                d["reasoning_budget_tokens"] = cfg["reasoning_budget_tokens"];
            d.update(shared);
            return d;
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
            auto authority = req.get_header_value("Host");
            const auto host_name = authority.starts_with("[") ? authority.substr(1, authority.find(']') - 1)
                                                              : authority.substr(0, authority.find(':'));
            if (host_name.empty() || (loopback(host) && !loopback(host_name))) {
                json(res, 403, ApiError(403, "Host is not allowed").body());
                return httplib::Server::HandlerResponse::Handled;
            }
            const auto origin = req.get_header_value("Origin");
            if (!origin.empty() && origin != "http://" + authority && origin != "https://" + authority) {
                json(res, 403, ApiError(403, "foreign Origin is not allowed").body());
                return httplib::Server::HandlerResponse::Handled;
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
                  {"images", false},
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
            for (const auto &name : models) {
                J m{{"id", name},
                    {"object", "model"},
                    {"status", {{"value", engine.loaded() ? "loaded" : "unloaded"}}},
                    {"meta", {{"n_ctx", engine.context()}}},
                    {"architecture",
                     {{"input_modalities", J::array({"text"})}, {"output_modalities", J::array({"text"})}}}};
                if (name != model)
                    m["alias_of"] = model;
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
                json(res, 200, {{"servers", J::array()}, {"tools", 0}});
        });
        http.Get("/props", [&](const httplib::Request &req, httplib::Response &res) {
            if (!protect(req, res))
                return;
            auto params = defaults();
            params["n_predict"] = params.value("max_tokens", -1);
            json(res, 200,
                 {{"default_generation_settings", {{"n_ctx", engine.context()}, {"params", params}}},
                  {"total_slots", 1},
                  {"model_alias", model},
                  {"chat_template", read_text(utf8_path(template_path))},
                  {"chat_template_caps", chat.capabilities()},
                  {"modalities", {{"vision", false}}},
                  {"models_autoload", true},
                  {"is_sleeping", !engine.loaded()}});
        });
        http.Get("/v1/status", [&](const httplib::Request &req, httplib::Response &res) {
            if (!protect(req, res))
                return;
            auto s = engine.snapshot();
            json(res, 200,
                 {{"service", "strata"},
                  {"model", model},
                  {"loaded", engine.loaded()},
                  {"auto_load", true},
                  {"cache_max_tokens", engine.context()},
                  {"context", {{"native", engine.context()}, {"max_positions", engine.context()}}},
                  {"concurrency", {{"serving", 1}, {"requested", 1}}},
                  {"dialects", J::array({"/v1/chat/completions", "/v1/messages", "/v1/responses"})},
                  {"vision", {{"enabled", false}, {"available", false}, {"error", nullptr}}},
                  {"structured_output",
                   {{"formats", J::array({"json_object", "json_schema"})},
                    {"method", "prompt_and_validate"},
                    {"constrained_decoding", false},
                    {"stream_buffered", true}}},
                  {"activity", {{"in_flight", s.value("queued", 0) + (s.value("busy", false) ? 1 : 0)}}},
                  {"last_timings", s.value("last", J(nullptr))}});
        });
        http.Get("/metrics", [&](const httplib::Request &req, httplib::Response &res) {
            if (!protect(req, res))
                return;
            const auto s = engine.snapshot();
            std::lock_guard<std::mutex> l(history.mutex);
            if (req.get_header_value("Accept").find("text/plain") != std::string::npos ||
                req.get_param_value("format") == "prometheus") {
                std::ostringstream out;
                out << "# TYPE vllm:num_requests_running gauge\nvllm:num_requests_running "
                    << (s.value("busy", false) ? 1 : 0)
                    << "\n# TYPE vllm:num_requests_waiting gauge\nvllm:num_requests_waiting "
                    << s.value("queued", 0)
                    << "\n# TYPE vllm:prompt_tokens_total counter\nvllm:prompt_tokens_total "
                    << history.totals["prompt_tokens"]
                    << "\n# TYPE vllm:generation_tokens_total counter\nvllm:generation_tokens_total "
                    << history.totals["output_tokens"] << '\n';
                res.set_content(out.str(), "text/plain; version=0.0.4; charset=utf-8");
                return;
            }
            J rows = history.requests;
            if (req.get_param_value("requests") != "all" && rows.size() > 12)
                rows.erase(rows.begin() + 12, rows.end());
            auto info = s.value("info", J::object());
            info.update(
                J{{"model", model}, {"max_context", engine.context()}, {"images", false}, {"native", true}});
            const auto tel = telemetry.snapshot();
            json(res, 200,
                 {{"engine", info},
                  {"live",
                   {{"state", s.value("busy", false) ? s.value("generated", 0) > 0 ? "generating" : "reading"
                                                     : "idle"},
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
                  {"time", std::time(nullptr)}});
        });
        if (cfg.value("api_monitor", false))
            http.Get("/api/requests", [&](const httplib::Request &req, httplib::Response &res) {
                if (!protect(req, res))
                    return;
                std::lock_guard<std::mutex> l(history.mutex);
                const auto id = req.get_param_value("id");
                if (id.empty()) {
                    json(res, 200,
                         {{"requests", history.records},
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
                    json(res, 200,
                         engine.control(std::string(path).ends_with("unload") ? "unload" : "load",
                                        req.is_connection_closed));
                }
            });
        auto generate = [&](const httplib::Request &req, httplib::Response &res) {
            if (!protect(req, res))
                return;
            const auto dialect = req.path == "/v1/responses"            ? ApiDialect::responses
                                 : req.path.starts_with("/v1/messages") ? ApiDialect::anthropic
                                                                        : ApiDialect::chat;
            auto body = parse_text_json(req.body);
            const auto requested = body.value("model", model);
            if (std::find(models.begin(), models.end(), requested) == models.end())
                throw ApiError(404, "model not found", "invalid_request_error", "model", "model_not_found");
            if (req.path == "/v1/messages/count_tokens") {
                const auto p =
                    anthropic_to_messages(body, cfg.value("anthropic_thinking", "model") == "model");
                if (!images_of(p.messages).empty())
                    throw ApiError(400, "native server is text-only");
                const auto r = chat.render_prompt(p, tokenizer.controls());
                json(res, 200, {{"input_tokens", tokenizer.encode(r.text, true, r.plain).size()}});
                return;
            }
            auto prepared = prepare_api_request(dialect, body, defaults(), tokenizer, chat, engine.context(),
                                                cfg.value("fit_max_tokens", false),
                                                cfg.value("anthropic_thinking", "model") == "model");
            auto closed = req.is_connection_closed;
            const auto path = req.path;
            auto execute = [&, prepared, path, requested,
                            closed](const std::function<bool(const std::string &, const J &)> &writer) -> J {
                std::atomic<bool> disconnected{false};
                const auto request_id = api_id("request");
                const auto input = prepared.wire.dump(2);
                J record = cfg.value("api_monitor", false) ? J{{"id", request_id},
                                                               {"path", path},
                                                               {"model", requested},
                                                               {"input", input.substr(0, 262144)},
                                                               {"input_truncated", input.size() > 262144},
                                                               {"output", ""},
                                                               {"reasoning", ""},
                                                               {"output_truncated", false},
                                                               {"reasoning_truncated", false},
                                                               {"state", "queued"},
                                                               {"started_at", std::time(nullptr)},
                                                               {"stream", prepared.stream},
                                                               {"queue_s", 0.},
                                                               {"load_s", 0.},
                                                               {"first_token_s", nullptr},
                                                               {"http_status", 200}}
                                                           : J(nullptr);
                history.update(record);
                J buffered = J::array();
                std::string answer;
                auto emit = [&](const std::string &name, const J &event) {
                    if (prepared.format.enabled()) {
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
                int generated = 0, reasoning_tokens = 0, repeat = 0, last_id = -1;
                bool natural = false, repeated = false, called = false;
                Utf8Decoder decoder;
                StopMatcher stops(prepared.stops);
                OutputParser parser(prepared.thinking, prepared.prompt.tools, true,
                                    cfg.value("tool_call_recovery", false));
                auto deliver = [&](const OutputEvent &e) {
                    if (e.kind == "content")
                        answer += e.text;
                    called = called || e.kind == "tool_start" || e.kind == "tool_call";
                    if (!record.is_null() && (e.kind == "content" || e.kind == "reasoning")) {
                        const auto key = e.kind == "content" ? "output" : "reasoning";
                        auto s = record[key].get<std::string>() + e.text;
                        record[key] = s.substr(0, 262144);
                        record[std::string(key) + "_truncated"] =
                            record[std::string(key) + "_truncated"].get<bool>() || s.size() > 262144;
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
                    if (!prepared.force.empty() && !prepared.thinking)
                        events(parser.feed(prepared.force));
                    auto cancelled = [&] { return disconnected || closed() || stopping; };
                    auto lease = engine.acquire(cancelled);
                    if (!record.is_null()) {
                        record["queue_s"] =
                            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
                        history.update(record);
                    }
                    auto prompt = prepared.ids;
                    auto force = prepared.thinking ? prepared.force : std::string();
                    int budget = prepared.thinking ? prepared.reasoning_budget : 0, thought = 0;
                    J done;
                    for (;;) {
                        std::vector<int> segment_ids;
                        bool wrap = false, opens = false;
                        auto segment = engine.generate(
                            prompt, prepared.max_new - generated, prepared.sampling,
                            [&](int id) {
                                if (disconnected || closed())
                                    return false;
                                if (id < 0)
                                    return output.ping(engine.snapshot().value("progress", J(nullptr)));
                                ++generated;
                                if (!record.is_null() && record["first_token_s"].is_null()) {
                                    record["first_token_s"] = std::chrono::duration<double>(
                                                                  std::chrono::steady_clock::now() - started)
                                                                  .count();
                                    record["state"] = "generating";
                                    history.update(record);
                                }
                                if (static_cast<size_t>(id) >= tokenizer.size())
                                    throw std::runtime_error("engine token outside tokenizer vocabulary");
                                const auto raw = tokenizer.token_bytes(id);
                                if (raw == "<|im_end|>" || raw == "<|endoftext|>") {
                                    natural = true;
                                    return false;
                                }
                                segment_ids.push_back(id);
                                reasoning_tokens +=
                                    parser.state() == "reasoning" || parser.state() == "rcall";
                                repeat = id == last_id ? repeat + 1 : 1;
                                last_id = id;
                                const auto limit = cfg.value("repeat_stop_tokens", 256);
                                if (limit > 0 && repeat >= limit) {
                                    repeated = true;
                                    return false;
                                }
                                const auto ok = events(parser.feed(decoder.push(raw)));
                                if (!ok)
                                    return false;
                                if (budget && parser.state() == "reasoning") {
                                    ++thought;
                                    if (thought >= budget && parser.boundary() && !decoder.pending()) {
                                        wrap = true;
                                        return false;
                                    }
                                }
                                if (!force.empty()) {
                                    if (called)
                                        force.clear();
                                    else if (parser.state() == "content" && parser.boundary() &&
                                             !decoder.pending()) {
                                        opens = true;
                                        return false;
                                    }
                                }
                                return true;
                            },
                            cancelled, lease.get());
                        if (done.is_null())
                            done = segment;
                        else {
                            for (auto field :
                                 {"generated", "prompt_ms", "decode_ms", "drafts_offered", "drafts_accepted",
                                  "hits", "lookups", "offloaded", "ram_blobs", "file_blobs", "file_mb"})
                                if (segment.contains(field))
                                    done[field] = done.value(field, 0.) + segment[field].get<double>();
                            done["finish"] = segment["finish"];
                        }
                        if (!(wrap || opens) || cancelled())
                            break;
                        const auto insertion = wrap
                                                   ? std::string("\n\nI have thought about this long enough; "
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
                            reasoning_tokens += parser.state() == "reasoning" || parser.state() == "rcall";
                            events(parser.feed(decoder.push(tokenizer.token_bytes(id))));
                        }
                        if (stops.hit)
                            break;
                        prompt.insert(prompt.end(), segment_ids.begin(), segment_ids.end());
                        prompt.insert(prompt.end(), extra.begin(), extra.end());
                    }
                    if (!stops.hit)
                        events(parser.feed(decoder.push({}, true)));
                    if (stops.hit || natural)
                        done["finish"] = "stop";
                    if (repeated)
                        done["finish"] = "length";
                    events(parser.finish(done.value("finish", "stop")));
                    if (!stops.hit) {
                        auto held = stops.flush();
                        if (!held.empty())
                            deliver({"content", held, {}});
                    }
                    done["completion_tokens"] = generated;
                    done["reasoning_tokens"] = reasoning_tokens;
                    if (prepared.format.enabled() && !(prepared.dialect == ApiDialect::responses && called)) {
                        try {
                            prepared.format.validate(answer, done.value("finish", "stop"));
                        } catch (const std::exception &e) {
                            throw ApiError(500, e.what(), "structured_output_failed", {},
                                           "structured_output_failed");
                        }
                    }
                    auto result = output.finish(done, stops.hit);
                    if (prepared.format.enabled())
                        for (const auto &e : buffered)
                            if (!writer(e["name"].get<std::string>(), e["body"])) {
                                disconnected = true;
                                break;
                            }
                    const auto duration =
                        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
                    if (!record.is_null()) {
                        record["usage"] = result.value("usage", J(nullptr));
                        record["state"] = disconnected || closed() ? "disconnected" : "completed";
                        record["wallclock_s"] = duration;
                        record["timings"] = result.value("timings", J(nullptr));
                        if (!prepared.stream) {
                            const auto bytes = result.dump();
                            record["response"] = bytes.substr(0, 262144);
                            record["response_truncated"] = bytes.size() > 262144;
                        }
                    }
                    history.add({{"id", request_id},
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
                                 {"decode_tok_s", done.value("decode_ms", 0.) > 0
                                                      ? J(1000. * done.value("generated", generated) /
                                                          done["decode_ms"].get<double>())
                                                      : J(nullptr)},
                                 {"drafts_offered", done.value("drafts_offered", 0)},
                                 {"drafts_accepted", done.value("drafts_accepted", 0)}},
                                record);
                    return result;
                } catch (const ApiError &e) {
                    if (!record.is_null()) {
                        record["state"] = "error";
                        record["error"] = e.body()["error"];
                        record["http_status"] = prepared.stream ? 200 : e.status;
                        record["wallclock_s"] =
                            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
                    }
                    history.add({{"id", request_id}, {"path", path}, {"error", e.body()}}, record);
                    if (prepared.stream) {
                        if (prepared.format.enabled()) {
                            ApiOutput failure(prepared, requested, writer);
                            failure.start();
                            failure.failed(e);
                        } else
                            output.failed(e);
                        return nullptr;
                    }
                    throw;
                } catch (const std::exception &) {
                    ApiError e(500, "native generation failed", "server_error");
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
                "text/event-stream", [execute, dialect](size_t, httplib::DataSink &sink) {
                    auto writer = [&](const std::string &name, const J &event) {
                        if (!sink.is_writable())
                            return false;
                        const auto bytes = event.is_null()
                                               ? std::string(": keep-alive\n\n")
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
        };
        http.Post("/v1/chat/completions/?", generate);
        http.Post("/v1/messages/?", generate);
        http.Post("/v1/responses/?", generate);
        http.Post("/v1/messages/count_tokens/?", generate);
        http.Post(".*", [&](const httplib::Request &req, httplib::Response &res) {
            if (protect(req, res))
                json(res, 400,
                     ApiError(400, "native server does not yet support this route", "invalid_request_error",
                              {}, "unsupported_parameter")
                         .body());
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
        std::cout << "ready: http://" << host << ':' << port << "/v1 (native server, context "
                  << engine.context() << ")" << std::endl;
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
#ifdef _WIN32
int wmain(int argc, wchar_t **argv) {
    std::vector<std::string> args;
    for (int i = 0; i < argc; ++i)
        args.push_back(strata::runtime::path_utf8(std::filesystem::path(argv[i])));
    return run(args);
}
#else
int main(int argc, char **argv) { return run(std::vector<std::string>(argv, argv + argc)); }
#endif
