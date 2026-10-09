#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <sddl.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "strata/runtime/cluster_launch.hpp"
#include "strata/runtime/path.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <random>
#include <set>
#include <stdexcept>
#include <thread>

namespace strata::runtime {
namespace {
#ifdef _WIN32
std::atomic<bool> stopped{false};
struct Handle {
    HANDLE value = nullptr;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
struct PrivateSecurity {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), nullptr, FALSE};
    PrivateSecurity() {
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;FA;;;OW)", SDDL_REVISION_1, &descriptor, nullptr))
            throw std::runtime_error("cannot create private configuration ACL");
        attributes.lpSecurityDescriptor = descriptor;
    }
    ~PrivateSecurity() { LocalFree(descriptor); }
};
BOOL WINAPI stop_console(DWORD event) {
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT || event == CTRL_CLOSE_EVENT ||
        event == CTRL_SHUTDOWN_EVENT || event == CTRL_LOGOFF_EVENT) {
        stopped = 1; return TRUE;
    }
    return FALSE;
}
// CommandLineToArgvW / CRT quoting, including trailing backslashes and empty args.
std::wstring quote(const std::string& arg) {
    const auto wide = utf8_path(arg).native();
    std::wstring result = L"\"";
    size_t slashes = 0;
    for (auto c : wide) {
        if (c == L'\\') { ++slashes; continue; }
        result.append(slashes * (c == L'"' ? 2 : 1), L'\\');
        if (c == L'"') result += L'\\';
        result += c; slashes = 0;
    }
    result.append(slashes * 2, L'\\');
    return result + L'"';
}
#else
volatile std::sig_atomic_t stopped = 0;
void stop_signal(int signal) { stopped = signal; }
int status_code(int status) {
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

// This supervisor owns the role's process group. The lifetime pipe closes even
// when the launcher is killed with SIGKILL, allowing cleanup without Python or
// a signal handler that calls unsafe C++ code. The role cannot inherit the pipe.
int supervise(const std::vector<char*>& args, int lifetime) {
    const pid_t child = fork();
    if (child < 0) return 125;
    if (child == 0) {
        close(lifetime);
        if (setpgid(0, 0) != 0) _exit(125);
        std::signal(SIGINT, SIG_DFL); std::signal(SIGTERM, SIG_DFL); std::signal(SIGHUP, SIG_DFL);
        execv(args.front(), args.data());
        _exit(127);
    }
    // Establish the group from both sides so a stop cannot race exec/setpgid.
    if (setpgid(child, child) != 0 && errno != EACCES && errno != ESRCH) {
        kill(child, SIGKILL); waitpid(child, nullptr, 0); return 125;
    }
    bool stopping = false;
    auto deadline = std::chrono::steady_clock::time_point::max();
    for (;;) {
        int status = 0;
        const auto done = waitpid(child, &status, WNOHANG);
        if (done == child) { kill(-child, SIGKILL); return status_code(status); }
        if (done < 0 && errno != EINTR) { kill(-child, SIGKILL); return 125; }
        pollfd pipe{lifetime, POLLIN, 0};
        const auto polled = poll(&pipe, 1, 100);
        if (!stopping && (stopped || (polled > 0 && (pipe.revents & (POLLHUP | POLLERR | POLLIN))))) {
            kill(-child, stopped ? stopped : SIGTERM);
            stopping = true;
            deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        }
        if (stopping && std::chrono::steady_clock::now() >= deadline) kill(-child, SIGKILL);
    }
}
#endif
} // namespace

std::string environment_utf8(const char* name) {
#ifdef _WIN32
    const auto wide_name = utf8_path(name).native();
    const auto size = GetEnvironmentVariableW(wide_name.c_str(), nullptr, 0);
    if (!size) return {};
    std::wstring value(size, L'\0');
    SetLastError(ERROR_SUCCESS);
    const auto written = GetEnvironmentVariableW(wide_name.c_str(), value.data(), size);
    if ((!written && GetLastError() != ERROR_SUCCESS) || written >= size)
        throw std::runtime_error("cannot read environment variable");
    value.resize(written);
    return path_utf8(std::filesystem::path(value));
#else
    const auto* value = std::getenv(name); return value ? value : "";
#endif
}

std::filesystem::path executable_directory() {
#ifdef _WIN32
    std::wstring path(32768, L'\0');
    const auto size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!size || size >= path.size()) throw std::runtime_error("cannot locate launcher executable");
    path.resize(size); return std::filesystem::path(path).parent_path();
#else
    std::array<char, 32768> path{};
    const auto size = readlink("/proc/self/exe", path.data(), path.size());
    if (size <= 0 || static_cast<size_t>(size) == path.size()) throw std::runtime_error("cannot locate launcher executable");
    return utf8_path(std::string(path.data(), static_cast<size_t>(size))).parent_path();
#endif
}

Json coordinator_config(const Json& source, const ClusterConfig& plan, const Json& node,
                        const std::string& engine, const std::string& prefill,
                        bool no_api_key, const std::filesystem::path& working_directory) {
    if (!source.is_object() || !source.contains("args") || !source["args"].is_array())
        throw std::runtime_error("coordinator server config requires an args array");
    const auto& old = source["args"];
    for (const auto& arg : old) if (!arg.is_string()) throw std::runtime_error("server args must be strings");
    if (std::find(old.begin(), old.end(), Json("--native")) == old.end())
        throw std::runtime_error("network workers require an installed native IQ model");
    const std::set<std::string> values{
        "--network-workers", "--network-timeout-ms", "--network-assignment", "--network-config-id",
        "--max-context", "--prefill", "--batch", "--slots", "--layer-split", "--split-device", "--peer-device",
        "--expert-cache-device1", "--expert-cache-device2", "--expert-cache-device3", "--pipeline-windows",
        "--shared-expert-arena", "--resident-budget-gib", "--kv-resident", "--pcie-frac", "--short-read",
        "--gpu", "--network-local-ram-mib"};
    const std::set<std::string> flags{
        "--no-pool", "--resident-experts", "--resident-cpu-experts", "--resident-cpu-experts-soft",
        "--remote-expert-opt", "--split-skip-if-fits", "--trim-stage-weights", "--mmap-experts", "--network-reduce"};
    Json args = Json::array();
    for (size_t i = 0; i < old.size(); ++i) {
        const auto arg = old[i].get<std::string>();
        if (values.count(arg)) {
            if (++i == old.size()) throw std::runtime_error("missing value for " + arg);
        } else if (!flags.count(arg)) args.push_back(old[i]);
    }
    for (const auto& arg : std::vector<std::string>{"--network-workers", plan.endpoints, "--network-timeout-ms", "5000",
            "--mmap-experts", "--pcie-frac", "0", "--short-read", "0", "--max-context",
            std::to_string(node["context"].get<int>()), "--prefill", "512"}) args.push_back(arg);
    if (node.value("network_reduce", false)) args.push_back("--network-reduce");
    if (node.value("network_local_ram_mib", int64_t(0))) {
        args.push_back("--network-local-ram-mib"); args.push_back(std::to_string(node["network_local_ram_mib"].get<int64_t>()));
    }
    for (const auto& arg : std::vector<std::string>{"--prefill", prefill, "--network-assignment", plan.assignment,
            "--network-config-id", plan.digest, "--network-timeout-ms", std::to_string(plan.data["scheduler"]["timeout_ms"].get<int>())})
        args.push_back(arg);
    Json cfg = source;
    cfg["args"] = std::move(args);
    cfg["gpu"] = 0; cfg["host"] = "127.0.0.1"; cfg["open_browser"] = false;
    cfg["model_name"] = source.value("model_name", std::string("strata")) + "-network";
    cfg["exe"] = path_utf8(std::filesystem::absolute(utf8_path(engine)));
    cfg["log"] = path_utf8(working_directory / "logs" / ("cluster-" + node["id"].get<std::string>() + ".log"));
    for (const auto* key : {"layer_split", "split_skip_if_fits", "batch", "batch_groups", "pipeline_windows"}) cfg.erase(key);
    if (no_api_key) cfg.erase("api_key");
    const auto port = cfg.value("port", Json(8080));
    if (!port.is_number_integer() || (port.is_number_unsigned() && port.get<uint64_t>() > 65535) ||
        port.get<int64_t>() < 1 || port.get<int64_t>() > 65535)
        throw std::runtime_error("coordinator server config port must be an integer in 1..65535");
    return cfg;
}

void write_private_json(const std::filesystem::path& path, const Json& value) {
    const auto text = value.dump(2) + "\n";
#ifdef _WIN32
    PrivateSecurity security;
    Handle file{CreateFileW(path.c_str(), GENERIC_WRITE, 0, &security.attributes, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (file.value == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot exclusively create private runtime config");
    DWORD written = 0;
    if (text.size() > MAXDWORD || !WriteFile(file.value, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) || written != text.size())
        throw std::runtime_error("cannot write runtime config");
#else
    const int file = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (file < 0) throw std::runtime_error("cannot exclusively create private runtime config");
    size_t offset = 0;
    while (offset < text.size()) {
        const auto written = write(file, text.data() + offset, text.size() - offset);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) { close(file); throw std::runtime_error("cannot write runtime config"); }
        offset += static_cast<size_t>(written);
    }
    if (close(file) != 0) throw std::runtime_error("cannot close runtime config");
#endif
}

RuntimeDirectory::RuntimeDirectory() {
    std::random_device random;
    for (int attempt = 0; attempt < 100; ++attempt) {
        auto candidate = std::filesystem::temp_directory_path() /
            ("strata-cluster-" + std::to_string(random()) + "-" + std::to_string(random()));
#ifdef _WIN32
        PrivateSecurity security;
        if (CreateDirectoryW(candidate.c_str(), &security.attributes)) { path = std::move(candidate); return; }
        if (GetLastError() != ERROR_ALREADY_EXISTS) break;
#else
        if (mkdir(candidate.c_str(), 0700) == 0) { path = std::move(candidate); return; }
        if (errno != EEXIST) break;
#endif
    }
    throw std::runtime_error("cannot create private runtime directory");
}
RuntimeDirectory::~RuntimeDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path, error);
}

int run_contained(const std::vector<std::string>& argv) {
    if (argv.empty()) throw std::runtime_error("empty child command");
    stopped = 0;
#ifdef _WIN32
    Handle job{CreateJobObjectW(nullptr, nullptr)};
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job.value || !SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
        throw std::runtime_error("cannot create child containment job");
    if (!SetConsoleCtrlHandler(stop_console, TRUE)) throw std::runtime_error("cannot install console stop handler");
    struct HandlerCleanup { ~HandlerCleanup() { SetConsoleCtrlHandler(stop_console, FALSE); } } handler;
    std::wstring command;
    for (const auto& arg : argv) { if (!command.empty()) command += L' '; command += quote(arg); }
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const auto executable = utf8_path(argv.front());
    // An invalid/missing role must fail unattended, without a loader error dialog.
    DWORD old_error_mode = 0;
    if (!SetThreadErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX, &old_error_mode))
        throw std::runtime_error("cannot disable child loader error dialogs");
    struct ErrorModeCleanup { DWORD previous; ~ErrorModeCleanup() { SetThreadErrorMode(previous, nullptr); } } mode{old_error_mode};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
                        CREATE_SUSPENDED | CREATE_NEW_PROCESS_GROUP, nullptr, nullptr, &startup, &process))
        throw std::runtime_error("cannot start native role executable");
    Handle child{process.hProcess}, thread{process.hThread};
    if (!AssignProcessToJobObject(job.value, child.value) || ResumeThread(thread.value) == static_cast<DWORD>(-1)) {
        TerminateProcess(child.value, 125); WaitForSingleObject(child.value, INFINITE);
        throw std::runtime_error("cannot contain/resume native role process");
    }
    bool stopping = false;
    auto deadline = std::chrono::steady_clock::time_point::max();
    for (;;) {
        const auto status = WaitForSingleObject(child.value, 100);
        if (status == WAIT_OBJECT_0) break;
        if (status != WAIT_TIMEOUT) throw std::runtime_error("cannot wait for native role process");
        if (stopped && !stopping) {
            GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, process.dwProcessId);
            stopping = true; deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        }
        if (stopping && std::chrono::steady_clock::now() >= deadline) TerminateJobObject(job.value, 130);
    }
    DWORD result = 125;
    if (!GetExitCodeProcess(child.value, &result)) throw std::runtime_error("cannot read native role exit status");
    // Wait for descendants to release files before deleting the runtime config.
    // Kill-on-close remains the backstop for abrupt launcher termination.
    if (!TerminateJobObject(job.value, result)) throw std::runtime_error("cannot stop native role descendants");
    const auto cleanup_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    for (;;) {
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
        if (!QueryInformationJobObject(job.value, JobObjectBasicAccountingInformation, &accounting, sizeof(accounting), nullptr))
            throw std::runtime_error("cannot check native role descendants");
        if (!accounting.ActiveProcesses) break;
        if (std::chrono::steady_clock::now() >= cleanup_deadline)
            throw std::runtime_error("native role descendants did not stop");
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return static_cast<int>(result);
#else
    struct sigaction action{}, old_int{}, old_term{}, old_hup{};
    action.sa_handler = stop_signal; sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, &old_int) || sigaction(SIGTERM, &action, &old_term) || sigaction(SIGHUP, &action, &old_hup))
        throw std::runtime_error("cannot install launcher signal handlers");
    struct HandlerCleanup {
        struct sigaction i, t, h;
        ~HandlerCleanup() { sigaction(SIGINT, &i, nullptr); sigaction(SIGTERM, &t, nullptr); sigaction(SIGHUP, &h, nullptr); }
    } handler{old_int, old_term, old_hup};
    std::vector<char*> args;
    for (const auto& arg : argv) args.push_back(const_cast<char*>(arg.c_str()));
    args.push_back(nullptr);
    int lifetime[2];
    if (pipe2(lifetime, O_CLOEXEC)) throw std::runtime_error("cannot create launcher lifetime pipe");
    const pid_t supervisor = fork();
    if (supervisor < 0) { close(lifetime[0]); close(lifetime[1]); throw std::runtime_error("cannot fork role supervisor"); }
    if (!supervisor) { close(lifetime[1]); _exit(supervise(args, lifetime[0])); }
    close(lifetime[0]);
    int status = 0;
    for (;;) {
        const auto done = waitpid(supervisor, &status, WNOHANG);
        if (done == supervisor) break;
        if (done < 0 && errno != EINTR) {
            close(lifetime[1]); throw std::runtime_error("cannot wait for role supervisor");
        }
        if (stopped) { kill(supervisor, stopped); stopped = 0; }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    close(lifetime[1]); return status_code(status);
#endif
}
} // namespace strata::runtime
