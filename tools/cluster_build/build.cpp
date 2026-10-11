#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <unistd.h>
#endif
#include "build.hpp"
#include "strata/runtime/cluster_launch.hpp"
extern "C" {
#include "sha256.h"
}
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <ctime>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <random>
#include <regex>
#include <stdexcept>
#include <thread>

namespace strata::build {
namespace {
class BuildCancelled : public std::runtime_error {
public:
    BuildCancelled() : std::runtime_error("cluster build cancelled; no package published for this node") {}
};
#ifdef _WIN32
std::atomic<bool> batch_stopped{false};
BOOL WINAPI stop_batch(DWORD event) {
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT || event == CTRL_CLOSE_EVENT || event == CTRL_SHUTDOWN_EVENT) {
        batch_stopped = true; return TRUE;
    }
    return FALSE;
}
int windows_batch(const fs::path& executable, const fs::path& script, const std::string& host) {
    // cmd's /s /c grammar is different from CRT argv quoting. The script and
    // host are fixed by the builder; option values travel in the environment.
    if (path_utf8(script).find_first_of("\"%\r\n") != std::string::npos ||
        (host != "host16" && host != "host11" && host != "host17")) throw std::runtime_error("invalid batch entry");
    std::wstring line = L"cmd.exe /d /s /c \"\"" + script.native() + L"\" " + utf8_path(host).native() + L"\"";
    struct Handle { HANDLE value; ~Handle() { if (value) CloseHandle(value); } } job{CreateJobObjectW(nullptr, nullptr)};
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{}; limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job.value || !SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) throw std::runtime_error("cannot contain build processes");
    batch_stopped = false;
    if (!SetConsoleCtrlHandler(stop_batch, TRUE)) throw std::runtime_error("cannot install build stop handler");
    struct Handler { ~Handler() { SetConsoleCtrlHandler(stop_batch, FALSE); } } handler;
    STARTUPINFOW startup{}; startup.cb = sizeof(startup); startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE); startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE); startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), line.data(), nullptr, nullptr, TRUE, CREATE_SUSPENDED | CREATE_NEW_PROCESS_GROUP,
                        nullptr, nullptr, &startup, &process)) throw std::runtime_error("cannot start build batch entry");
    Handle child{process.hProcess}, thread{process.hThread};
    if (!AssignProcessToJobObject(job.value, child.value) || ResumeThread(thread.value) == DWORD(-1)) {
        TerminateProcess(child.value, 125); WaitForSingleObject(child.value, INFINITE); throw std::runtime_error("cannot contain/resume build batch entry");
    }
    bool stopping = false;
    auto deadline = std::chrono::steady_clock::time_point::max();
    for (;;) {
        const auto status = WaitForSingleObject(child.value, 100);
        if (status == WAIT_OBJECT_0) break;
        if (status != WAIT_TIMEOUT) throw std::runtime_error("cannot wait for build process");
        if (batch_stopped && !stopping) {
            stopping = true; GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, process.dwProcessId);
            deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        }
        if (stopping && std::chrono::steady_clock::now() >= deadline) TerminateJobObject(job.value, 130);
    }
    DWORD code;
    if (!GetExitCodeProcess(child.value, &code)) throw std::runtime_error("cannot read build exit status");
    if (!TerminateJobObject(job.value, code)) throw std::runtime_error("cannot stop build descendants");
    const auto cleanup = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    for (;;) {
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
        if (!QueryInformationJobObject(job.value, JobObjectBasicAccountingInformation, &accounting, sizeof(accounting), nullptr)) throw std::runtime_error("cannot check build descendants");
        if (!accounting.ActiveProcesses) break;
        if (std::chrono::steady_clock::now() >= cleanup) throw std::runtime_error("build descendants did not stop");
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return batch_stopped ? 130 : static_cast<int>(code);
}
#endif
std::string token() {
    std::random_device random;
    return std::to_string(std::chrono::system_clock::now().time_since_epoch().count()) + "-" + std::to_string(random());
}
std::string utc_now() {
    const auto time = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &time);
#else
    gmtime_r(&time, &utc);
#endif
    std::array<char, 32> text{};
    std::strftime(text.data(), text.size(), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return text.data();
}
std::string get(const Cache& cache, const std::string& key) {
    auto found = cache.find(key); return found == cache.end() ? "" : found->second;
}
std::vector<std::string> split(const std::string& value, char separator) {
    std::vector<std::string> result;
    size_t start = 0;
    for (;;) {
        auto end = value.find(separator, start);
        if (end != start) result.push_back(value.substr(start, end == std::string::npos ? end : end - start));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return result;
}
void text_file(const fs::path& file, const std::string& text) {
    fs::create_directories(file.parent_path());
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.exceptions(std::ios::badbit | std::ios::failbit);
    out << text; out.close();
}
void replace_file(const fs::path& source, const fs::path& target) {
#ifdef _WIN32
    if (!MoveFileExW(source.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("cannot atomically replace " + path_utf8(target));
#else
    fs::rename(source, target);
#endif
}
bool redirected(const fs::path& path) {
    std::error_code error;
    if (fs::is_symlink(fs::symlink_status(path, error))) return true;
#ifdef _WIN32
    const auto attrs = GetFileAttributesW(path.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES && attrs & FILE_ATTRIBUTE_REPARSE_POINT) return true;
#endif
    return false;
}
void direct_child(const fs::path& path) {
    if (redirected(path) || resolved(path).parent_path() != resolved(path.parent_path()))
        throw std::runtime_error("output directory must not redirect elsewhere: " + path_utf8(path));
}
class Stage {
    fs::path parent;
public:
    fs::path path;
    explicit Stage(const fs::path& output) : parent(resolved(output)), path(parent / (".cluster-stage-" + token())) {
        if (!fs::create_directory(path)) throw std::runtime_error("cannot create unique staging directory");
    }
    ~Stage() {
        // Never traverse an externally redirected staging directory during cleanup.
        try {
            std::error_code error;
            if (!redirected(path) && resolved(path).parent_path() == parent) fs::remove_all(path, error);
        } catch (...) {} // Cleanup must not hide the original build failure.
    }
};
class Lock {
    fs::path path;
public:
    Lock(const fs::path& output, const std::string& host) : path(output / (".cluster-lock-" + host)) {
        if (!fs::create_directory(path)) throw std::runtime_error("node build locked; check for another builder or a stale lock: " + path_utf8(path));
    }
    ~Lock() { std::error_code error; fs::remove(path, error); }
};
void environment(const std::string& name, const std::string& value) {
#ifdef _WIN32
    if (!SetEnvironmentVariableW(utf8_path(name).c_str(), value.empty() ? nullptr : utf8_path(value).c_str()))
        throw std::runtime_error("cannot set build environment variable " + name);
#else
    if (value.empty() ? unsetenv(name.c_str()) : setenv(name.c_str(), value.c_str(), 1))
        throw std::runtime_error("cannot set build environment variable " + name);
#endif
}
class Environment {
    std::map<std::string, std::string> saved;
public:
    void set(const std::string& name, const std::string& value) {
        saved.try_emplace(name, runtime::environment_utf8(name.c_str())); environment(name, value);
    }
    ~Environment() { for (const auto& [name, value] : saved) { try { environment(name, value); } catch (...) {} } }
};
fs::path tool(const std::string& name) {
#ifdef _WIN32
    const auto suffix = ".exe";
    const auto separator = ';';
#else
    const auto suffix = "";
    const auto separator = ':';
#endif
    for (const auto& item : split(runtime::environment_utf8("PATH"), separator)) {
        auto file = utf8_path(item) / (name + suffix);
        if (fs::is_regular_file(file)) return resolved(file);
    }
    throw std::runtime_error("required tool not found on PATH: " + name);
}
std::vector<std::string> programs(const std::string& host) {
#ifdef _WIN32
    std::vector<std::string> names{"strata-host.exe", "strata_network_backend.dll"};
    if (host != "host11") names.insert(names.begin(), "strata.exe");
#else
    std::vector<std::string> names{"strata-host", "libstrata_network_backend.so"};
    if (host != "host11") names.insert(names.begin(), "strata");
#endif
    return names;
}
std::vector<fs::path> runtime_dirs(const Options& options, const Cache& cache, const fs::path& build) {
    std::vector<fs::path> dirs{build};
    dirs.insert(dirs.end(), options.runtime_dirs.begin(), options.runtime_dirs.end());
    for (const auto& key : {"CUDAToolkit_BIN_DIR", "CUDAToolkit_LIBRARY_DIR", "CUDAToolkit_LIBRARY_ROOT", "CMAKE_HIP_COMPILER_ROCM_ROOT"}) {
        if (get(cache, key).empty()) continue;
        auto dir = utf8_path(get(cache, key));
        for (const auto& sub : {"", "x64", "bin", "lib", "lib64", "targets/x86_64-linux/lib"}) dirs.push_back(dir / sub);
    }
    if (!options.rocm.empty()) { dirs.push_back(options.rocm / "bin"); dirs.push_back(options.rocm / "lib"); }
    for (const auto& key : {"CMAKE_CXX_COMPILER", "CMAKE_CUDA_COMPILER"}) {
        auto dir = utf8_path(get(cache, key)).parent_path();
        for (int i = 0; i < 4 && !dir.empty(); ++i, dir = dir.parent_path())
            for (const auto& sub : {"", "bin", "bin/x64", "lib", "lib64", "bin/compiler", "redist/intel64_win/compiler"}) dirs.push_back(dir / sub);
    }
#ifdef _WIN32
    auto add_redist = [&](const fs::path& base) {
        if (!fs::is_directory(base)) return;
        std::vector<fs::path> versions;
        for (const auto& entry : fs::directory_iterator(base)) if (entry.is_directory()) versions.push_back(entry.path());
        std::sort(versions.rbegin(), versions.rend());
        for (const auto& version : versions) {
            auto x64 = version / "x64";
            if (fs::is_directory(x64)) for (const auto& entry : fs::directory_iterator(x64))
                if (entry.is_directory() && path_utf8(entry.path().filename()).starts_with("Microsoft.VC")) dirs.push_back(entry.path());
        }
    };
    auto explicit_redist = utf8_path(runtime::environment_utf8("VCToolsRedistDir")) / "x64";
    if (fs::is_directory(explicit_redist)) for (const auto& entry : fs::directory_iterator(explicit_redist))
        if (entry.is_directory()) dirs.push_back(entry.path());
    // The cached linker identifies the installation without running vswhere.
    auto dir = utf8_path(get(cache, "CMAKE_LINKER")).parent_path();
    for (int i = 0; i < 8 && !dir.empty(); ++i, dir = dir.parent_path()) add_redist(dir / "Redist/MSVC");
    const auto vs = utf8_path(runtime::environment_utf8("ProgramFiles")) / "Microsoft Visual Studio";
    if (fs::is_directory(vs)) for (const auto& version : fs::directory_iterator(vs))
        if (version.is_directory()) for (const auto& edition : fs::directory_iterator(version))
            if (edition.is_directory()) add_redist(edition.path() / "VC/Redist/MSVC");
    for (const auto& item : split(runtime::environment_utf8("PATH"), ';')) dirs.push_back(utf8_path(item));
#else
    for (const auto& name : {"/lib/x86_64-linux-gnu", "/usr/lib/x86_64-linux-gnu", "/lib64", "/usr/lib64", "/usr/lib", "/lib"}) dirs.emplace_back(name);
    for (const auto& item : split(runtime::environment_utf8("LD_LIBRARY_PATH"), ':')) dirs.push_back(utf8_path(item));
#endif
    return dirs;
}
void resources(const Options& options, const fs::path& build, const fs::path& stage, const std::string& host) {
    copy_tree(options.root / "serve/web", stage / "web");
    fs::copy_file(options.root / "serve/chat_template.jinja", stage / "chat_template.jinja");
    copy_tree(build / "native-text-licenses", stage / "licenses/native-host");
    fs::copy_file(options.root / "LICENSE", stage / "licenses/Strata-LICENSE.txt");
    fs::create_directory(stage / "configs");
    for (const auto& name : {"cluster-2hosts.json", "cluster-3hosts.json", "cluster-4hosts.example.json"})
        fs::copy_file(options.root / "configs" / name, stage / "configs" / name);
#ifdef _WIN32
    const std::string entry = "strata-host.exe";
#else
    const std::string entry = "./strata-host";
#endif
    text_file(stage / "RUN.txt", "Native host package; models and drivers are not included. Python is not required.\n"
        "Edit model/library/engine paths in a common cluster.json; copy the same plan to every node.\n"
        "Keep STRATA_NETWORK_KEY in the environment. Non-loopback API requires STRATA_API_KEY.\n"
        "Run: " + entry + " cluster --config cluster.json --node " + host + "\n"
        "The coordinator also needs its server config, pack and tokenizer.\n"
        "For images, configure vision.mmproj and vision.model; built-in vision uses CPU ggml.\n"
        "HIP packages retain rocm/bin and device data; coordinator lib_dirs must include rocm/bin.\n"
        "Publication does not stop running services. The previous package remains in *.previous-*.\n");
}
void runtime_licenses(const Cache& cache, const fs::path& stage) {
    if (fs::is_regular_file(stage / "libmmd.dll") || fs::is_regular_file(stage / "svml_dispmd.dll")) {
        auto dir = utf8_path(get(cache, "CMAKE_CXX_COMPILER")).parent_path();
        bool found = false;
        for (int i = 0; i < 4 && !dir.empty(); ++i, dir = dir.parent_path()) {
            const auto license = dir / "share/doc/compiler/licensing/c";
            if (fs::is_regular_file(license / "LICENSE.rtf")) {
                copy_tree(license, stage / "licenses/intel-compiler"); found = true; break;
            }
        }
        if (!found) throw std::runtime_error("Intel runtime license is missing from the cached compiler installation");
    }
    if (get(cache, "STRATA_ENABLE_CUDA") == "ON") {
        const auto cuda = utf8_path(get(cache, "CMAKE_CUDA_COMPILER")).parent_path().parent_path();
        fs::create_directories(stage / "licenses/cuda");
        for (const auto& name : {"EULA.txt", "LICENSE"}) {
            if (!fs::is_regular_file(cuda / name)) throw std::runtime_error("CUDA runtime license missing: " + path_utf8(cuda / name));
            fs::copy_file(cuda / name, stage / "licenses/cuda" / name);
        }
    }
    text_file(stage / "licenses/RUNTIME-NOTICE.txt", "GPU drivers and Windows system DLLs are supplied by the target system.\n"
        "Microsoft Visual C++ runtime files are redistributable under the Visual Studio Distributable Code license.\n"
        "Intel, CUDA and ROCm runtime terms are included when those components are shipped.\n");
}
void rocm_resources(const Options& options, const Cache& cache, const fs::path& stage, const std::vector<fs::path>& roots,
                    const std::vector<fs::path>& dirs) {
    const auto rocm = options.rocm.empty() ? utf8_path(get(cache, "CMAKE_HIP_COMPILER_ROCM_ROOT")) : options.rocm;
    if (!fs::is_directory(rocm)) throw std::runtime_error("HIP packaging requires --rocm-root pointing to an unpacked SDK");
    auto bin = rocm / "bin";
#ifndef _WIN32
    bin = rocm / "lib";
#endif
    const auto target = stage / "rocm/bin";
    fs::create_directories(target);
    auto search = dirs; search.insert(search.begin(), bin);
    std::vector<fs::path> dynamic = roots;
    bool comgr = false;
    for (const auto& entry : fs::directory_iterator(bin)) {
        const auto name = path_utf8(entry.path().filename());
        if (lower(name).starts_with("amd_comgr") && lower(name).ends_with(".dll")) {
            fs::copy_file(entry.path(), target / entry.path().filename()); dynamic.push_back(target / entry.path().filename()); comgr = true;
        }
#ifndef _WIN32
        if (name == "libamd_comgr.so.2") {
            fs::copy_file(entry.path(), target / entry.path().filename()); dynamic.push_back(target / entry.path().filename()); comgr = true;
        }
#endif
    }
    if (!comgr) throw std::runtime_error("HIP SDK is missing its dynamically loaded amd_comgr runtime");
    copy_runtime(target, dynamic, search);
    // Windows resolves HIP before the display driver's System32 copy. Put the
    // import closure beside the EXEs as well; BLAS stays beside its device data.
    std::vector<fs::path> hip_roots;
    for (const auto& entry : fs::directory_iterator(target)) {
        const auto name = lower(path_utf8(entry.path().filename()));
        if (name.starts_with("amdhip64_") || name.starts_with("amd_comgr") || name.starts_with("libamdhip64") || name.starts_with("libamd_comgr")) {
            fs::copy_file(entry.path(), stage / entry.path().filename()); hip_roots.push_back(stage / entry.path().filename());
        }
    }
    copy_runtime(stage, hip_roots, {target});
    // Import closure of the host and backend is required in their own directory.
    // ROCm libraries whose relative data paths matter remain in rocm/bin too.
    std::vector<fs::path> host_runtime;
    for (const auto& root : roots) for (const auto& name : imports(root)) {
        const auto file = utf8_path(name);
        if (!fs::is_regular_file(stage / file) && fs::is_regular_file(target / file) && !fs::is_regular_file(bin / file)) {
            fs::copy_file(target / file, stage / file); host_runtime.push_back(stage / file);
        }
    }
    copy_runtime(stage, host_runtime, {target});
    for (const auto& library : {"rocblas", "hipblaslt"}) {
        auto source = bin / library / "library";
        if (!fs::is_directory(source)) {
            if (std::string(library) == "rocblas") throw std::runtime_error("HIP SDK is missing rocBLAS device data");
            continue;
        }
        auto out = target / library / "library"; fs::create_directories(out);
        const std::regex arch("gfx[0-9a-f]+");
        for (const auto& entry : fs::directory_iterator(source)) {
            const auto name = path_utf8(entry.path().filename()); std::smatch match;
            if (std::regex_search(name, match, arch) && std::find(options.archs.begin(), options.archs.end(), match.str()) == options.archs.end()) continue;
            if (entry.is_directory()) copy_tree(entry.path(), out / entry.path().filename());
            else fs::copy_file(entry.path(), out / entry.path().filename());
        }
    }
    // TheRock uses .kpack; traditional Linux ROCm uses the library directories above.
    if (fs::is_directory(rocm / ".kpack")) {
        fs::create_directory(stage / "rocm/.kpack");
        for (const auto& arch : options.archs) {
            auto source = rocm / ".kpack" / ("blas_lib_" + arch + ".kpack");
            if (!fs::is_regular_file(source)) throw std::runtime_error("missing ROCm device package for " + arch);
            fs::copy_file(source, stage / "rocm/.kpack" / source.filename());
        }
    }
#ifdef _WIN32
    else throw std::runtime_error("HIP SDK is missing .kpack device packages");
#endif
    auto licenses = stage / "rocm/licenses"; fs::create_directory(licenses);
    for (const auto& component : {"rocblas", "rocsolver", "hipblas", "hipblas-common", "hipblaslt", "amd_comgr", "rocm-core"}) {
        bool found = false;
        const auto doc = rocm / "share/doc" / component;
        if (fs::is_directory(doc)) for (const auto& entry : fs::directory_iterator(doc)) {
            if (entry.is_regular_file() && lower(path_utf8(entry.path().filename())).starts_with("license")) {
                fs::copy_file(entry.path(), licenses / (std::string(component) + "-" + path_utf8(entry.path().filename()))); found = true;
            }
        }
        if (!found && std::string(component) != "rocsolver") throw std::runtime_error("missing ROCm license: " + std::string(component));
    }
    text_file(licenses / "NOTICE.txt", "Unmodified AMD ROCm components from the supplied SDK.\n"
        "Source: https://github.com/ROCm/TheRock and https://github.com/ROCm/rocm-systems\n"
        "rocBLAS/hipBLAS/hipBLAS-common/hipBLASLt/rocm-core: MIT; rocSOLVER: BSD 2-clause;\n"
        "amd_comgr: Apache 2.0 with LLVM exceptions. See the accompanying licenses.\n"
        "HIP runtime and rocm_kpack: MIT; TheRock wheels do not carry their license files.\n"
        "Microsoft C++ runtime is redistributable under the Visual Studio Distributable Code license.\n");
}
void build_node(const Options& options, const std::string& host, const fs::path& build, const fs::path& log) {
    Environment env;
    fs::create_directories(log.parent_path());
    Stage compiler_temp(log.parent_path());
    env.set("TEMP", path_utf8(compiler_temp.path));
    env.set("TMP", path_utf8(compiler_temp.path));
    for (const auto& name : {"CFLAGS", "CXXFLAGS", "CUDAFLAGS", "HIPFLAGS", "STRATA_CLUSTER_BUILD_DIR", "BUILD_DIR", "DIST_DIR"}) env.set(name, "");
    env.set("STRATA_BUILD_JOBS", std::to_string(options.jobs));
    env.set("STRATA_CLUSTER_NATIVE", "ON");
    env.set("STRATA_GGML_DIR", path_utf8(options.ggml));
    for (const auto& [name, path] : options.dependency_dirs) env.set(name, path_utf8(path));
#ifdef _WIN32
    env.set("STRATA_CLUSTER_BUILD_DIR", path_utf8(build));
    env.set("STRATA_ROCM_ROOT", path_utf8(options.rocm));
    env.set("STRATA_ROCM_COMPILER_ROOT", path_utf8(options.rocm_compiler));
    std::string archs;
    for (const auto& arch : options.archs) { if (!archs.empty()) archs += ';'; archs += arch; }
    env.set("STRATA_HIP_ARCHS", archs);
    const auto script = options.root / "tools" / (host == "host17" ? "build_cluster_hip_native.bat" : "build_cluster_intel.bat");
    // cmd.exe receives only this fixed repository script and the fixed host ID.
    // User values are passed as environment entries, never as shell fragments.
    const auto spelling = path_utf8(script);
    if (spelling.find_first_of("\"%\r\n") != std::string::npos) throw std::runtime_error("Windows batch entry cannot use quote, percent or newline in repository path");
    const auto result = command({path_utf8(tool("cmd")), "/d", "/c", "call", spelling, host}, options.root, log);
#else
    throw std::runtime_error("Linux build migration is pending");
    const int result = 1;
#endif
    if (result == 130) throw BuildCancelled();
    if (result) throw std::runtime_error(host + ": build exited " + std::to_string(result) + "; see " + path_utf8(log));
}
void package(const Options& options, const std::string& host, const fs::path& build, const fs::path& stage) {
    auto cache = cache_values(build); verify_profile(host, cache, options.archs);
    fs::create_directory(stage);
    const auto names = programs(host); std::vector<fs::path> roots;
    for (const auto& name : names) {
        auto file = build / utf8_path(name);
        if (!fs::is_regular_file(file)) throw std::runtime_error("required compiled file is missing: " + path_utf8(file));
        fs::copy_file(file, stage / utf8_path(name)); roots.push_back(stage / utf8_path(name));
    }
    auto dirs = runtime_dirs(options, cache, build);
    if (host == "host17") rocm_resources(options, cache, stage, roots, dirs);
    else copy_runtime(stage, roots, dirs);
    resources(options, build, stage, host);
    runtime_licenses(cache, stage);
    verify_package(stage, names);
    Json hashes = Json::object(), files = Json::object();
    for (const auto& name : names) hashes[name] = file_sha256(stage / utf8_path(name));
    for (const auto& entry : fs::recursive_directory_iterator(stage)) if (entry.is_regular_file()) {
        const auto relative = entry.path().lexically_relative(stage).generic_u8string();
        files[std::string(relative.begin(), relative.end())] = file_sha256(entry.path());
    }
    Json meta{{"node", host}, {"built_at_utc", utc_now()},
        {"build_directory", path_utf8(build)}, {"compiler", get(cache, "CMAKE_CXX_COMPILER")}, {"c_release_flags", get(cache, "CMAKE_C_FLAGS_RELEASE")},
        {"cxx_release_flags", get(cache, "CMAKE_CXX_FLAGS_RELEASE")}, {"portable", get(cache, "STRATA_PORTABLE") == "ON"},
        {"backend", host == "host17" ? "hip" : host == "host16" ? "cuda" : "cpu"}, {"hip_archs", host == "host17" ? options.archs : std::vector<std::string>{}},
        {"native_host", true}, {"programs", hashes}, {"files", files}};
    atomic_json(stage / "CLUSTER_BUILD.json", meta);
}
} // namespace
std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; });
    return value;
}
fs::path resolved(const fs::path& path) { return fs::weakly_canonical(fs::absolute(path)); }
bool contains(const fs::path& parent, const fs::path& child) {
    const auto p = resolved(parent), c = resolved(child);
    auto pi = p.begin(), ci = c.begin();
    for (; pi != p.end(); ++pi, ++ci) {
        if (ci == c.end()) return false;
#ifdef _WIN32
        if (lower(path_utf8(*pi)) != lower(path_utf8(*ci))) return false;
#else
        if (*pi != *ci) return false;
#endif
    }
    return true;
}
Cache cache_values(const fs::path& build) {
    std::ifstream input(build / "CMakeCache.txt");
    if (!input) throw std::runtime_error("CMake cache is missing: " + path_utf8(build));
    Cache cache; std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto colon = line.find(':'), equal = line.find('=');
        if (!line.empty() && line[0] != '#' && !line.starts_with("//") && colon != std::string::npos && equal != std::string::npos && colon < equal)
            cache[line.substr(0, colon)] = line.substr(equal + 1);
    }
    return cache;
}
void verify_profile(const std::string& host, const Cache& cache, const std::vector<std::string>& archs) {
    if (get(cache, "CMAKE_BUILD_TYPE") != "Release") throw std::runtime_error(host + ": requires a Release build");
    for (const auto& [name, expected] : std::vector<std::pair<std::string, std::string>>{
        {"STRATA_ENABLE_CUDA", host == "host16" ? "ON" : "OFF"}, {"STRATA_ENABLE_HIP", host == "host17" ? "ON" : "OFF"},
        {"STRATA_PORTABLE", host == "host16" ? "OFF" : "ON"}, {"STRATA_BUILD_NATIVE_CLUSTER", "ON"}})
        if (get(cache, name) != expected) throw std::runtime_error(host + ": " + name + " must be " + expected);
    if (get(cache, "STRATA_ENABLE_SYCL") == "ON") throw std::runtime_error("SYCL has no cluster network backend");
    const auto compiler = lower(path_utf8(utf8_path(get(cache, "CMAKE_CXX_COMPILER")).filename()));
#ifdef _WIN32
    const bool intel = compiler == "icx-cl.exe";
#else
    const bool intel = compiler == "icpx" || compiler == "icx";
#endif
    if (host != "host17" && !intel) throw std::runtime_error(host + ": requires Intel oneAPI compiler");
    if (host == "host17" && compiler.find("clang") == std::string::npos) throw std::runtime_error("host17: requires ROCm Clang");
    std::string flags;
    for (const auto& name : {"CMAKE_C_FLAGS", "CMAKE_CXX_FLAGS", "CMAKE_C_FLAGS_RELEASE", "CMAKE_CXX_FLAGS_RELEASE"}) flags += " " + get(cache, name);
    static const std::regex native("(/Qx[^\\s]+|/arch:AVX(512|10)[^\\s]*|-m(avx512|avxvnni|amx)[^\\s]*|-march=[^\\s]+|-mcpu=[^\\s]+|-xHost|-x[A-Z][^\\s]*)", std::regex::icase);
    if (host != "host16" && std::regex_search(flags, native)) throw std::runtime_error(host + ": nonportable CPU option in cached flags");
    const auto low = lower(flags);
    if (host == "host16") {
#ifdef _WIN32
        if (low.find("/qxhost") == std::string::npos || low.find("/o3") == std::string::npos)
#else
        if (low.find("-xhost") == std::string::npos || low.find("-o3") == std::string::npos)
#endif
            throw std::runtime_error("host16: missing native /O3 /QxHost (Linux: -O3 -xHost) profile");
        if (get(cache, "CMAKE_CUDA_ARCHITECTURES") != "89") throw std::runtime_error("host16: expected CUDA sm_89");
    }
    if (host == "host11" && low.find("/o3") == std::string::npos && low.find("-o3") == std::string::npos) throw std::runtime_error("host11: expected O3");
    if (host != "host16") {
        if (get(cache, "GGML_NATIVE") != "OFF") throw std::runtime_error(host + ": GGML_NATIVE must be OFF");
        for (const auto& name : {"GGML_AVX512", "GGML_AVX512_VBMI", "GGML_AVX512_VNNI", "GGML_AVX512_BF16", "GGML_AVX_VNNI", "GGML_AMX_TILE", "GGML_AMX_INT8", "GGML_AMX_BF16"})
            if (get(cache, name) == "ON") throw std::runtime_error(host + ": ggml must retain its AVX2 baseline; " + name + " is ON");
    }
    if (host == "host17") {
        const auto actual = split(get(cache, "CMAKE_HIP_ARCHITECTURES"), ';');
        if (std::set<std::string>(actual.begin(), actual.end()) != std::set<std::string>(archs.begin(), archs.end()))
            throw std::runtime_error("host17: cached HIP targets differ from requested architectures");
    }
}
void copy_tree(const fs::path& source, const fs::path& target) {
    if (!fs::is_directory(source)) throw std::runtime_error("missing resource directory: " + path_utf8(source));
    if (contains(source, target)) throw std::runtime_error("resource copy would contain its own output");
    fs::create_directories(target);
    for (const auto& entry : fs::recursive_directory_iterator(source)) {
        if (entry.is_symlink() || redirected(entry.path())) throw std::runtime_error("resource tree contains redirected path: " + path_utf8(entry.path()));
        const auto out = target / entry.path().lexically_relative(source);
        if (entry.is_directory()) fs::create_directories(out);
        else if (entry.is_regular_file()) { fs::create_directories(out.parent_path()); fs::copy_file(entry.path(), out); }
        else throw std::runtime_error("resource tree contains a special file");
    }
}
std::string file_sha256(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) throw std::runtime_error("cannot hash file: " + path_utf8(file));
    sha256_t state; sha256_init(&state);
    std::array<char, 1 << 16> block{};
    while (in) { in.read(block.data(), block.size()); sha256_update(&state, reinterpret_cast<const unsigned char*>(block.data()), static_cast<size_t>(in.gcount())); }
    if (!in.eof()) throw std::runtime_error("failed reading file for SHA256");
    std::array<unsigned char, 32> digest{}; sha256_final(&state, digest.data());
    std::string result;
    for (auto c : digest) { result += "0123456789abcdef"[c >> 4]; result += "0123456789abcdef"[c & 15]; }
    return result;
}
void atomic_json(const fs::path& path, const Json& value) {
    auto temporary = path; temporary += ".tmp-" + token();
    try { text_file(temporary, value.dump(2) + "\n"); replace_file(temporary, path); }
    catch (...) { std::error_code error; fs::remove(temporary, error); throw; }
}
std::string publish(const fs::path& stage, const fs::path& destination) {
    direct_child(destination); direct_child(stage);
    if (resolved(stage).parent_path() != resolved(destination).parent_path() && !contains(destination.parent_path(), stage))
        throw std::runtime_error("staging must be on the output filesystem");
    fs::path backup;
    if (fs::exists(destination)) {
        const auto marker = destination / "CLUSTER_BUILD.json";
        if (!fs::is_regular_file(marker) || runtime::read_json(marker).value("node", "") != path_utf8(destination.filename()))
            throw std::runtime_error("refusing to replace directory not created for this node");
        backup = destination.parent_path() / (path_utf8(destination.filename()) + ".previous-" + token());
        fs::rename(destination, backup);
    }
    try { fs::rename(stage, destination); }
    catch (...) {
        if (!backup.empty()) {
            try { fs::rename(backup, destination); }
            catch (...) { throw std::runtime_error("publication and rollback failed; previous package retained at " + path_utf8(backup)); }
        }
        throw;
    }
    return backup.empty() ? "" : path_utf8(backup);
}
void verify_package(const fs::path& stage, const std::vector<std::string>& names) {
    std::set<std::string> expected, actual, libraries;
    for (const auto& name : names) if (lower(name).ends_with(".exe") || name == "strata" || name == "strata-host") expected.insert(lower(name));
    for (const auto& entry : fs::recursive_directory_iterator(stage)) {
        if (redirected(entry.path())) throw std::runtime_error("package contains redirected path");
        if (!entry.is_regular_file()) continue;
        const auto name = lower(path_utf8(entry.path().filename())), extension = lower(path_utf8(entry.path().extension()));
        if (extension == ".py" || extension == ".pyw" || extension == ".pyc" || extension == ".pyo" || extension == ".pyd" || name.starts_with("python") || name.starts_with("libpython"))
            throw std::runtime_error("native deployment must not contain or depend on Python");
        if (extension == ".exe" || name == "strata" || name == "strata-host") {
            actual.insert(name);
            if (entry.path().parent_path() != stage) throw std::runtime_error("nested executable in native deployment");
        }
        if ((name.starts_with("strata") && extension == ".dll") || name.starts_with("libstrata")) {
            if (entry.path().parent_path() != stage) throw std::runtime_error("nested project library in native deployment");
            libraries.insert(name);
        }
    }
#ifdef _WIN32
    const std::set<std::string> wanted{"strata_network_backend.dll"};
#else
    const std::set<std::string> wanted{"libstrata_network_backend.so"};
#endif
    if (actual != expected || expected.size() > 2 || libraries != wanted) throw std::runtime_error("native package must contain its exact programs, at most two executables and one project library");
}
int command(const std::vector<std::string>& args, const fs::path& cwd, const fs::path& log) {
    std::cout.flush(); std::cerr.flush();
    const auto previous = fs::current_path();
    struct Cwd { fs::path path; ~Cwd() { std::error_code error; fs::current_path(path, error); } } restore{previous};
    fs::current_path(cwd);
#ifdef _WIN32
    HANDLE handle = INVALID_HANDLE_VALUE;
    const auto old_out = GetStdHandle(STD_OUTPUT_HANDLE), old_err = GetStdHandle(STD_ERROR_HANDLE);
    struct Output { HANDLE handle, out, err; ~Output() { if (handle != INVALID_HANDLE_VALUE) { SetStdHandle(STD_OUTPUT_HANDLE, out); SetStdHandle(STD_ERROR_HANDLE, err); CloseHandle(handle); } } } output{handle, old_out, old_err};
    if (!log.empty()) {
        fs::create_directories(log.parent_path());
        SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        handle = CreateFileW(log.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot open build log");
        output.handle = handle;
        SetStdHandle(STD_OUTPUT_HANDLE, handle); SetStdHandle(STD_ERROR_HANDLE, handle);
    }
#else
    const int out = dup(STDOUT_FILENO), err = dup(STDERR_FILENO);
    struct Output { int out, err; ~Output() { dup2(out, STDOUT_FILENO); dup2(err, STDERR_FILENO); close(out); close(err); } } output{out, err};
    if (!log.empty()) {
        fs::create_directories(log.parent_path());
        FILE* file = fopen(log.c_str(), "ab");
        if (!file) throw std::runtime_error("cannot open build log");
        dup2(fileno(file), STDOUT_FILENO); dup2(fileno(file), STDERR_FILENO); fclose(file);
    }
#endif
#ifdef _WIN32
    if (args.size() == 6 && lower(path_utf8(utf8_path(args.front()).filename())) == "cmd.exe" && args[3] == "call")
        return windows_batch(utf8_path(args[0]), utf8_path(args[4]), args[5]);
#endif
    return runtime::run_contained(args);
}
Options parse(const std::vector<std::string>& args) {
    Options options;
    std::set<std::string> seen;
    std::vector<std::string> overrides;
    for (size_t i = 1; i < args.size(); ++i) {
        const auto arg = args[i];
        if (arg != "--runtime-dir" && arg != "--build-dir" && !seen.insert(arg).second) throw std::runtime_error("duplicate option: " + arg);
        if (arg == "--native" || arg == "--no-install-rocm") continue; // Native mode never installs an SDK.
        if (arg == "--dry-run") { options.dry_run = true; continue; }
        if (arg == "--package-only") { options.package_only = true; continue; }
        if (arg == "--hosts" || arg == "--hip-archs") {
            auto& target = arg == "--hosts" ? options.hosts : options.archs; target.clear();
            while (i + 1 < args.size() && !args[i + 1].starts_with("--")) target.push_back(args[++i]);
            if (target.empty() || std::set<std::string>(target.begin(), target.end()).size() != target.size()) throw std::runtime_error("provide distinct values for " + arg);
            continue;
        }
        if (++i == args.size() || args[i].empty()) throw std::runtime_error("missing value for " + arg);
        const auto value = args[i];
        if (arg == "--source-root") options.root = utf8_path(value);
        else if (arg == "--build-root") options.build_root = utf8_path(value);
        else if (arg == "--out-root") options.out_root = utf8_path(value);
        else if (arg == "--ggml-dir") options.ggml = utf8_path(value);
        else if (arg == "--rocm-root") options.rocm = utf8_path(value);
        else if (arg == "--rocm-venv") options.rocm = utf8_path(value) / "Lib/site-packages/_rocm_sdk_devel";
        else if (arg == "--rocm-compiler-root") options.rocm_compiler = utf8_path(value);
        else if (arg == "--jsoncons-dir" || arg == "--webp-dir" || arg == "--avif-dir" || arg == "--aom-dir") {
            const std::map<std::string, std::string> keys{{"--jsoncons-dir", "STRATA_JSONCONS_DIR"}, {"--webp-dir", "STRATA_WEBP_DIR"},
                {"--avif-dir", "STRATA_AVIF_DIR"}, {"--aom-dir", "STRATA_AOM_DIR"}};
            options.dependency_dirs[keys.at(arg)] = resolved(utf8_path(value));
        }
        else if (arg == "--runtime-dir") options.runtime_dirs.push_back(resolved(utf8_path(value)));
        else if (arg == "--build-dir") overrides.push_back(value);
        else if (arg == "--jobs") {
            size_t end; options.jobs = std::stoi(value, &end);
            if (end != value.size() || options.jobs < 1 || options.jobs > 128) throw std::runtime_error("jobs must be 1..128");
        } else throw std::runtime_error("unknown option: " + arg);
    }
    if (options.root.empty()) options.root = fs::current_path();
    options.root = resolved(options.root);
    if (!fs::is_regular_file(options.root / "tools/build_cluster.py") || !fs::is_directory(options.root / "serve/web"))
        throw std::runtime_error("run in the Strata source directory or pass --source-root");
    options.build_root = resolved(options.build_root.empty() ? options.root / "build-cluster" : options.build_root);
    options.out_root = resolved(options.out_root.empty() ? options.root / "dist/cluster-native" : options.out_root);
    if (options.ggml.empty()) {
        options.ggml = options.root / "third_party/llama.cpp";
        if (!fs::is_regular_file(options.ggml / "ggml/CMakeLists.txt")) options.ggml = options.root / "build-intel/_deps/strata_llamacpp-src";
    }
    options.ggml = resolved(options.ggml);
    if (!options.dry_run && !fs::is_regular_file(options.ggml / "ggml/CMakeLists.txt")) throw std::runtime_error("provide --ggml-dir with the pinned llama.cpp checkout");
    if (options.rocm.empty()) options.rocm = utf8_path(runtime::environment_utf8("STRATA_ROCM_ROOT"));
#ifdef _WIN32
    if (options.rocm.empty() && fs::is_directory(options.root / ".rocm-win/Lib/site-packages/_rocm_sdk_devel")) options.rocm = options.root / ".rocm-win/Lib/site-packages/_rocm_sdk_devel";
#else
    if (options.rocm.empty()) options.rocm = "/opt/rocm";
#endif
    if (!options.rocm.empty()) options.rocm = resolved(options.rocm);
    if (options.rocm_compiler.empty()) options.rocm_compiler = utf8_path(runtime::environment_utf8("STRATA_ROCM_COMPILER_ROOT"));
    if (options.rocm_compiler.empty()) options.rocm_compiler = options.rocm;
#ifdef _WIN32
    if (!fs::is_regular_file(options.rocm_compiler / "lib/llvm/bin/clang++.exe") && fs::is_regular_file(options.rocm / "../_rocm_sdk_core/lib/llvm/bin/clang++.exe"))
        options.rocm_compiler = resolved(options.rocm / "../_rocm_sdk_core");
#endif
    if (!options.rocm_compiler.empty()) options.rocm_compiler = resolved(options.rocm_compiler);
    for (const auto& host : options.hosts) {
        if (host != "host16" && host != "host11" && host != "host17") throw std::runtime_error("unknown host: " + host);
        options.builds[host] = options.build_root / host;
    }
    std::set<std::string> specified;
    for (const auto& value : overrides) {
        const auto equal = value.find('='); const auto host = value.substr(0, equal);
        if (equal == std::string::npos || equal + 1 == value.size() || !options.builds.count(host) || !specified.insert(host).second)
            throw std::runtime_error("--build-dir must specify a selected host once as HOST=DIR");
        options.builds[host] = resolved(utf8_path(value.substr(equal + 1)));
    }
    for (const auto& arch : options.archs) if (!std::regex_match(arch, std::regex("gfx[0-9a-f]+"))) throw std::runtime_error("invalid HIP architecture: " + arch);
    std::vector<fs::path> paths;
    for (const auto& [host, build] : options.builds) {
        const auto destination = options.out_root / host; direct_child(destination);
        paths.push_back(build); paths.push_back(destination);
        for (const auto& source : {options.root / "serve", options.root / "configs", options.ggml, options.rocm})
            if (!source.empty() && (contains(source, destination) || contains(destination, source))) throw std::runtime_error("output overlaps source resources or SDK");
    }
    for (size_t i = 0; i < paths.size(); ++i) for (size_t j = i + 1; j < paths.size(); ++j)
        if (contains(paths[i], paths[j]) || contains(paths[j], paths[i])) throw std::runtime_error("each node's build and output directories must be independent");
    return options;
}
int execute(const Options& options) {
    for (const auto& host : options.hosts) {
        std::cout << host << ": " << (host == "host16" ? "oneAPI O3, native CPU + CUDA sm_89" : host == "host11" ? "oneAPI O3, portable AVX2, no QxHost" : "ROCm Clang, portable AVX2")
            << "\n  build: " << path_utf8(options.builds.at(host)) << "\n  output: " << path_utf8(options.out_root / host) << '\n';
        if (host == "host17") { std::cout << "  HIP: "; for (const auto& arch : options.archs) std::cout << arch << ' '; std::cout << '\n'; }
    }
    if (options.dry_run) { std::cout << "Dry run: no writes, tool invocations or downloads.\n"; return 0; }
#ifndef _WIN32
    throw std::runtime_error("this stage provides Windows builds/packages only; Linux migration is pending");
#endif
    fs::create_directories(options.out_root);
    const auto logs = options.out_root / "_logs" / token();
    Json summary = Json::object();
    bool failed = false;
    for (const auto& host : options.hosts) {
        try {
            Lock lock(options.out_root, host);
            Stage temporary(options.out_root);
            const auto build = options.builds.at(host);
            if (!options.package_only) build_node(options, host, build, logs / (host + ".log"));
            const auto stage = temporary.path / host;
            package(options, host, build, stage);
            const auto destination = options.out_root / host;
            const auto backup = publish(stage, destination);
            summary[host] = {{"status", "success"}, {"output", path_utf8(destination)}, {"previous_output", backup.empty() ? Json(nullptr) : Json(backup)}};
            std::cout << '[' << host << "] ready: " << path_utf8(destination) << std::endl;
        } catch (const BuildCancelled& error) {
            summary[host] = {{"status", "cancelled"}, {"error", error.what()}};
            for (const auto& pending : options.hosts) if (!summary.contains(pending)) summary[pending] = {{"status", "cancelled"}};
            atomic_json(options.out_root / "build-summary.json", summary);
            std::cerr << error.what() << std::endl;
            return 130;
        } catch (const Json::exception&) {
            failed = true; summary[host] = {{"status", "failed"}, {"error", "invalid JSON metadata"}};
            std::cerr << '[' << host << "] FAILED: invalid JSON metadata\n";
        } catch (const std::exception& error) {
            failed = true; summary[host] = {{"status", "failed"}, {"error", error.what()}};
            std::cerr << '[' << host << "] FAILED: " << error.what() << std::endl;
        }
    }
    atomic_json(options.out_root / "build-summary.json", summary);
    return failed ? 1 : 0;
}
int main(const std::vector<std::string>& args) {
    if (std::find(args.begin(), args.end(), "--help") != args.end()) {
        std::cout << "strata-cluster-build [--hosts host16 host11 host17] [--dry-run | --package-only]\n"
            "  --source-root DIR --build-root DIR --out-root DIR --build-dir HOST=DIR\n"
            "  --jobs 1..128 --hip-archs gfx1102 gfx1103 --ggml-dir DIR\n"
            "  --rocm-root DIR --rocm-compiler-root DIR --runtime-dir DIR (repeatable)\n"
            "  --jsoncons-dir DIR --webp-dir DIR --avif-dir DIR --aom-dir DIR (offline sources)\n"
            "Native builds only; requires installed oneAPI/CUDA or unpacked ROCm, CMake/Ninja.\n"
            "Windows builds/packages in this stage. No Python or SDK installer is invoked.\n"
            "Publishes one node at a time, preserves *.previous-* packages, never stops services.\n";
        return 0;
    }
    try { return execute(parse(args)); }
    catch (const Json::exception&) { std::cerr << "cluster build failed: invalid JSON metadata\n"; }
    catch (const std::exception& error) { std::cerr << "cluster build failed: " << error.what() << '\n'; }
    return 2;
}
} // namespace strata::build
