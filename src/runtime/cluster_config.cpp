#include "strata/runtime/cluster_config.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <set>
#include <stdexcept>
#include <sstream>
extern "C" {
#include "sha256.h"
}

namespace strata::runtime {
namespace {
using Names = std::initializer_list<const char*>;
void fields(const Json& j, Names required, Names optional = {}) {
    if (!j.is_object()) throw std::runtime_error("expected JSON object");
    std::set<std::string> names;
    for (auto key : required) {
        if (!j.contains(key)) throw std::runtime_error(std::string("missing field: ") + key);
        names.emplace(key);
    }
    for (auto key : optional) names.emplace(key);
    for (auto it = j.begin(); it != j.end(); ++it)
        if (!names.count(it.key())) throw std::runtime_error("unknown field: " + it.key());
}
void integer(const Json& j, int64_t low, int64_t high, const char* name) {
    if (!j.is_number_integer() || (j.is_number_unsigned() && j.get<uint64_t>() > uint64_t(high)) ||
        j.get<int64_t>() < low || j.get<int64_t>() > high)
        throw std::runtime_error(std::string(name) + " must be an integer in " +
                                 std::to_string(low) + ".." + std::to_string(high));
}
void string(const Json& j, const char* name) {
    // Match Python str.strip(), including Unicode whitespace, without a locale
    // dependency. JSON parsing has already checked the UTF-8 encoding.
    bool content = false;
    if (j.is_string()) {
        const auto& text = j.get_ref<const std::string&>();
        for (size_t i = 0; i < text.size();) {
            uint32_t cp = static_cast<uint8_t>(text[i++]);
            int rest = 0;
            if (cp >= 0xf0) { cp &= 7; rest = 3; }
            else if (cp >= 0xe0) { cp &= 15; rest = 2; }
            else if (cp >= 0xc0) { cp &= 31; rest = 1; }
            while (rest-- && i < text.size()) cp = (cp << 6) | (static_cast<uint8_t>(text[i++]) & 63);
            if (!((cp >= 9 && cp <= 13) || (cp >= 0x1c && cp <= 0x20) || cp == 0x85 || cp == 0xa0 ||
                  cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200a) || cp == 0x2028 || cp == 0x2029 ||
                  cp == 0x202f || cp == 0x205f || cp == 0x3000)) { content = true; break; }
        }
    }
    if (!content) throw std::runtime_error(std::string(name) + " must be a nonempty string");
}
void boolean(const Json& j, const char* name) {
    if (!j.is_boolean()) throw std::runtime_error(std::string(name) + " must be true or false");
}
void ipv4(const Json& j) {
    string(j, "host");
    const auto text = j.get<std::string>();
    std::istringstream in(text);
    std::string part;
    int count = 0;
    while (std::getline(in, part, '.')) {
        if (++count > 4 || part.empty() || part.size() > 3 || (part.size() > 1 && part[0] == '0') ||
            part.find_first_not_of("0123456789") != std::string::npos || std::stoi(part) > 255)
            throw std::runtime_error("invalid IPv4 address");
    }
    if (count != 4 || text.back() == '.') throw std::runtime_error("invalid IPv4 address");
}
std::string sha256(const std::string& text) {
    std::array<unsigned char, 32> bytes{};
    sha256_hash(bytes.data(), reinterpret_cast<const unsigned char*>(text.data()), text.size());
    std::string result;
    for (auto b : bytes) { result += "0123456789abcdef"[b >> 4]; result += "0123456789abcdef"[b & 15]; }
    return result;
}
} // namespace

Json read_json(const std::filesystem::path& path) {
    // Bound what is actually read, even if the file is replaced/grows after stat.
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open JSON configuration");
    std::string text((1u << 20) + 1, '\0');
    in.read(text.data(), static_cast<std::streamsize>(text.size()));
    text.resize(static_cast<size_t>(in.gcount()));
    if (text.size() > (1u << 20)) throw std::runtime_error("JSON configuration exceeds 1 MiB");
    if (in.bad()) throw std::runtime_error("cannot read JSON configuration");
    std::vector<std::set<std::string>> keys;
    auto callback = [&](int depth, Json::parse_event_t event, Json& parsed) {
        if (depth > 64) throw std::runtime_error("JSON configuration nesting exceeds 64");
        if (event == Json::parse_event_t::object_start) keys.emplace_back();
        else if (event == Json::parse_event_t::key) {
            if (!keys.back().insert(parsed.get<std::string>()).second)
                throw std::runtime_error("duplicate JSON key: " + parsed.get<std::string>());
        } else if (event == Json::parse_event_t::object_end) keys.pop_back();
        return true;
    };
    return Json::parse(text, callback);
}

ClusterConfig ClusterConfig::load(const std::filesystem::path& path) { return ClusterConfig(read_json(path)); }

ClusterConfig::ClusterConfig(Json value) : data(std::move(value)) {
    fields(data, {"schema_version", "cluster_id", "revision", "model", "scheduler", "nodes"});
    integer(data["schema_version"], 1, 1, "schema_version");
    integer(data["revision"], 1, 0xffffffff, "revision");
    string(data["cluster_id"], "cluster_id");
    const auto& model = data["model"];
    fields(model, {"name", "layers", "experts"});
    string(model["name"], "model name");
    integer(model["layers"], 1, 128, "model layers");
    integer(model["experts"], 512, 512, "model experts");
    const auto& scheduler = data["scheduler"];
    fields(scheduler, {"policy", "local_gpu_cache_first", "timeout_ms"});
    if (scheduler["policy"] != "expert-ranges" || !scheduler["local_gpu_cache_first"].is_boolean() ||
        scheduler["local_gpu_cache_first"] != true)
        throw std::runtime_error("supported policy: expert-ranges, local_gpu_cache_first=true");
    integer(scheduler["timeout_ms"], 100, 60000, "timeout_ms");
    const auto& nodes = data["nodes"];
    if (!nodes.is_array() || nodes.size() < 2 || nodes.size() > 64)
        throw std::runtime_error("nodes must contain 2..64 entries");
    std::set<std::string> ids, addresses;
    size_t coordinators = 0;
    for (const auto& n : nodes) {
        if (!n.is_object()) throw std::runtime_error("node must be an object");
        const auto role = n.value("role", std::string());
        if (role == "coordinator") {
            fields(n, {"id", "online", "role", "host", "server_config", "engine", "context", "prefill"},
                      {"network_reduce", "network_local_ram_mib"});
            if (n.contains("network_reduce")) boolean(n["network_reduce"], "network_reduce");
            integer(n.value("network_local_ram_mib", Json(0)), 0, 65536, "network_local_ram_mib");
            string(n["server_config"], "server_config"); string(n["engine"], "engine");
            integer(n["context"], 1024, 131072, "context"); integer(n["prefill"], 512, 2048, "prefill");
            coordinator = n; ++coordinators;
        } else if (role == "expert-worker") {
            fields(n, {"id", "online", "role", "host", "port", "backend", "device", "workers", "ram_cache_mib",
                       "gpu_cache_mib", "pack", "native", "library", "expert_ranges"}, {"preload_experts"});
            if (n.contains("preload_experts")) boolean(n["preload_experts"], "preload_experts");
            if (n["backend"] != "cpu" && n["backend"] != "gpu") throw std::runtime_error("worker backend must be cpu or gpu");
            for (auto key : {"pack", "native", "library"}) string(n[key], key);
            integer(n["port"], 1, 65535, "port"); integer(n["device"], 0, 255, "device");
            integer(n["workers"], 0, 256, "workers"); integer(n["ram_cache_mib"], 64, 1048576, "ram_cache_mib");
            integer(n["gpu_cache_mib"], 64, 65536, "gpu_cache_mib");
            if (!n["expert_ranges"].is_array()) throw std::runtime_error("expert_ranges must be an array");
            std::array<bool, 512> seen{};
            for (const auto& range : n["expert_ranges"]) {
                if (!range.is_array() || range.size() != 2) throw std::runtime_error("expert range must be [first,last]");
                integer(range[0], 0, 511, "first expert"); integer(range[1], range[0].get<int>(), 511, "last expert");
                for (int e = range[0].get<int>(); e <= range[1].get<int>(); ++e) {
                    if (seen[e]) throw std::runtime_error("overlapping ranges within a node");
                    seen[e] = true;
                }
            }
            boolean(n["online"], "online");
            if (n["online"].get<bool>()) {
                if (n["expert_ranges"].empty()) throw std::runtime_error("online worker needs expert ranges");
                workers.push_back(n);
            }
        } else throw std::runtime_error("role must be coordinator or expert-worker");
        string(n["id"], "id");
        const auto id = n["id"].get<std::string>();
        if (id.size() > 64 || id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != std::string::npos)
            throw std::runtime_error("invalid node id");
        if (!ids.insert(id).second) throw std::runtime_error("duplicate node id");
        boolean(n["online"], "online"); ipv4(n["host"]);
        if (!addresses.insert(n["host"].get<std::string>()).second) throw std::runtime_error("each node must have a distinct IPv4 address");
    }
    if (coordinators != 1 || !coordinator["online"].get<bool>()) throw std::runtime_error("exactly one online coordinator is required");
    if (workers.empty() || workers.size() > 16) throw std::runtime_error("the plan requires 1..16 online expert workers");
    std::sort(workers.begin(), workers.end(), [](const Json& a, const Json& b) { return a["id"].get<std::string>() < b["id"].get<std::string>(); });
    assignment.assign(512, '?');
    for (size_t i = 0; i < workers.size(); ++i) {
        const auto& n = workers[i];
        if (i) endpoints += ',';
        endpoints += n["host"].get<std::string>() + ":" + std::to_string(n["port"].get<int>());
        for (const auto& range : n["expert_ranges"])
            for (int e = range[0].get<int>(); e <= range[1].get<int>(); ++e) {
                if (assignment[e] != '?') throw std::runtime_error("expert " + std::to_string(e) + " has multiple online owners");
                assignment[e] = "0123456789abcdef"[i];
            }
    }
    const auto gap = assignment.find('?');
    if (gap != std::string::npos) throw std::runtime_error("expert " + std::to_string(gap) + " has no online owner");
    // nlohmann::json orders object keys. ensure_ascii=true matches Python's
    // sort_keys=True, ensure_ascii=True, separators=(',', ':') for this schema.
    digest = sha256(data.dump(-1, ' ', true));
}

const Json& ClusterConfig::node(const std::string& id) const {
    for (const auto& n : data["nodes"]) if (n["id"] == id) {
        if (!n["online"].get<bool>()) throw std::runtime_error("node " + id + " is offline in this startup plan");
        return n;
    }
    throw std::runtime_error("node " + id + " is not in the configuration");
}
} // namespace strata::runtime
