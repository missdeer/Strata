#include "strata/runtime/host_commands.hpp"
// Native LAN worker. All request buffers and backend state are reused; JSON,
// GGUF directory parsing and model fingerprinting happen only during startup.
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>
#endif

#include "strata/core/network_backend.h"
#include "strata/runtime/cluster_config.hpp"
#include "strata/runtime/expert_store.hpp"
#include "strata/runtime/path.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
using Bytes = std::vector<uint8_t>;
using strata::runtime::utf8_path;
constexpr int hidden = 2560, max_tokens = 8, max_entries = 80;
constexpr uint32_t magic = 0x5354584e, version = 1, max_body = 2u << 20;
volatile std::sig_atomic_t stopped = 0;
void stop(int) { stopped = 1; }
std::string environment(const char* name) {
#ifdef _WIN32
    const std::wstring wide_name(name, name + std::strlen(name));
    const auto size = GetEnvironmentVariableW(wide_name.c_str(), nullptr, 0);
    if (!size) return {};
    std::wstring value(size, L'\0');
    if (!GetEnvironmentVariableW(wide_name.c_str(), value.data(), size)) return {};
    const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (!bytes) throw std::runtime_error("invalid UTF-16 environment variable");
    std::string result(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.c_str(), -1, result.data(), bytes, nullptr, nullptr);
    result.pop_back(); return result;
#else
    const auto* p = std::getenv(name); return p ? p : "";
#endif
}
uint64_t get(const uint8_t* p, size_t n = 4) {
    uint64_t value = 0; for (size_t i = 0; i < n; ++i) value |= uint64_t(p[i]) << (8 * i); return value;
}
void put(uint8_t* p, uint64_t value, size_t n = 4) { for (size_t i = 0; i < n; ++i) p[i] = static_cast<uint8_t>(value >> (8 * i)); }

#ifdef _WIN32
using Socket = SOCKET;
constexpr Socket invalid_socket = INVALID_SOCKET;
void close_socket(Socket s) { closesocket(s); }
#else
using Socket = int;
constexpr Socket invalid_socket = -1;
void close_socket(Socket s) { close(s); }
#endif
struct SocketOwner {
    Socket value = invalid_socket;
    explicit SocketOwner(Socket s) : value(s) {}
    ~SocketOwner() { if (value != invalid_socket) close_socket(value); }
    SocketOwner(const SocketOwner&) = delete;
    SocketOwner& operator=(const SocketOwner&) = delete;
};
void nonblocking(Socket s) {
#ifdef _WIN32
    u_long mode = 1;
    if (ioctlsocket(s, FIONBIO, &mode)) throw std::runtime_error("cannot set nonblocking socket");
#else
    const int flags = fcntl(s, F_GETFL);
    if (flags < 0 || fcntl(s, F_SETFL, flags | O_NONBLOCK) < 0) throw std::runtime_error("cannot set nonblocking socket");
#endif
}
bool interrupted_socket() {
#ifdef _WIN32
    const auto error = WSAGetLastError(); return error == WSAEWOULDBLOCK || error == WSAEINTR;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
#endif
}
void ready(Socket s, bool writing, Clock::time_point deadline) {
    for (;;) {
        if (stopped) throw std::runtime_error("worker stopping");
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        if (remaining <= 0) throw std::runtime_error("frame deadline exceeded");
        const int ms = static_cast<int>(std::min<int64_t>(remaining, 100));
#ifdef _WIN32
        WSAPOLLFD p{s, static_cast<SHORT>(writing ? POLLWRNORM : POLLRDNORM), 0};
        const int rc = WSAPoll(&p, 1, ms);
#else
        pollfd p{s, static_cast<short>(writing ? POLLOUT : POLLIN), 0};
        const int rc = poll(&p, 1, ms);
#endif
        if (rc < 0 && interrupted_socket()) continue;
        if (rc < 0 || (p.revents & (POLLERR | POLLNVAL))) throw std::runtime_error("connection failed");
        if (rc > 0) return; // HUP reaches recv(), which reports EOF.
    }
}
struct Disconnected : std::runtime_error { Disconnected() : std::runtime_error("connection closed") {} };
void receive(Socket s, uint8_t* p, size_t bytes, Clock::time_point deadline) {
    while (bytes) {
        ready(s, false, deadline);
#ifdef _WIN32
        const int rc = recv(s, reinterpret_cast<char*>(p), static_cast<int>(bytes), 0);
#else
        const auto rc = recv(s, p, bytes, 0);
#endif
        if (rc < 0 && interrupted_socket()) continue;
        if (rc <= 0) throw Disconnected();
        p += rc; bytes -= static_cast<size_t>(rc);
    }
}
struct Frame { uint64_t seq = 0; uint32_t kind = 0; Bytes body; };
void frame(Socket s, Frame& f, int timeout_ms) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    std::array<uint8_t, 24> header{};
    receive(s, header.data(), header.size(), deadline);
    f.seq = get(header.data() + 8, 8);
    const auto size = get(header.data() + 16);
    if (get(header.data()) != magic || get(header.data() + 4) != version || size > max_body)
        throw std::runtime_error("invalid protocol header");
    f.kind = static_cast<uint32_t>(get(header.data() + 20));
    f.body.resize(static_cast<size_t>(size));
    receive(s, f.body.data(), f.body.size(), deadline);
}
// Scatter-send header and result without constructing a Python-style immutable
// snapshot. Single-session ownership keeps the output alive through the send.
void send_frame(Socket s, uint64_t seq, uint32_t kind, const void* data, size_t bytes, int timeout_ms) {
    std::array<uint8_t, 24> header{};
    put(header.data(), magic); put(header.data() + 4, version); put(header.data() + 8, seq, 8);
    put(header.data() + 16, bytes); put(header.data() + 20, kind);
    const uint8_t* ptrs[] = {header.data(), static_cast<const uint8_t*>(data)};
    size_t lengths[] = {header.size(), bytes};
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    int first = 0;
    while (first < 2) {
        if (!lengths[first]) { ++first; continue; }
        ready(s, true, deadline);
#ifdef _WIN32
        WSABUF buffers[2]{};
        for (int i = first; i < 2; ++i) { buffers[i - first].buf = reinterpret_cast<char*>(const_cast<uint8_t*>(ptrs[i])); buffers[i - first].len = static_cast<ULONG>(lengths[i]); }
        DWORD sent = 0;
        if (WSASend(s, buffers, static_cast<DWORD>(2 - first), &sent, 0, nullptr, nullptr)) {
            if (interrupted_socket()) continue;
            throw Disconnected();
        }
        size_t n = sent;
#else
        iovec buffers[2]{};
        for (int i = first; i < 2; ++i) { buffers[i - first].iov_base = const_cast<uint8_t*>(ptrs[i]); buffers[i - first].iov_len = lengths[i]; }
        msghdr message{}; message.msg_iov = buffers; message.msg_iovlen = static_cast<size_t>(2 - first);
        const auto sent = sendmsg(s, &message, MSG_NOSIGNAL);
        if (sent < 0 && interrupted_socket()) continue;
        if (sent <= 0) throw Disconnected();
        size_t n = static_cast<size_t>(sent);
#endif
        if (!n) throw Disconnected();
        while (first < 2 && n >= lengths[first]) { n -= lengths[first]; ++first; }
        if (first < 2 && n) { ptrs[first] += n; lengths[first] -= n; }
    }
}

class Backend {
public:
    explicit Backend(const std::filesystem::path& library) {
#ifdef _WIN32
        // Only this process's loader search path is changed. ROCm puts its
        // dependent DLLs below rocm/bin in the deployment package.
        const auto folder = library.parent_path();
        const auto rocm = folder / "rocm" / "bin";
        if (!SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS)) throw std::runtime_error("cannot configure DLL search path");
        dirs_.push_back(AddDllDirectory(folder.c_str()));
        if (std::filesystem::is_directory(rocm)) dirs_.push_back(AddDllDirectory(rocm.c_str()));
        library_ = LoadLibraryExW(library.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
#else
        library_ = dlopen(library.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
        if (!library_) {
            close(); const auto text = library.u8string();
            throw std::runtime_error("cannot load worker backend: " + std::string(text.begin(), text.end()));
        }
        try {
#define LOAD(name) name = symbol<decltype(name)>(#name)
            LOAD(strata_network_error); LOAD(strata_network_create); LOAD(strata_network_destroy);
            LOAD(strata_network_format_bytes); LOAD(strata_network_compute);
            LOAD(strata_network_all_finite); LOAD(strata_network_reduce);
#undef LOAD
        } catch (...) { close(); throw; }
    }
    ~Backend() { if (handle) strata_network_destroy(handle); close(); }
    Backend(const Backend&) = delete;
    Backend& operator=(const Backend&) = delete;
    decltype(&::strata_network_error) strata_network_error = nullptr;
    decltype(&::strata_network_create) strata_network_create = nullptr;
    decltype(&::strata_network_destroy) strata_network_destroy = nullptr;
    decltype(&::strata_network_format_bytes) strata_network_format_bytes = nullptr;
    decltype(&::strata_network_compute) strata_network_compute = nullptr;
    decltype(&::strata_network_all_finite) strata_network_all_finite = nullptr;
    decltype(&::strata_network_reduce) strata_network_reduce = nullptr;
    void* handle = nullptr;
    bool failed = false;
private:
    template<typename T> T symbol(const char* name) {
#ifdef _WIN32
        auto pointer = GetProcAddress(library_, name);
#else
        auto pointer = dlsym(library_, name);
#endif
        if (!pointer) throw std::runtime_error(std::string("worker library lacks ") + name + "; update the backend");
        return reinterpret_cast<T>(pointer);
    }
    void close() {
#ifdef _WIN32
        if (library_) FreeLibrary(library_);
        for (auto dir : dirs_) if (dir) RemoveDllDirectory(dir);
        dirs_.clear();
#else
        if (library_) dlclose(library_);
#endif
        library_ = nullptr;
    }
#ifdef _WIN32
    HMODULE library_ = nullptr;
    std::vector<DLL_DIRECTORY_COOKIE> dirs_;
#else
    void* library_ = nullptr;
#endif
};

struct Options {
    std::filesystem::path pack, native, library;
    std::string host = "127.0.0.1", client = "127.0.0.1", assignment, digest;
    int port = 18500, index = -1, count = 2, workers = 0, device = -1, timeout_ms = 60000, layers = 0;
    uint64_t ram_mib = 512, gpu_mib = 4096;
    bool preload = false, verbose = false;
};
uint64_t number(const std::string& text) {
    if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos) throw std::runtime_error("expected nonnegative integer argument");
    return std::stoull(text);
}
int small_number(const std::string& text) {
    auto n = number(text); if (n > INT32_MAX) throw std::runtime_error("integer argument out of range"); return static_cast<int>(n);
}
void print_plan(const strata::runtime::ClusterConfig& plan) {
    std::cout << strata::runtime::Json{{"config_sha256", plan.digest}, {"assignment", plan.assignment}, {"endpoints", plan.endpoints}}.dump() << std::endl;
}
bool options(const std::vector<std::string>& args, Options& o) {
    std::string config, node, backend = "cpu";
    bool validate = false;
    std::vector<std::string> names;
    for (size_t i = 1; i < args.size(); ++i) {
        const auto& arg = args[i]; names.push_back(arg);
        if (arg == "--help") {
            std::cout << "strata-host worker --cluster-config FILE --node ID\n"
                         "strata-host worker --cluster-config FILE --validate\n"
                         "strata-host worker --pack DIR --native GGUF --library DLL --index N [--count N]\n"
                         "  [--host IPv4 --port PORT --allow-client IPv4 --backend cpu|gpu --device N]\n"
                         "  [--workers N --ram-cache-mib N --gpu-cache-mib N --preload-experts --timeout-ms N --verbose]\n"
                         "Set STRATA_NETWORK_KEY (16..256 UTF-8 bytes). Profiling: STRATA_NETWORK_PROFILE=1.\n";
            return false;
        }
        if (arg == "--validate") { validate = true; continue; }
        if (arg == "--preload-experts") { o.preload = true; continue; }
        if (arg == "--verbose") { o.verbose = true; continue; }
        if (++i == args.size()) throw std::runtime_error("missing value for " + arg);
        const auto& value = args[i];
        if (arg == "--cluster-config") config = value;
        else if (arg == "--node") node = value;
        else if (arg == "--pack") o.pack = utf8_path(value);
        else if (arg == "--native") o.native = utf8_path(value);
        else if (arg == "--library") o.library = utf8_path(value);
        else if (arg == "--host") o.host = value;
        else if (arg == "--allow-client") o.client = value;
        else if (arg == "--backend") backend = value;
        else if (arg == "--port") o.port = small_number(value);
        else if (arg == "--index") o.index = small_number(value);
        else if (arg == "--count") o.count = small_number(value);
        else if (arg == "--workers") o.workers = small_number(value);
        else if (arg == "--device") o.device = small_number(value);
        else if (arg == "--ram-cache-mib") o.ram_mib = number(value);
        else if (arg == "--gpu-cache-mib") o.gpu_mib = number(value);
        else if (arg == "--timeout-ms") o.timeout_ms = small_number(value);
        else throw std::runtime_error("unknown option: " + arg);
    }
    if (!config.empty()) {
        for (const auto& arg : names)
            if (arg != "--cluster-config" && arg != "--node" && arg != "--validate")
                throw std::runtime_error("cluster startup accepts only --cluster-config and --node; edit the shared plan");
        const auto plan = strata::runtime::ClusterConfig::load(utf8_path(config));
        if (!node.empty()) plan.node(node);
        print_plan(plan);
        if (validate) return false;
        const auto& n = plan.node(node);
        if (n["role"] != "expert-worker") throw std::runtime_error("this node is the coordinator; native API migration is not complete");
        o.pack = utf8_path(n["pack"].get<std::string>()); o.native = utf8_path(n["native"].get<std::string>());
        o.library = utf8_path(n["library"].get<std::string>());
        o.host = n["host"].get<std::string>(); o.client = plan.coordinator["host"].get<std::string>();
        o.port = n["port"].get<int>(); o.workers = n["workers"].get<int>();
        o.ram_mib = n["ram_cache_mib"].get<uint64_t>(); o.gpu_mib = n["gpu_cache_mib"].get<uint64_t>();
        o.device = n["backend"] == "gpu" ? n["device"].get<int>() : -1;
        o.preload = n.value("preload_experts", false); o.count = static_cast<int>(plan.workers.size());
        for (size_t i = 0; i < plan.workers.size(); ++i) if (plan.workers[i]["id"] == node) o.index = static_cast<int>(i);
        o.timeout_ms = std::max(1000, plan.data["scheduler"]["timeout_ms"].get<int>());
        o.assignment = plan.assignment; o.digest = plan.digest;
        o.layers = plan.data["model"]["layers"].get<int>();
    } else {
        if (validate || !node.empty()) throw std::runtime_error("--validate/--node requires --cluster-config");
        if (backend != "cpu" && backend != "gpu") throw std::runtime_error("backend must be cpu or gpu");
        if (backend == "gpu") o.device = std::max(0, o.device);
        else o.device = -1;
    }
    const int limit = o.digest.empty() ? 3 : 16;
    if (o.pack.empty() || o.library.empty() || o.port < 1 || o.port > 65535 || o.count < 1 || o.count > limit || o.index < 0 || o.index >= o.count ||
        o.workers > 256 || o.device > 255 || o.ram_mib < 64 || o.ram_mib > 1048576 || o.gpu_mib < 64 || o.gpu_mib > 65536 || o.timeout_ms < 100 || o.timeout_ms > 300000)
        throw std::runtime_error("valid pack, library, port, index/count, timeout, worker count and cache budgets are required");
    return true;
}

class Worker {
public:
    Worker(const Options& options, strata::runtime::ExpertStore& store, Backend& backend, std::string key)
        : o_(options), store_(store), b_(backend), key_(std::move(key)), fingerprint_(store.fingerprint()) {}
    uint64_t fingerprint() const { return fingerprint_; }
    void session(Socket socket) {
        Frame f; f.body.reserve(max_tokens * hidden * 4 + 16 + max_entries * 12);
        try {
            nonblocking(socket);
            const int one = 1;
            setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
            frame(socket, f, o_.timeout_ms);
            const bool configured = !o_.digest.empty();
            const size_t offset = configured ? 28 + 64 + 512 : 28;
            const bool reduced = f.kind == (configured ? 6u : 5u);
            if (f.seq != 0 || (f.kind != (configured ? 3u : 1u) && !reduced) || f.body.size() < offset)
                throw std::runtime_error("handshake required");
            // Compare every byte for equal-length keys; do not log credentials.
            unsigned difference = 0;
            if (f.body.size() - offset != key_.size()) throw std::runtime_error("authentication failed");
            for (size_t i = 0; i < key_.size(); ++i) difference |= f.body[offset + i] ^ static_cast<uint8_t>(key_[i]);
            if (difference) throw std::runtime_error("authentication failed");
            if (configured && (std::memcmp(f.body.data() + 28, o_.digest.data(), 64) || std::memcmp(f.body.data() + 92, o_.assignment.data(), 512)))
                throw std::runtime_error("cluster configuration mismatch; restart every node with the same plan");
            if (get(f.body.data(), 8) != fingerprint_ || get(f.body.data() + 8) != store_.layers().size() || get(f.body.data() + 12) != 512 ||
                get(f.body.data() + 16) != static_cast<uint64_t>(o_.index) || get(f.body.data() + 20) != static_cast<uint64_t>(o_.count) || get(f.body.data() + 24) != hidden)
                throw std::runtime_error("model fingerprint, geometry, or shard assignment mismatch");
            send_frame(socket, 0, f.kind + 128, nullptr, 0, o_.timeout_ms);
            uint64_t expected = 1;
            for (;;) {
                ready(socket, false, Clock::time_point::max()); // indefinite idle, interruptible shutdown
                frame(socket, f, o_.timeout_ms);
                if (f.kind != (reduced ? 4u : 2u) || f.seq != expected) throw std::runtime_error("unexpected task identity");
                if (b_.failed) throw std::runtime_error("native backend failed; restart the worker");
                const auto parse_start = Clock::now();
                if (f.body.size() < 16) throw std::runtime_error("truncated task");
                const auto layer = get(f.body.data()), nt = get(f.body.data() + 4), k = get(f.body.data() + 8), n = get(f.body.data() + 12);
                if (layer >= store_.layers().size() || nt < 1 || nt > max_tokens || k < 1 || k > 10 || n < 1 || n > nt * k)
                    throw std::runtime_error("invalid task geometry");
                const size_t stride = reduced ? 12 : 8, metadata = 16 + static_cast<size_t>(n) * stride;
                if (f.body.size() != metadata + nt * hidden * 4) throw std::runtime_error("invalid task size");
                std::array<bool, max_entries> seen{};
                for (size_t i = 0; i < n; ++i) {
                    const auto* p = f.body.data() + 16 + i * stride;
                    const auto row = get(p), expert = get(p + 4);
                    const auto owner = expert < 512 ? (configured ? std::string("0123456789abcdef").find(o_.assignment[expert]) : expert % o_.count) : SIZE_MAX;
                    if (row >= nt * k || expert >= 512 || owner != static_cast<size_t>(o_.index) || seen[row])
                        throw std::runtime_error("invalid, duplicate, or unowned routing entry");
                    for (size_t j = 0; j < i; ++j)
                        if (rows_[j] / static_cast<int32_t>(k) == static_cast<int32_t>(row / k) && ids_[j] == static_cast<int32_t>(expert))
                            throw std::runtime_error("invalid, duplicate, or unowned routing entry");
                    seen[row] = true; rows_[i] = static_cast<int32_t>(row); ids_[i] = static_cast<int32_t>(expert);
                    if (reduced) {
                        if (!b_.strata_network_all_finite(p + 8, 4)) throw std::runtime_error("non-finite router weight");
                        std::memcpy(&weights_[i], p + 8, 4);
                    }
                }
                if (!b_.strata_network_all_finite(f.body.data() + metadata, nt * hidden * 4)) throw std::runtime_error("non-finite activation");
                const auto parsed = Clock::now();
                // Each shared_ptr pins a blob even if a later lookup evicts it.
                std::array<strata::runtime::ExpertStore::Buffer, max_entries> buffers;
                for (size_t i = 0; i < n; ++i) { buffers[i] = store_.get(static_cast<int>(layer), ids_[i]); pointers_[i] = buffers[i]->data(); }
                std::memcpy(input_.data(), f.body.data() + metadata, static_cast<size_t>(nt) * hidden * 4);
                const auto prepared = Clock::now();
                const auto& fmt = store_.layers()[layer];
                if (!b_.strata_network_compute(b_.handle, fmt.gate_up, fmt.down, static_cast<int>(layer), input_.data(), static_cast<int>(nt), static_cast<int>(n),
                    rows_.data(), ids_.data(), pointers_.data(), static_cast<int>(k), output_.data())) {
                    b_.failed = true; throw std::runtime_error(b_.strata_network_error());
                }
                const auto computed = Clock::now();
                if (!b_.strata_network_all_finite(output_.data(), n * hidden * 4)) {
                    b_.failed = true; throw std::runtime_error("native backend returned non-finite results");
                }
                const auto checked = Clock::now();
                const float* result = output_.data(); size_t size = static_cast<size_t>(n) * hidden * 4;
                if (reduced) {
                    if (!b_.strata_network_reduce(output_.data(), rows_.data(), weights_.data(), static_cast<int>(nt), static_cast<int>(n), static_cast<int>(k), sum_.data())) {
                        b_.failed = true; throw std::runtime_error(b_.strata_network_error());
                    }
                    result = sum_.data(); size = static_cast<size_t>(nt) * hidden * 4;
                }
                const auto reduced_at = Clock::now();
                send_frame(socket, f.seq, f.kind + 128, result, size, o_.timeout_ms);
                const auto sent = Clock::now();
                if (o_.verbose || profile_) {
                    auto ms = [](auto a, auto z) { return std::chrono::duration<double, std::milli>(z - a).count(); };
                    std::cout << "task=" << f.seq << " layer=" << layer << " entries=" << n << " bytes=" << f.body.size() + size
                              << " parse_ms=" << ms(parse_start, parsed) << " prepare_ms=" << ms(parsed, prepared)
                              << " native_ms=" << ms(prepared, computed) << " validate_ms=" << ms(computed, checked)
                              << " reduce_ms=" << ms(checked, reduced_at) << " send_ms=" << ms(reduced_at, sent) << std::endl;
                }
                ++expected;
            }
        } catch (const Disconnected&) {
        } catch (const std::exception& e) {
            const std::string error = std::string(e.what()).substr(0, 1024);
            try { send_frame(socket, f.seq, 255, error.data(), error.size(), o_.timeout_ms); } catch (...) {}
            if (!stopped) std::cerr << "worker session ended: " << error << std::endl;
        }
    }
private:
    const Options& o_;
    strata::runtime::ExpertStore& store_;
    Backend& b_;
    std::string key_;
    uint64_t fingerprint_;
    bool profile_ = environment("STRATA_NETWORK_PROFILE") == "1";
    std::array<int32_t, max_entries> rows_{}, ids_{};
    std::array<const uint8_t*, max_entries> pointers_{};
    std::array<float, max_entries> weights_{};
    std::vector<float> input_ = std::vector<float>(max_tokens * hidden);
    std::vector<float> output_ = std::vector<float>(max_entries * hidden);
    std::vector<float> sum_ = std::vector<float>(max_tokens * hidden);
};

int run(const std::vector<std::string>& args) {
    try {
        Options o;
        if (!options(args, o)) return 0;
        const auto key = environment("STRATA_NETWORK_KEY");
        if (key.size() < 16 || key.size() > 256) throw std::runtime_error("set STRATA_NETWORK_KEY to a 16..256 byte secret");
#ifdef _WIN32
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data)) throw std::runtime_error("WSAStartup failed");
        struct Cleanup { ~Cleanup() { WSACleanup(); } } cleanup;
#endif
        sockaddr_in address{}, client{};
        address.sin_family = AF_INET; address.sin_port = htons(static_cast<uint16_t>(o.port));
        if (inet_pton(AF_INET, o.host.c_str(), &address.sin_addr) != 1 || inet_pton(AF_INET, o.client.c_str(), &client.sin_addr) != 1)
            throw std::runtime_error("invalid worker/coordinator IPv4 address");
        uint32_t endian = 1;
        if (*reinterpret_cast<uint8_t*>(&endian) != 1 || sizeof(float) != 4) throw std::runtime_error("worker requires little-endian FP32 host");
        std::signal(SIGINT, stop); std::signal(SIGTERM, stop);
        strata::runtime::ExpertStore store(o.pack, o.native, o.ram_mib << 20);
        if (o.layers && store.layers().size() != static_cast<size_t>(o.layers))
            throw std::runtime_error("local pack does not match the model geometry in the shared plan");
        Backend backend(std::filesystem::absolute(o.library));
        for (const auto& f : store.layers())
            if (backend.strata_network_format_bytes(f.gate_up, f.down) != f.bytes) throw std::runtime_error("backend does not support the pack's expert geometry");
        backend.handle = backend.strata_network_create(o.workers, o.device, o.gpu_mib);
        if (!backend.handle) throw std::runtime_error(backend.strata_network_error());
        Worker worker(o, store, backend, key);
        if (o.preload) store.preload(o.index, o.count, o.assignment, [] { return stopped != 0; });
        if (stopped) return 0;
        SocketOwner listener(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
        if (listener.value == invalid_socket || bind(listener.value, reinterpret_cast<sockaddr*>(&address), sizeof(address)) || listen(listener.value, 4))
            throw std::runtime_error("cannot bind/listen on worker address");
        nonblocking(listener.value);
        std::cout << "worker ready: " << o.host << ':' << o.port << " shard=" << o.index << '/' << o.count
                  << " backend=" << (o.device < 0 ? "cpu" : "gpu") << " fingerprint=" << std::hex << worker.fingerprint() << std::dec << std::endl;
        while (!stopped) {
            ready(listener.value, false, Clock::time_point::max());
            sockaddr_in peer{};
#ifdef _WIN32
            int size = sizeof(peer);
#else
            socklen_t size = sizeof(peer);
#endif
            SocketOwner connection(accept(listener.value, reinterpret_cast<sockaddr*>(&peer), &size));
            if (connection.value == invalid_socket) { if (interrupted_socket()) continue; throw std::runtime_error("accept failed"); }
            if (peer.sin_addr.s_addr == client.sin_addr.s_addr) worker.session(connection.value);
        }
        return 0;
    } catch (const std::exception& e) {
        if (stopped) return 0;
        std::cerr << "worker startup failed: " << e.what() << std::endl;
        return 1;
    }
}
} // namespace

int strata::runtime::worker_main(const std::vector<std::string>& args) { return run(args); }
