#include <nlohmann/json.hpp>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>
using J = nlohmann::ordered_json;
std::mutex output;
void send(const J &value) {
    std::lock_guard<std::mutex> lock(output);
    std::cout << value.dump() << std::endl;
}
int main() {
    std::vector<std::thread> work;
    std::atomic<bool> cancelled{false};
    std::string line;
    while (std::getline(std::cin, line)) {
        auto m = J::parse(line, nullptr, false);
        if (!m.is_object() || !m.contains("method"))
            continue;
        const auto method = m["method"].get<std::string>();
        if (method == "notifications/cancelled") {
            cancelled = true;
            continue;
        }
        if (!m.contains("id"))
            continue;
        J result = J::object();
        const auto params = m.value("params", J::object());
        if (method == "initialize") {
            send({{"jsonrpc", "2.0"}, {"id", "server-ping"}, {"method", "ping"}});
            result = {{"protocolVersion", "2025-06-18"},
                      {"capabilities", {{"tools", J::object()}}},
                      {"serverInfo", {{"name", "native-fixture"}, {"version", "1"}}}};
        } else if (method == "tools/list")
            result = {{"tools",
                       J::array({J{{"name", "echo"},
                                   {"description", "Echo text"},
                                   {"inputSchema",
                                    {{"type", "object"}, {"properties", {{"text", {{"type", "string"}}}}}}}},
                                 J{{"name", "slow"}, {"inputSchema", {{"type", "object"}}}},
                                 J{{"name", "fail"}, {"inputSchema", {{"type", "object"}}}}})}};
        else if (method == "tools/call") {
            const auto tool = params.value("name", "");
            if (tool == "fail") {
                send({{"jsonrpc", "2.0"},
                      {"id", m["id"]},
                      {"error", {{"code", -32603}, {"message", "fixture failed"}}}});
                continue;
            }
            if (tool == "slow") {
                auto id = m["id"];
                work.emplace_back([&, id] {
                    for (int i = 0; i < 100 && !cancelled; ++i)
                        std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    send({{"jsonrpc", "2.0"},
                          {"id", id},
                          {"result",
                           {{"content",
                             J::array({J{{"type", "text"},
                                         {"text", cancelled ? "cancelled" : "slow complete"}}})}}}});
                });
                continue;
            }
            auto text = params.value("arguments", J::object()).value("text", "");
            if (const auto *prefix = std::getenv("MCP_TEST_PREFIX"))
                text = std::string(prefix) + text;
            result = {{"content", J::array({J{{"type", "text"}, {"text", text}}})}};
        } else {
            send({{"jsonrpc", "2.0"},
                  {"id", m["id"]},
                  {"error", {{"code", -32601}, {"message", "method not found"}}}});
            continue;
        }
        send({{"jsonrpc", "2.0"}, {"id", m["id"]}, {"result", result}});
    }
    cancelled = true;
    for (auto &t : work)
        if (t.joinable())
            t.join();
}
