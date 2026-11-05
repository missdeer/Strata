#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
#endif
#include "strata/runtime/engine_process.hpp"
#include "strata/runtime/path.hpp"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <future>
#include <cwctype>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace strata::runtime {
struct EngineProcess::Impl {
    std::string buffered;
    size_t maximum_line = 1u << 20;
    mutable int result = -1;
    std::atomic<bool> stop_console{false};
    std::thread console;
#ifdef _WIN32
    HANDLE process = nullptr, job = nullptr, input = nullptr, output = nullptr;
#else
    pid_t process = -1;
    int input = -1, output = -1;
#endif
};
#ifdef _WIN32
namespace {
std::wstring quote(const std::string &arg) {
    const auto s = utf8_path(arg).native();
    std::wstring out = L"\"";
    size_t slashes = 0;
    for (auto c : s) {
        if (c == L'\\') {
            ++slashes;
            continue;
        }
        out.append(slashes * (c == L'"' ? 2 : 1) + (c == L'"' ? 1 : 0), L'\\');
        slashes = 0;
        out += c;
    }
    out.append(slashes * 2, L'\\');
    return out + L"\"";
}
void release(HANDLE &h) {
    if (h && h != INVALID_HANDLE_VALUE)
        CloseHandle(h);
    h = nullptr;
}
} // namespace
#endif
EngineProcess::EngineProcess(const std::vector<std::string> &argv, const std::filesystem::path &cwd,
                             const std::filesystem::path &log,
                             const std::map<std::string, std::string> &environment, size_t maximum_line,
                             const std::vector<std::string> &removed_environment)
    : impl_(std::make_unique<Impl>()) {
    impl_->maximum_line = maximum_line;
    if (argv.empty())
        throw std::invalid_argument("empty engine command");
    const auto console_setting = environment.find("STRATA_ENGINE_CONSOLE");
    const bool console = console_setting != environment.end() && console_setting->second == "1";
    if (console && log.empty())
        throw std::invalid_argument("STRATA_ENGINE_CONSOLE requires an engine log path");
    std::error_code log_error;
    const auto log_size = console ? std::filesystem::file_size(log, log_error) : 0;
    const auto log_start = log_error ? 0 : log_size;
#ifdef _WIN32
    HANDLE child_in = nullptr, child_out = nullptr, err = nullptr;
    LPPROC_THREAD_ATTRIBUTE_LIST attributes = nullptr;
    std::vector<unsigned char> storage;
    PROCESS_INFORMATION pi{};
    try {
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        if (!CreatePipe(&child_in, &impl_->input, &sa, 0) || !CreatePipe(&impl_->output, &child_out, &sa, 0))
            throw std::runtime_error("cannot create engine pipes");
        if (!SetHandleInformation(impl_->input, HANDLE_FLAG_INHERIT, 0) ||
            !SetHandleInformation(impl_->output, HANDLE_FLAG_INHERIT, 0))
            throw std::runtime_error("cannot protect engine pipes");
        err =
            CreateFileW(log.empty() ? L"NUL" : log.c_str(), FILE_APPEND_DATA,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (err == INVALID_HANDLE_VALUE)
            throw std::runtime_error("cannot open engine log");
        impl_->job = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!impl_->job ||
            !SetInformationJobObject(impl_->job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
            throw std::runtime_error("cannot contain engine process");
        SIZE_T bytes = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
        storage.resize(bytes);
        attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
        if (!InitializeProcThreadAttributeList(attributes, 1, 0, &bytes)) {
            attributes = nullptr;
            throw std::runtime_error("cannot initialize engine handles");
        }
        HANDLE handles[]{child_in, child_out, err};
        if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles,
                                       sizeof(handles), nullptr, nullptr))
            throw std::runtime_error("cannot limit inherited engine handles");
        STARTUPINFOEXW si{};
        si.StartupInfo.cb = sizeof(si);
        si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        si.StartupInfo.hStdInput = child_in;
        si.StartupInfo.hStdOutput = child_out;
        si.StartupInfo.hStdError = err;
        si.lpAttributeList = attributes;
        std::wstring cmd;
        for (const auto &arg : argv) {
            if (!cmd.empty())
                cmd += L' ';
            cmd += quote(arg);
        }
        auto exe = utf8_path(argv.front());
        if (exe.is_relative()) {
            if (exe.has_parent_path())
                exe = std::filesystem::absolute(cwd / exe);
            else {
                std::wstring found(32768, L'\0');
                std::wstring search;
                for (const auto &[k, v] : environment)
                    if (_stricmp(k.c_str(), "PATH") == 0)
                        search = utf8_path(v).native();
                for (const auto *extension : {L".exe", L".com", L".cmd", L".bat"}) {
                    const auto n =
                        SearchPathW(search.empty() ? nullptr : search.c_str(), exe.c_str(), extension,
                                    static_cast<DWORD>(found.size()), found.data(), nullptr);
                    if (n && n < found.size()) {
                        found.resize(n);
                        exe = found;
                        break;
                    }
                }
            }
        }
        auto extension = exe.extension().wstring();
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
        if (extension == L".cmd" || extension == L".bat") {
            std::wstring argument_line = quote(path_utf8(exe));
            for (size_t i = 1; i < argv.size(); ++i)
                argument_line += L" " + quote(argv[i]);
            std::wstring system(32768, L'\0');
            const auto n = GetSystemDirectoryW(system.data(), static_cast<UINT>(system.size()));
            if (!n || n >= system.size())
                throw std::runtime_error("cannot locate command interpreter");
            system.resize(n);
            exe = std::filesystem::path(system) / L"cmd.exe";
            cmd = quote(path_utf8(exe)) + L" /d /s /c \"" + argument_line + L"\"";
        } else if (argv.size() == 5 && argv[1] == "/d" && argv[2] == "/s" && argv[3] == "/c")
            cmd = quote(path_utf8(exe)) + L" /d /s /c \"" + utf8_path(argv[4]).native() + L"\"";
        struct Less {
            bool operator()(const std::wstring &a, const std::wstring &b) const {
                return _wcsicmp(a.c_str(), b.c_str()) < 0;
            }
        };
        std::map<std::wstring, std::wstring, Less> variables;
        if (auto inherited = GetEnvironmentStringsW()) {
            for (auto p = inherited; *p; p += wcslen(p) + 1) {
                std::wstring row(p);
                const auto at = row.find(L'=', row[0] == L'=' ? 1 : 0);
                if (at != std::wstring::npos)
                    variables[row.substr(0, at)] = row.substr(at + 1);
            }
            FreeEnvironmentStringsW(inherited);
        }
        for (const auto &[key, value] : environment) {
            if (key.empty() || key.find('=') != std::string::npos || key.find('\0') != std::string::npos ||
                value.find('\0') != std::string::npos)
                throw std::invalid_argument("invalid child environment");
            variables[utf8_path(key).native()] = utf8_path(value).native();
        }
        for (const auto &key : removed_environment)
            variables.erase(utf8_path(key).native());
        std::wstring block;
        for (const auto &[key, value] : variables) {
            block += key + L"=" + value;
            block += L'\0';
        }
        block += L'\0';
        const bool created =
            CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE,
                           CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT |
                               CREATE_UNICODE_ENVIRONMENT,
                           block.data(), cwd.empty() ? nullptr : cwd.c_str(), &si.StartupInfo, &pi) != FALSE;
        DeleteProcThreadAttributeList(attributes);
        attributes = nullptr;
        if (!created)
            throw std::runtime_error("cannot start engine executable");
        impl_->process = pi.hProcess;
        if (!AssignProcessToJobObject(impl_->job, pi.hProcess) || ResumeThread(pi.hThread) == DWORD(-1))
            throw std::runtime_error("cannot contain/start engine process");
        release(pi.hThread);
        release(child_in);
        release(child_out);
        release(err);
    } catch (...) {
        if (attributes)
            DeleteProcThreadAttributeList(attributes);
        if (pi.hProcess)
            TerminateProcess(pi.hProcess, 2);
        release(pi.hThread);
        release(child_in);
        release(child_out);
        release(err);
        close();
        throw;
    }
#else
    int in[2]{-1, -1}, out[2]{-1, -1};
    if (pipe(in) || pipe(out)) {
        for (auto fd : {in[0], in[1], out[0], out[1]})
            if (fd >= 0)
                ::close(fd);
        throw std::runtime_error("cannot create engine pipes");
    }
    const auto parent = getpid();
    impl_->process = fork();
    if (impl_->process == 0) {
        setpgid(0, 0);
#ifdef __linux__
        prctl(PR_SET_PDEATHSIG, SIGKILL);
        if (getppid() != parent)
            _exit(127);
#endif
        const int err = open(log.empty() ? "/dev/null" : log.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
        if (err < 0 || dup2(in[0], STDIN_FILENO) < 0 || dup2(out[1], STDOUT_FILENO) < 0 ||
            dup2(err, STDERR_FILENO) < 0)
            _exit(127);
        for (auto fd : {in[0], in[1], out[0], out[1], err})
            if (fd > 2)
                ::close(fd);
        if (!cwd.empty() && chdir(cwd.c_str()))
            _exit(127);
        for (const auto &[key, value] : environment)
            if (setenv(key.c_str(), value.c_str(), 1))
                _exit(127);
        for (const auto &key : removed_environment)
            unsetenv(key.c_str());
        std::vector<char *> args;
        for (const auto &s : argv)
            args.push_back(const_cast<char *>(s.c_str()));
        args.push_back(nullptr);
        execvp(args[0], args.data());
        _exit(127);
    }
    ::close(in[0]);
    ::close(out[1]);
    impl_->input = in[1];
    impl_->output = out[0];
    if (impl_->process < 0) {
        close();
        throw std::runtime_error("cannot start engine");
    }
    setpgid(impl_->process, impl_->process);
    fcntl(impl_->input, F_SETFD, FD_CLOEXEC);
    fcntl(impl_->output, F_SETFD, FD_CLOEXEC);
    fcntl(impl_->input, F_SETFL, fcntl(impl_->input, F_GETFL) | O_NONBLOCK);
#endif
    if (console) {
        try {
            impl_->console = std::thread([this, log, log_start] {
                // Read only newly appended stderr. stdout remains exclusively
                // the engine protocol; the child still writes directly to its
                // original log, so console output cannot stall inference.
                std::ifstream file(log, std::ios::binary);
                if (!file) {
                    std::fputs("cannot mirror engine log to console\n", stderr);
                    return;
                }
                file.seekg(static_cast<std::streamoff>(log_start));
                for (;;) {
                    const bool finishing = impl_->stop_console;
                    char bytes[4096];
                    file.read(bytes, sizeof(bytes));
                    const auto count = file.gcount();
                    if (count) {
                        std::fwrite(bytes, 1, static_cast<size_t>(count), stderr);
                        std::fflush(stderr);
                    }
                    if (file.bad())
                        return;
                    if (!file.eof())
                        continue;
                    file.clear();
                    if (finishing)
                        return;
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            });
        } catch (...) {
            close();
            throw;
        }
    }
}
EngineProcess::~EngineProcess() { close(); }
void EngineProcess::send(const std::string &line) {
    const auto data = line + "\n";
#ifdef _WIN32
    // A child that reports READY but stops reading must not hold the service's
    // FIFO forever. Cancel the synchronous writer and contain the entire job.
    std::packaged_task<void()> write([&] {
#endif
        size_t offset = 0;
#ifndef _WIN32
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
#endif
        while (offset < data.size()) {
#ifdef _WIN32
            DWORD written = 0;
            if (!WriteFile(impl_->input, data.data() + offset,
                           static_cast<DWORD>(std::min<size_t>(data.size() - offset, 65536)), &written,
                           nullptr) ||
                !written)
                throw std::runtime_error("engine input closed");
#else
        const auto written = write(impl_->input, data.data() + offset, data.size() - offset);
        if (written < 0 && errno == EINTR)
            continue;
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (std::chrono::steady_clock::now() >= deadline)
                throw std::runtime_error("engine input write deadline exceeded");
            pollfd pipe{impl_->input, POLLOUT, 0};
            poll(&pipe, 1, 100);
            continue;
        }
        if (written <= 0)
            throw std::runtime_error("engine input closed");
#endif
            offset += static_cast<size_t>(written);
        }
#ifdef _WIN32
    });
    auto result = write.get_future();
    std::thread writer(std::move(write));
    const bool timed_out = result.wait_for(std::chrono::seconds(15)) != std::future_status::ready;
    if (timed_out) {
        CancelSynchronousIo(writer.native_handle());
        TerminateJobObject(impl_->job, 2);
    }
    writer.join();
    if (timed_out)
        throw std::runtime_error("engine input write deadline exceeded");
    result.get();
#endif
}
bool EngineProcess::receive(std::string &line, std::chrono::milliseconds timeout) {
    const auto end = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        const auto at = impl_->buffered.find('\n');
        if (at != std::string::npos) {
            line = impl_->buffered.substr(0, at);
            impl_->buffered.erase(0, at + 1);
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            return true;
        }
        char bytes[4096];
#ifdef _WIN32
        DWORD available = 0, count = 0;
        if (!PeekNamedPipe(impl_->output, nullptr, 0, nullptr, &available, nullptr))
            throw std::runtime_error("engine output closed");
        if (available) {
            if (!ReadFile(impl_->output, bytes, std::min<DWORD>(available, sizeof(bytes)), &count, nullptr) ||
                !count)
                throw std::runtime_error("engine output closed");
        } else {
            if (WaitForSingleObject(impl_->process, 0) == WAIT_OBJECT_0)
                throw std::runtime_error("engine exited");
            if (std::chrono::steady_clock::now() >= end)
                return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
#else
        pollfd p{impl_->output, POLLIN, 0};
        const auto left =
            std::chrono::duration_cast<std::chrono::milliseconds>(end - std::chrono::steady_clock::now())
                .count();
        const auto ready = poll(&p, 1, static_cast<int>(std::max<int64_t>(0, left)));
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready < 0)
            throw std::runtime_error("cannot poll engine");
        if (!ready)
            return false;
        const auto count = read(impl_->output, bytes, sizeof(bytes));
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            throw std::runtime_error("engine output closed");
#endif
        impl_->buffered.append(bytes, static_cast<size_t>(count));
        if (impl_->buffered.size() > impl_->maximum_line)
            throw std::runtime_error("engine protocol line exceeds limit");
    }
}
bool EngineProcess::alive() const {
#ifdef _WIN32
    if (!impl_->process || WaitForSingleObject(impl_->process, 0) != WAIT_TIMEOUT) {
        DWORD code = 2;
        if (impl_->process)
            GetExitCodeProcess(impl_->process, &code);
        impl_->result = static_cast<int>(code);
        return false;
    }
    return true;
#else
    if (impl_->process <= 0 || impl_->result != -1)
        return false;
    int status = 0;
    const auto pid = waitpid(impl_->process, &status, WNOHANG);
    if (pid == impl_->process) {
        impl_->result = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
        return false;
    }
    return pid == 0;
#endif
}
int EngineProcess::exit_code() const {
    if (alive())
        return -1;
    return impl_->result;
}
std::optional<std::pair<double, uint64_t>> EngineProcess::activity() const {
#ifdef _WIN32
    FILETIME born{}, ended{}, kernel{}, user{};
    IO_COUNTERS io{};
    if (!impl_->process || !GetProcessTimes(impl_->process, &born, &ended, &kernel, &user) ||
        !GetProcessIoCounters(impl_->process, &io))
        return {};
    const auto count = [](const FILETIME &t) { return (uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
    return std::pair{(count(kernel) + count(user)) / 10000000.,
                     uint64_t(io.ReadTransferCount + io.WriteTransferCount)};
#else
    if (impl_->process <= 0)
        return {};
    const auto root = std::string("/proc/") + std::to_string(impl_->process);
    std::ifstream stat(root + "/stat"), io(root + "/io");
    std::string line;
    if (!std::getline(stat, line))
        return {};
    const auto end = line.rfind(')');
    if (end == std::string::npos)
        return {};
    std::istringstream fields(line.substr(end + 2));
    std::string value;
    uint64_t ticks = 0;
    for (int i = 0; i <= 12; ++i) {
        if (!(fields >> value))
            return {};
        if (i == 11 || i == 12)
            ticks += std::stoull(value);
    }
    uint64_t bytes = 0, n = 0;
    std::string key;
    while (io >> key >> n)
        if (key == "read_bytes:" || key == "write_bytes:")
            bytes += n;
    const auto rate = sysconf(_SC_CLK_TCK);
    if (rate <= 0)
        return {};
    return std::pair{ticks / double(rate), bytes};
#endif
}
void EngineProcess::finish() noexcept {
    try {
        send("QUIT");
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < end) {
            std::string ignored;
            receive(ignored, std::chrono::milliseconds(100));
        }
    } catch (...) { /* EOF is the expected graceful exit; close also contains descendants. */
    }
    close();
}
void EngineProcess::close() noexcept {
    if (!impl_)
        return;
#ifdef _WIN32
    if (impl_->job)
        TerminateJobObject(impl_->job, 2);
    if (impl_->process)
        WaitForSingleObject(impl_->process, 5000);
    release(impl_->input);
    release(impl_->output);
    release(impl_->process);
    release(impl_->job);
#else
    if (impl_->process > 0) {
        kill(-impl_->process, SIGKILL);
        while (waitpid(impl_->process, nullptr, 0) < 0 && errno == EINTR) {
        }
        impl_->process = -1;
    }
    if (impl_->input >= 0)
        ::close(impl_->input);
    if (impl_->output >= 0)
        ::close(impl_->output);
    impl_->input = impl_->output = -1;
#endif
    // Terminate the writer first, then drain its final diagnostics before
    // releasing the mirror. Repeated close() calls remain harmless.
    impl_->stop_console = true;
    if (impl_->console.joinable())
        impl_->console.join();
}
} // namespace strata::runtime
