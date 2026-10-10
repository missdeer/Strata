#include "strata/runtime/host_commands.hpp"
#include "strata/runtime/path.hpp"
#include <iostream>

namespace {
int run(const std::vector<std::string>& args) {
    if (args.size() < 2 || args[1] == "--help" || args[1] == "-h") {
        std::cout << "Usage: strata-host <command> [options]\n"
                     "  cluster  validate a shared plan or launch a node\n"
                     "  worker   serve the node's expert shard\n"
                     "  serve    run the HTTP/SSE API and web app\n"
                     "  vision   encode images (resident stdin protocol)\n"
                     "Use strata-host <command> --help for command options.\n";
        return args.size() < 2 ? 2 : 0;
    }
    // Preserve argv[0] for diagnostics; each command parses options from index 1.
    std::vector<std::string> command{args[0]};
    command.insert(command.end(), args.begin() + 2, args.end());
    using namespace strata::runtime;
    if (args[1] == "cluster") return cluster_main(command);
    if (args[1] == "worker") return worker_main(command);
    if (args[1] == "serve") return server_main(command);
    if (args[1] == "vision") return vision_main(command);
    std::cerr << "unknown command: " << args[1] << '\n';
    return 2;
}
} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    std::vector<std::string> args;
    for (int i = 0; i < argc; ++i)
        args.push_back(strata::runtime::path_utf8(std::filesystem::path(argv[i])));
    return run(args);
}
#else
int main(int argc, char** argv) { return run(std::vector<std::string>(argv, argv + argc)); }
#endif
