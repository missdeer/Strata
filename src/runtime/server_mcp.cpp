#include "strata/runtime/server_mcp.hpp"
#include "strata/runtime/server_engine.hpp"
#include "strata/runtime/native_http.hpp"
#include "strata/runtime/path.hpp"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <sstream>
namespace strata::runtime {
namespace {
using J = TextJson;
using Clock = std::chrono::steady_clock;
J settings(const J &cfg) {
    J out{{"timeout_s", 60.}, {"start_timeout_s", 120.}, {"max_result_chars", 20000}, {"max_rounds", 8}};
    const auto given = cfg.value("mcp", J::object());
    if (!given.is_object())
        throw std::invalid_argument("mcp must be an object");
    for (auto it = given.begin(); it != given.end(); ++it) {
        if (!out.contains(it.key()) || !it.value().is_number() || it.value().get<double>() <= 0)
            throw std::invalid_argument("invalid MCP setting: " + it.key());
        if ((it.key() == "max_rounds" || it.key() == "max_result_chars") && !it.value().is_number_integer())
            throw std::invalid_argument("MCP counts must be integers");
        out[it.key()] = it.value();
    }
    return out;
}
J servers(const J &cfg) {
    J result = J::object();
    for (auto key : {"mcp_servers", "mcpServers"})
        if (cfg.contains(key) && !cfg[key].is_null()) {
            if (!cfg[key].is_object())
                throw std::invalid_argument(std::string(key) + " must be an object");
            result.update(cfg[key]);
        }
    return result;
}
std::string clean(const std::string &s) {
    std::string out;
    for (unsigned char c : s) {
        if ((c & 0xc0) == 0x80)
            continue;
        out +=
            (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-'
                ? char(c)
                : '_';
    }
    return out.empty() ? "x" : out;
}
size_t chars(const std::string &text) {
    size_t n = 0;
    for (unsigned char c : text)
        if ((c & 0xc0) != 0x80)
            ++n;
    return n;
}
std::string cut(const std::string &text, size_t n) {
    size_t at = 0, count = 0;
    while (at < text.size()) {
        if ((static_cast<unsigned char>(text[at]) & 0xc0) != 0x80 && count++ == n)
            break;
        ++at;
    }
    return text.substr(0, at);
}
std::string number(size_t n) {
    auto s = std::to_string(n);
    for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3)
        s.insert(static_cast<size_t>(i), ",");
    return s;
}
std::string result_text(const J &result) {
    std::vector<std::string> parts;
    for (const auto &b : result.value("content", J::array())) {
        if (!b.is_object())
            continue;
        const auto type = b.value("type", "");
        if (type == "text")
            parts.push_back(b.value("text", ""));
        else if (type == "resource") {
            const auto r = b.value("resource", J::object());
            parts.push_back(r.contains("text") ? r["text"].get<std::string>()
                                               : "[resource " + r.value("uri", "") + " (" +
                                                     r.value("mimeType", "binary") + "), not shown]");
        } else if (type == "resource_link")
            parts.push_back("[resource link: " + b.value("name", "") + " " + b.value("uri", "") + "]");
        else if (type == "image" || type == "audio")
            parts.push_back("[" + type + " (" + b.value("mimeType", "?") + ", " +
                            number(b.value("data", "").size() * 3 / 4) + " bytes): not shown to the model]");
        else
            parts.push_back(python_json(b));
    }
    if (parts.empty() && result.contains("structuredContent") && !result["structuredContent"].is_null())
        parts.push_back(python_json(result["structuredContent"]));
    std::string out;
    for (const auto &p : parts) {
        if (!out.empty())
            out += '\n';
        out += p;
    }
    return out;
}
} // namespace
void validate_mcp(const J &cfg) {
    settings(cfg);
    const auto all = servers(cfg);
    for (auto it = all.begin(); it != all.end(); ++it) {
        const auto &c = it.value();
        if (c.is_object() && c.value("disabled", false))
            continue;
        if (!c.is_object())
            throw std::invalid_argument("MCP server must be an object");
        if (c.contains("url")) {
            if (!c["url"].is_string() || (!c["url"].get<std::string>().starts_with("http://") &&
                                          !c["url"].get<std::string>().starts_with("https://")))
                throw std::invalid_argument("MCP URL must use HTTP or HTTPS");
            if (c.value("type", "") == "sse")
                throw std::invalid_argument("MCP requires Streamable HTTP, not legacy SSE");
            if (c.contains("headers") && !c["headers"].is_object())
                throw std::invalid_argument("MCP headers must be an object");
        } else if (!c.contains("command") || !c["command"].is_string() ||
                   c["command"].get<std::string>().empty())
            throw std::invalid_argument("MCP server requires command or URL");
        if (c.contains("args") && !c["args"].is_array())
            throw std::invalid_argument("MCP args must be an array");
        if (c.contains("env") && !c["env"].is_object())
            throw std::invalid_argument("MCP env must be an object");
    }
}
struct McpHub::Impl {
    struct Server {
        Impl *hub;
        std::string name;
        J config, tools = J::array(), info = J::object();
        std::string state = "starting", error, session, protocol = "2025-06-18";
        std::mutex state_mutex;
        std::timed_mutex rpc_mutex;
        std::unique_ptr<EngineProcess> process;
        uint64_t next = 0;
        Clock::time_point last_start{};
        std::thread startup;
        Server(Impl *owner, std::string n, J c) : hub(owner), name(std::move(n)), config(std::move(c)) {}
        bool cancelled(const std::function<bool()> &cancel) const {
            return hub->stopping || (cancel && cancel());
        }
        std::string safe(std::string text) const {
            for (auto field : {"headers", "env"}) {
                const auto values = config.value(field, J::object());
                for (const auto &v : values)
                    if (v.is_string() && v.get<std::string>().size() >= 4) {
                        const auto secret = v.get<std::string>();
                        size_t at = 0;
                        while ((at = text.find(secret, at)) != std::string::npos) {
                            text.replace(at, secret.size(), "<redacted>");
                            at += 10;
                        }
                    }
            }
            return text;
        }
        void set_state(const std::string &value, const std::string &why = {}) {
            std::lock_guard<std::mutex> lock(state_mutex);
            state = value;
            error = safe(why);
        }
        void notify(const std::string &method, const J &params = J::object()) {
            J message{{"jsonrpc", "2.0"}, {"method", method}, {"params", params}};
            if (process)
                process->send(message.dump());
            else {
                auto headers = config.value("headers", J::object()).get<std::map<std::string, std::string>>();
                headers["Content-Type"] = "application/json";
                headers["Accept"] = "application/json, text/event-stream";
                if (!session.empty())
                    headers["Mcp-Session-Id"] = session;
                if (!protocol.empty())
                    headers["MCP-Protocol-Version"] = protocol;
                auto r = native_http("POST", config["url"].get<std::string>(), headers, message.dump(),
                                     hub->options["timeout_s"].get<double>(), 16u << 20,
                                     [&] { return hub->stopping.load(); });
                if (r.status >= 400)
                    throw std::runtime_error("MCP HTTP notification failed");
            }
        }
        void close() {
            if (process) {
                process->close();
                process.reset();
            } else if (config.contains("url") && !session.empty()) {
                try {
                    auto headers =
                        config.value("headers", J::object()).get<std::map<std::string, std::string>>();
                    headers["Mcp-Session-Id"] = session;
                    headers["MCP-Protocol-Version"] = protocol;
                    headers["Accept"] = "application/json, text/event-stream";
                    native_http("DELETE", config["url"].get<std::string>(), headers, "", 3, 16u << 20);
                } catch (...) {
                }
            }
            session.clear();
        }
        J request(const std::string &method, const J &params, double timeout,
                  const std::function<bool()> &cancel) {
            const auto id = ++next;
            const J message{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}};
            J found = nullptr;
            auto accept = [&](const J &m) {
                if (m.is_object() && m.value("id", J(nullptr)) == id && !m.contains("method")) {
                    found = m;
                    return true;
                }
                return false;
            };
            if (process) {
                process->send(message.dump());
                const auto end = Clock::now() + std::chrono::duration<double>(timeout);
                while (found.is_null()) {
                    if (cancelled(cancel)) {
                        try {
                            process->send(J{{"jsonrpc", "2.0"},
                                            {"method", "notifications/cancelled"},
                                            {"params", {{"requestId", id}, {"reason", "stopped"}}}}
                                              .dump());
                        } catch (...) {
                        }
                        throw ApiError(499, "MCP call cancelled", "server_error");
                    }
                    if (Clock::now() >= end)
                        throw std::runtime_error("MCP request timed out");
                    std::string line;
                    if (!process->receive(line, std::chrono::milliseconds(100)))
                        continue;
                    auto m = parse_text_json(line);
                    if (m.is_object() && m.contains("method")) {
                        if (m.contains("id")) {
                            J reply{{"jsonrpc", "2.0"}, {"id", m["id"]}};
                            if (m["method"] == "ping")
                                reply["result"] = J::object();
                            else
                                reply["error"] = {{"code", -32601}, {"message", "method not supported"}};
                            process->send(reply.dump());
                        }
                        continue;
                    }
                    if (m.is_array())
                        for (const auto &row : m)
                            accept(row);
                    else
                        accept(m);
                }
            } else {
                auto headers = config.value("headers", J::object()).get<std::map<std::string, std::string>>();
                headers["Content-Type"] = "application/json";
                headers["Accept"] = "application/json, text/event-stream";
                if (!session.empty())
                    headers["Mcp-Session-Id"] = session;
                if (!protocol.empty())
                    headers["MCP-Protocol-Version"] = protocol;
                std::string stream, payload;
                auto r = native_http(
                    "POST", config["url"].get<std::string>(), headers, message.dump(), timeout, 16u << 20,
                    [&] { return cancelled(cancel); },
                    [&](const std::string &bytes) {
                        stream += bytes;
                        size_t end = 0;
                        while ((end = stream.find('\n')) != std::string::npos) {
                            auto line = stream.substr(0, end);
                            stream.erase(0, end + 1);
                            if (!line.empty() && line.back() == '\r')
                                line.pop_back();
                            if (line.starts_with("data:")) {
                                if (!payload.empty())
                                    payload += '\n';
                                auto value = line.substr(5);
                                while (!value.empty() && value.front() == ' ')
                                    value.erase(value.begin());
                                payload += value;
                            } else if (line.empty() && !payload.empty()) {
                                auto m = J::parse(payload, nullptr, false);
                                payload.clear();
                                if (m.is_array()) {
                                    for (const auto &row : m)
                                        accept(row);
                                } else
                                    accept(m);
                                if (!found.is_null())
                                    return false;
                            }
                        }
                        return true;
                    });
                if (r.headers.count("mcp-session-id"))
                    session = r.headers["mcp-session-id"];
                if (r.status >= 400) {
                    if (r.status == 404 && !session.empty())
                        set_state("stopped");
                    throw std::runtime_error("MCP HTTP " + std::to_string(r.status));
                }
                if (found.is_null()) {
                    auto m = J::parse(r.body, nullptr, false);
                    if (m.is_array())
                        for (const auto &row : m)
                            accept(row);
                    else
                        accept(m);
                }
                if (cancelled(cancel))
                    throw ApiError(499, "MCP call cancelled", "server_error");
            }
            if (found.is_null())
                throw std::runtime_error("MCP answer did not contain a response");
            if (found.contains("error")) {
                const auto e = found["error"];
                throw std::runtime_error(safe(e.is_object() ? e.value("message", "MCP error") + " (code " +
                                                                  e.value("code", J(nullptr)).dump() + ")"
                                                            : "MCP error"));
            }
            return found.value("result", J::object());
        }
        bool start_locked() {
            close();
            set_state("starting");
            last_start = Clock::now();
            try {
                if (!config.contains("url")) {
                    std::vector<std::string> args{config["command"].get<std::string>()};
                    for (const auto &a : config.value("args", J::array()))
                        args.push_back(a.is_string() ? a.get<std::string>() : a.dump());
                    std::map<std::string, std::string> environment;
                    const auto given = config.value("env", J::object());
                    for (auto it = given.begin(); it != given.end(); ++it)
                        environment[it.key()] =
                            it.value().is_string() ? it.value().get<std::string>() : it.value().dump();
                    process =
                        std::make_unique<EngineProcess>(args, utf8_path(config.value("cwd", ".")),
                                                        std::filesystem::path(), environment, 16u << 20);
                }
                const auto timeout = hub->options["start_timeout_s"].get<double>();
                auto initialized =
                    request("initialize",
                            {{"protocolVersion", "2025-06-18"},
                             {"capabilities", J::object()},
                             {"clientInfo", {{"name", "strata"}, {"title", "Strata"}, {"version", "1"}}}},
                            timeout, {});
                protocol = initialized.value("protocolVersion", "2025-06-18");
                notify("notifications/initialized");
                J all = J::array();
                std::string cursor;
                for (int page = 0; page < 100; ++page) {
                    J p;
                    try {
                        p = request("tools/list", cursor.empty() ? J::object() : J{{"cursor", cursor}},
                                    timeout, {});
                    } catch (const std::exception &e) {
                        if (all.empty() && std::string(e.what()).find("-32601") != std::string::npos)
                            break;
                        throw;
                    }
                    for (const auto &tool : p.value("tools", J::array()))
                        if (tool.is_object() && tool.value("name", J(nullptr)).is_string())
                            all.push_back(tool);
                    cursor = p.value("nextCursor", "");
                    if (cursor.empty())
                        break;
                }
                {
                    std::lock_guard<std::mutex> lock(state_mutex);
                    tools = all;
                    info = J::object();
                    const auto given = initialized.value("serverInfo", J::object());
                    for (auto key : {"name", "title", "version"})
                        if (given.contains(key))
                            info[key] = given[key];
                    info["protocol"] = protocol;
                    state = "ready";
                    error.clear();
                }
                return true;
            } catch (const std::exception &e) {
                process.reset();
                {
                    std::lock_guard<std::mutex> lock(state_mutex);
                    tools = J::array();
                }
                set_state("failed", e.what());
                return false;
            }
        }
        J invoke(const std::string &tool, const J &arguments, const std::function<bool()> &cancel) {
            std::unique_lock<std::timed_mutex> lock(rpc_mutex, std::defer_lock);
            while (!lock.try_lock_for(std::chrono::milliseconds(100)))
                if (cancelled(cancel))
                    throw ApiError(499, "MCP call cancelled", "server_error");
            std::string current;
            {
                std::lock_guard<std::mutex> state_lock(state_mutex);
                current = state;
            }
            if (current == "ready" && process && !process->alive())
                current = "stopped";
            if (current != "ready") {
                if (current == "failed" &&
                    std::chrono::duration<double>(Clock::now() - last_start).count() < 10)
                    throw std::runtime_error("MCP server is not running");
                if (!start_locked())
                    throw std::runtime_error("MCP server could not be restarted");
            }
            try {
                return request(
                    "tools/call",
                    {{"name", tool}, {"arguments", arguments.is_object() ? arguments : J::object()}},
                    hub->options["timeout_s"].get<double>(), cancel);
            } catch (...) {
                set_state("stopped");
                process.reset();
                throw;
            }
        }
    };
    J options;
    std::vector<std::unique_ptr<Server>> entries;
    std::atomic<bool> stopping{false};
    std::mutex routes_mutex;
    std::map<std::string, std::pair<Server *, std::string>> routes;
    explicit Impl(const J &cfg) : options(settings(cfg)) {
        validate_mcp(cfg);
        const auto all = servers(cfg);
        for (auto it = all.begin(); it != all.end(); ++it)
            if (!it.value().value("disabled", false))
                entries.push_back(std::make_unique<Server>(this, it.key(), it.value()));
        try {
            for (auto &server : entries)
                server->startup = std::thread([p = server.get()] {
                    std::lock_guard<std::timed_mutex> lock(p->rpc_mutex);
                    p->start_locked();
                });
        } catch (...) {
            stopping = true;
            for (auto &server : entries)
                if (server->startup.joinable())
                    server->startup.join();
            throw;
        }
    }
    ~Impl() {
        stopping = true;
        for (auto &server : entries)
            if (server->startup.joinable())
                server->startup.join();
        for (auto &server : entries) {
            std::lock_guard<std::timed_mutex> lock(server->rpc_mutex);
            server->close();
        }
    }
    J refresh(const std::set<std::string> &exclude = {}) {
        std::lock_guard<std::mutex> lock(routes_mutex);
        routes.clear();
        J result = J::array();
        for (auto &s : entries) {
            std::lock_guard<std::mutex> state(s->state_mutex);
            if (s->state != "ready" && s->state != "stopped")
                continue;
            for (const auto &t : s->tools) {
                const auto original = t["name"].get<std::string>();
                auto name = (clean(s->name) + "__" + clean(original)).substr(0, 64);
                int n = 2;
                while (routes.count(name))
                    name = name.substr(0, 60) + "_" + std::to_string(n++);
                routes[name] = {s.get(), original};
                if (exclude.count(name))
                    continue;
                auto schema = t.value("inputSchema", J::object());
                if (!schema.is_object() || schema.empty())
                    schema = {{"type", "object"}, {"properties", J::object()}};
                result.push_back({{"name", name},
                                  {"description", t.value("description", t.value("title", ""))},
                                  {"parameters", schema}});
            }
        }
        return result;
    }
};
McpHub::McpHub(const J &cfg) : impl_(std::make_unique<Impl>(cfg)) {}
McpHub::~McpHub() = default;
void McpHub::wait(double seconds) {
    const auto end = Clock::now() + std::chrono::duration<double>(seconds);
    for (;;) {
        bool waiting = false;
        for (auto &server : impl_->entries) {
            std::lock_guard<std::mutex> lock(server->state_mutex);
            waiting = waiting || server->state == "starting";
        }
        if (!waiting || Clock::now() >= end)
            return;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}
J McpHub::tools(const std::set<std::string> &exclude) { return impl_->refresh(exclude); }
int McpHub::max_rounds() const { return impl_->options["max_rounds"].get<int>(); }
J McpHub::status() {
    impl_->refresh();
    std::lock_guard<std::mutex> routes(impl_->routes_mutex);
    J entries = J::array();
    for (auto &s : impl_->entries) {
        std::lock_guard<std::mutex> state(s->state_mutex);
        J list = J::array();
        for (const auto &t : s->tools) {
            auto name = t["name"].get<std::string>();
            for (const auto &[flat, route] : impl_->routes)
                if (route.first == s.get() && route.second == name) {
                    name = flat;
                    break;
                }
            list.push_back(
                {{"name", name}, {"tool", t["name"]}, {"description", cut(t.value("description", ""), 300)}});
        }
        entries.push_back({{"name", s->name},
                           {"transport", s->config.contains("url") ? "http" : "stdio"},
                           {"status", s->state},
                           {"error", s->error.empty() ? J(nullptr) : J(s->error)},
                           {"info", s->info},
                           {"tools", list}});
    }
    auto opts = impl_->options;
    opts.erase("start_timeout_s");
    return {{"servers", entries}, {"tools", impl_->routes.size()}, {"settings", opts}};
}
J McpHub::call(const std::string &name, const J &arguments, const std::function<bool()> &cancelled) {
    impl_->refresh();
    Impl::Server *server = nullptr;
    std::string tool = name;
    {
        std::lock_guard<std::mutex> lock(impl_->routes_mutex);
        const auto it = impl_->routes.find(name);
        if (it != impl_->routes.end()) {
            server = it->second.first;
            tool = it->second.second;
        }
    }
    const auto born = Clock::now();
    bool ok = false;
    std::string text;
    try {
        if (!server)
            throw std::runtime_error("there is no tool named " + name);
        const auto result = server->invoke(tool, arguments, cancelled);
        text = result_text(result);
        ok = !result.value("isError", false);
        if (!ok)
            text = "error: " + (text.empty() ? "the tool reported an error" : text);
    } catch (const ApiError &) {
        throw;
    } catch (const std::exception &e) {
        text = "error: " + (server ? server->safe(e.what()) : std::string(e.what()));
    }
    const auto full = chars(text), cap = impl_->options["max_result_chars"].get<size_t>();
    const bool truncated = full > cap;
    if (truncated)
        text = cut(text, cap) + "\n\n[... truncated: the tool returned " + number(full) +
               " characters; only the first " + number(cap) + " are shown]";
    return {{"server", server ? J(server->name) : J(nullptr)},
            {"tool", tool},
            {"ok", ok},
            {"text", text},
            {"chars", full},
            {"truncated", truncated},
            {"ms", static_cast<int64_t>(std::chrono::duration<double>(Clock::now() - born).count() * 1000)}};
}
} // namespace strata::runtime
