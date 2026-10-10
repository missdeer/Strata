#pragma once

#include "strata/runtime/cluster_config.hpp"
#include <filesystem>
#include <string>
#include <vector>

namespace strata::runtime {
// The source object is copied; installed and shared configurations stay untouched.
Json coordinator_config(const Json& source, const ClusterConfig& plan, const Json& node,
                        const std::string& engine, const std::string& prefill,
                        bool no_api_key, const std::filesystem::path& working_directory);
std::string environment_utf8(const char* name);
std::filesystem::path executable_path();
std::filesystem::path executable_directory();
// Exclusive creation with owner-only access, including on Windows. Never overwrites.
void write_private_json(const std::filesystem::path& path, const Json& value);
class RuntimeDirectory {
public:
    RuntimeDirectory();
    ~RuntimeDirectory();
    RuntimeDirectory(const RuntimeDirectory&) = delete;
    RuntimeDirectory& operator=(const RuntimeDirectory&) = delete;
    std::filesystem::path path;
};
// Runs an argv directly, without a shell; contains descendants and forwards stop signals.
int run_contained(const std::vector<std::string>& argv);
} // namespace strata::runtime
