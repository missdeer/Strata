#pragma once

#include <nlohmann/json.hpp>
#include <filesystem>
#include <string>
#include <vector>

namespace strata::runtime {
using Json = nlohmann::json;

// Rejects duplicate keys and bounds the document before parsing. Used only at
// startup; the expert request path does not parse JSON.
Json read_json(const std::filesystem::path& path);

struct ClusterConfig {
    Json data;
    std::string digest, assignment, endpoints;
    std::vector<Json> workers;
    Json coordinator;

    explicit ClusterConfig(Json value);
    static ClusterConfig load(const std::filesystem::path& path);
    const Json& node(const std::string& id) const;
};
} // namespace strata::runtime
