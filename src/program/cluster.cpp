#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif
#include "strata/runtime/cluster_launch.hpp"
#include "strata/runtime/path.hpp"
#include <algorithm>
#include <iostream>
#include <set>
#include <stdexcept>

namespace {
using namespace strata::runtime;
int run(const std::vector<std::string>& argv) {
    try {
        std::string config, node_id, engine, prefill, api, output;
        bool validate = false, no_key = false;
        std::set<std::string> seen;
        for (size_t i = 1; i < argv.size(); ++i) {
            const auto& arg = argv[i];
            if (!seen.insert(arg).second) throw std::runtime_error("duplicate option: " + arg);
            if (arg == "--help") {
                std::cout << "strata-cluster --config cluster.json --validate | --node ID\n"
                    "  --exe PATH       coordinator engine override\n"
                    "  --prefill VALUE  512/1024/2048/4096/8192/auto/auto:8192\n"
                    "  --no-api-key     loopback coordinator without API authentication\n"
                    "  --api-exe PATH   native API executable (default: sibling strata-server)\n"
                    "  --export-config PATH  create private coordinator config and exit; never overwrite\n";
                return 0;
            }
            if (arg == "--validate") validate = true;
            else if (arg == "--no-api-key") no_key = true;
            else {
                std::string* target = nullptr;
                if (arg == "--config") target = &config;
                else if (arg == "--node") target = &node_id;
                else if (arg == "--exe") target = &engine;
                else if (arg == "--prefill") target = &prefill;
                else if (arg == "--api-exe") target = &api;
                else if (arg == "--export-config") target = &output;
                else throw std::runtime_error("unknown option: " + arg);
                if (++i == argv.size() || argv[i].empty()) throw std::runtime_error("missing value for " + arg);
                *target = argv[i];
            }
        }
        if (config.empty()) throw std::runtime_error("--config is required");
        if (!prefill.empty() && !std::set<std::string>{"512", "1024", "2048", "4096", "8192", "auto", "auto:8192"}.count(prefill))
            throw std::runtime_error("invalid --prefill");
        const auto config_path = std::filesystem::absolute(utf8_path(config));
        const auto plan = ClusterConfig::load(config_path);
        const Json* node = node_id.empty() ? nullptr : &plan.node(node_id);
        if (validate) {
            if (!output.empty()) throw std::runtime_error("--validate cannot export a coordinator config");
            std::cout << Json{{"config_sha256", plan.digest}, {"assignment", plan.assignment}, {"endpoints", plan.endpoints}}.dump() << std::endl;
            return 0;
        }
        if (!node) throw std::runtime_error("--node is required for startup");
        const bool worker = (*node)["role"] == "expert-worker";
        if (worker && (!engine.empty() || !prefill.empty() || no_key || !api.empty() || !output.empty()))
            throw std::runtime_error("coordinator options cannot be used for an expert-worker");
#ifdef _WIN32
        constexpr auto suffix = ".exe";
#else
        constexpr auto suffix = "";
#endif
        const auto sibling = executable_directory();
        if (worker) {
            const auto key = environment_utf8("STRATA_NETWORK_KEY");
            if (key.size() < 16 || key.size() > 256) throw std::runtime_error("set STRATA_NETWORK_KEY to a 16..256 byte secret");
            return run_contained({path_utf8(sibling / (std::string("strata-network-worker") + suffix)),
                                  "--cluster-config", path_utf8(config_path), "--node", node_id});
        }
        const auto cwd = std::filesystem::current_path();
        const auto source_path = std::filesystem::absolute(utf8_path((*node)["server_config"].get<std::string>()));
        const auto cfg = coordinator_config(read_json(source_path), plan, *node,
            engine.empty() ? (*node)["engine"].get<std::string>() : engine,
            prefill.empty() ? std::to_string((*node)["prefill"].get<int>()) : prefill, no_key, cwd);
        if (!output.empty()) {
            // Exclusive creation also rejects aliases, symlinks and hardlinks to inputs.
            write_private_json(std::filesystem::absolute(utf8_path(output)), cfg);
            std::cout << "Created private coordinator config; source and shared plan unchanged." << std::endl;
            return 0;
        }
        const auto key = environment_utf8("STRATA_NETWORK_KEY");
        if (key.size() < 16 || key.size() > 256) throw std::runtime_error("set STRATA_NETWORK_KEY to a 16..256 byte secret");
        const auto service = api.empty() ? sibling / (std::string("strata-server") + suffix) : std::filesystem::absolute(utf8_path(api));
        if (!std::filesystem::is_regular_file(service))
            throw std::runtime_error("native API executable unavailable (PLAN.md stage 4); use --export-config to inspect the plan");
        if (no_key) {
#ifdef _WIN32
            SetEnvironmentVariableW(L"STRATA_API_KEY", nullptr);
#else
            unsetenv("STRATA_API_KEY");
#endif
        }
        RuntimeDirectory runtime;
        const auto runtime_config = runtime.path / "coordinator.json";
        write_private_json(runtime_config, cfg);
        std::filesystem::create_directories(cwd / "logs");
        return run_contained({path_utf8(service), "--engine", "strata", "--config", path_utf8(runtime_config),
                              "--port", std::to_string(cfg.value("port", 8080))});
    } catch (const Json::exception&) {
        // JSON parse/type diagnostics can include fragments of a private API key.
        std::cerr << "cluster startup failed: invalid JSON configuration or field type" << std::endl;
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "cluster startup failed: " << error.what() << std::endl;
        return 2;
    }
}
} // namespace
#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    std::vector<std::string> args;
    for (int i = 0; i < argc; ++i) args.push_back(strata::runtime::path_utf8(std::filesystem::path(argv[i])));
    return run(args);
}
#else
int main(int argc, char** argv) { return run(std::vector<std::string>(argv, argv + argc)); }
#endif
