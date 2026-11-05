// A native child/grandchild fixture; never included in deployment packages.
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <unistd.h>
#endif
#include "strata/runtime/cluster_launch.hpp"
#include "strata/runtime/path.hpp"
#include <atomic>
#include <csignal>
#include <fstream>
#include <thread>

namespace {
#ifdef _WIN32
std::atomic<bool> stopped{false}, ignore_stop{false};
void stop(int) { if (!ignore_stop) stopped = true; }
BOOL WINAPI console_stop(DWORD) { if (!ignore_stop) stopped = true; return TRUE; }
int pid() { return static_cast<int>(GetCurrentProcessId()); }
#else
volatile std::sig_atomic_t stopped = 0, ignore_stop = 0;
void stop(int) { if (!ignore_stop) stopped = 1; }
int pid() { return static_cast<int>(getpid()); }
#endif
int run(const std::vector<std::string>& args) {
    using namespace strata::runtime;
    std::signal(SIGINT, stop); std::signal(SIGTERM, stop);
#ifdef _WIN32
    SetConsoleCtrlHandler(console_stop, TRUE);
#endif
    if (args.size() == 3 && args[1] == "--leaf") {
        { std::ofstream out(utf8_path(args[2])); out << pid() << std::endl; }
        while (!stopped) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        return 0;
    }
    std::string config;
    for (size_t i = 1; i + 1 < args.size(); ++i) if (args[i] == "--config") config = args[i + 1];
    auto cfg = read_json(utf8_path(config));
    ignore_stop = cfg.value("test_ignore_stop", false);
    cfg["test_argv"] = args;
    cfg["test_runtime_path"] = config;
    cfg["test_cwd"] = path_utf8(std::filesystem::current_path());
    cfg["test_pid"] = pid();
    cfg["test_api_key_present"] = !environment_utf8("STRATA_API_KEY").empty();
    if (cfg.value("test_descendant", false)) {
        const auto leaf = utf8_path(cfg["test_record"].get<std::string>() + ".leaf");
#ifdef _WIN32
        const auto exe = utf8_path(args.front());
        auto cmd = L"\"" + exe.native() + L"\" --leaf \"" + leaf.native() + L"\"";
        STARTUPINFOW startup{}; startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startup, &process)) return 125;
        CloseHandle(process.hThread); CloseHandle(process.hProcess);
#else
        const auto child = fork();
        if (child < 0) return 125;
        if (!child) { execl(args.front().c_str(), args.front().c_str(), "--leaf", leaf.c_str(), nullptr); _exit(127); }
#endif
    }
    { std::ofstream out(utf8_path(cfg["test_record"].get<std::string>())); out << cfg.dump(2); }
    if (cfg.value("test_wait", false))
        while (!stopped) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    return cfg.value("test_exit", 0);
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
