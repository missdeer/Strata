#pragma once
#include "strata/runtime/cluster_config.hpp"
#include "strata/runtime/path.hpp"
#include <map>
#include <set>

namespace strata::build {
namespace fs = std::filesystem;
using runtime::Json;
using runtime::path_utf8;
using runtime::utf8_path;
using Cache = std::map<std::string, std::string>;
struct Options {
    fs::path root, build_root, out_root, ggml, rocm, rocm_compiler;
    std::vector<fs::path> runtime_dirs;
    std::vector<std::string> hosts{"host16", "host11", "host17"}, archs{"gfx1102", "gfx1103"};
    std::map<std::string, fs::path> builds;
    std::map<std::string, fs::path> dependency_dirs;
    int jobs = 6;
    bool dry_run = false, package_only = false;
};
std::string lower(std::string value);
fs::path resolved(const fs::path& path);
bool contains(const fs::path& parent, const fs::path& child);
Cache cache_values(const fs::path& build);
void verify_profile(const std::string& host, const Cache& cache, const std::vector<std::string>& archs);
std::vector<std::string> imports(const fs::path& image);
void copy_runtime(const fs::path& destination, const std::vector<fs::path>& roots,
                  const std::vector<fs::path>& directories);
void copy_tree(const fs::path& source, const fs::path& target);
std::string file_sha256(const fs::path& file);
void atomic_json(const fs::path& path, const Json& value);
std::string publish(const fs::path& stage, const fs::path& destination);
void verify_package(const fs::path& stage, const std::vector<std::string>& programs);
int command(const std::vector<std::string>& args, const fs::path& cwd, const fs::path& log = {});
Options parse(const std::vector<std::string>& args);
int execute(const Options& options);
int main(const std::vector<std::string>& args);
} // namespace strata::build
