#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "strata/core/network_experts.hpp"
#include "strata/kernels/cpu/expert_layout.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <future>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <cstdio>
#include <stdexcept>

namespace strata::core {
namespace {
using Clock = std::chrono::steady_clock;
#ifdef _WIN32
using Socket = SOCKET;
constexpr Socket invalid = INVALID_SOCKET;
void close_socket(Socket s) { closesocket(s); }
#else
using Socket = int;
constexpr Socket invalid = -1;
void close_socket(Socket s) { close(s); }
#endif
constexpr uint32_t magic = 0x5354584e, version = 1;
constexpr size_t max_body = 2u << 20;
using Bytes = std::vector<uint8_t>;
void put(Bytes& b, uint64_t v, int n = 4) {
    for (int i = 0; i < n; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
uint64_t get(const uint8_t* b, int n = 4) {
    uint64_t v = 0;
    for (int i = 0; i < n; ++i) v |= uint64_t(b[i]) << (8 * i);
    return v;
}
void floats(Bytes& b, const float* x, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        uint32_t bits;
        std::memcpy(&bits, x + i, 4);
        put(b, bits);
    }
}
void ready(Socket s, bool writing, Clock::time_point deadline) {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
    if (ms <= 0) throw std::runtime_error("network experts: task deadline exceeded");
#ifdef _WIN32
    WSAPOLLFD p{s, static_cast<SHORT>(writing ? POLLWRNORM : POLLRDNORM), 0};
    const int rc = WSAPoll(&p, 1, static_cast<int>(ms));
#else
    pollfd p{s, static_cast<short>(writing ? POLLOUT : POLLIN), 0};
    const int rc = poll(&p, 1, static_cast<int>(ms));
#endif
    if (rc <= 0 || (p.revents & (POLLERR | POLLNVAL)))
        throw std::runtime_error("network experts: connection failed or timed out");
}
void transfer(Socket s, uint8_t* p, size_t n, bool writing, Clock::time_point deadline) {
    while (n) {
        ready(s, writing, deadline);
#ifdef _WIN32
        const int rc = writing ? send(s, reinterpret_cast<const char*>(p), static_cast<int>(n), 0)
                               : recv(s, reinterpret_cast<char*>(p), static_cast<int>(n), 0);
        if (rc < 0 && WSAGetLastError() == WSAEWOULDBLOCK) continue;
#else
        const auto rc = writing ? send(s, p, n, MSG_NOSIGNAL) : recv(s, p, n, 0);
        if (rc < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
#endif
        if (rc <= 0) throw std::runtime_error("network experts: worker disconnected");
        p += rc;
        n -= static_cast<size_t>(rc);
    }
}
Bytes exchange(Socket s, uint64_t seq, uint32_t type, Bytes body, Clock::time_point deadline) {
    Bytes header;
    put(header, magic); put(header, version); put(header, seq, 8);
    put(header, body.size()); put(header, type);
    transfer(s, header.data(), header.size(), true, deadline);
    transfer(s, body.data(), body.size(), true, deadline);
    std::array<uint8_t, 24> h{};
    transfer(s, h.data(), h.size(), false, deadline);
    const auto len = get(h.data() + 16);
    if (get(h.data()) != magic || get(h.data() + 4) != version || get(h.data() + 8, 8) != seq || len > max_body)
        throw std::runtime_error("network experts: invalid response identity or size");
    Bytes reply(static_cast<size_t>(len));
    transfer(s, reply.data(), reply.size(), false, deadline);
    if (get(h.data() + 20) == 255)
        throw std::runtime_error("network experts: " + std::string(reply.begin(), reply.end()));
    if (get(h.data() + 20) != type + 128)
        throw std::runtime_error("network experts: unexpected response type");
    return reply;
}
// Sampled identity, matching tools/network_worker.py. Includes formats and both end experts of every layer.
uint64_t identity(const std::function<const uint8_t*(int64_t, int64_t)>& blob, int64_t layers, int64_t experts) {
    uint64_t hash = 14695981039346656037ull;
    auto add = [&](const uint8_t* b, size_t n) {
        for (size_t i = 0; i < n; ++i) { hash ^= b[i]; hash *= 1099511628211ull; }
    };
    const auto& layout = strata::kernels::cpu::expert_layout();
    if (!layout.native) throw std::runtime_error("network experts: first version requires a native (IQ) pack");
    Bytes geometry;
    put(geometry, layers); put(geometry, experts);
    put(geometry, strata::kernels::cpu::H); put(geometry, strata::kernels::cpu::FF);
    add(geometry.data(), geometry.size());
    for (int64_t l = 0; l < layers; ++l) {
        Bytes fmt;
        put(fmt, layout.fmt[static_cast<size_t>(l)].gu_type);
        put(fmt, layout.fmt[static_cast<size_t>(l)].d_type);
        put(fmt, layout.blob_bytes(l), 8);
        add(fmt.data(), fmt.size());
        for (int64_t e : {int64_t{0}, experts - 1}) {
            const auto* b = blob(l, e);
            if (!b) throw std::runtime_error("network experts: cannot fingerprint expert source");
            const size_t n = static_cast<size_t>(layout.blob_bytes(l));
            const size_t sample = std::min(size_t{4096}, n);
            add(b, sample); add(b + n - sample, sample);
        }
    }
    return hash;
}
}  // namespace

struct NetworkExperts::Impl {
    struct Worker {
        Socket socket = invalid;
        uint64_t sequence = 0;
        std::string endpoint;
        std::vector<int32_t> rows;
        std::future<Bytes> pending;
        std::thread io;
        std::mutex mutex;
        std::condition_variable wake;
        std::unique_ptr<std::packaged_task<Bytes()>> task;
        bool stop = false;
        Clock::time_point start;
        uint64_t requests = 0, entries = 0, tx = 0, rx = 0;
        double ms = 0;
        void start_io() {
            io = std::thread([this] {
                for (;;) {
                    std::unique_ptr<std::packaged_task<Bytes()>> work;
                    {
                        std::unique_lock<std::mutex> lock(mutex);
                        wake.wait(lock, [&] { return stop || task != nullptr; });
                        if (stop && !task) return;
                        work = std::move(task);
                    }
                    (*work)();
                }
            });
        }
        void submit(std::packaged_task<Bytes()> work) {
            pending = work.get_future();
            {
                std::lock_guard<std::mutex> lock(mutex);
                task = std::make_unique<std::packaged_task<Bytes()>>(std::move(work));
            }
            wake.notify_one();
        }
        ~Worker() {
            { std::lock_guard<std::mutex> lock(mutex); stop = true; }
            wake.notify_one();
            if (io.joinable()) io.join();
            if (socket != invalid) close_socket(socket);
        }
    };
    std::vector<std::unique_ptr<Worker>> workers;
    std::vector<uint8_t> owned;
    std::vector<size_t> assignment;
    int timeout_ms = 5000;
    int64_t layers = 0, experts = 0;
    bool failed = false;
};
NetworkExperts::NetworkExperts() : impl_(std::make_unique<Impl>()) {}
NetworkExperts::~NetworkExperts() = default;

bool NetworkExperts::open(const std::string& endpoints, const std::string& key, int timeout_ms,
                          const std::function<const uint8_t*(int64_t, int64_t)>& blob,
                          int64_t layers, int64_t experts, std::string& error,
                          const std::string& assignment, const std::string& config_id) {
    try {
        if (!impl_->workers.empty()) throw std::runtime_error("network experts: already open");
        if (key.size() < 16 || key.size() > 256 || timeout_ms < 100 || timeout_ms > 60000)
            throw std::runtime_error("network experts: key needs 16..256 bytes, timeout 100..60000 ms");
#ifdef _WIN32
        static const bool initialized = [] { WSADATA d{}; return WSAStartup(MAKEWORD(2, 2), &d) == 0; }();
        if (!initialized) throw std::runtime_error("network experts: WSAStartup failed");
#endif
        impl_->timeout_ms = timeout_ms; impl_->layers = layers; impl_->experts = experts;
        std::vector<std::string> peers;
        for (size_t from = 0;;) {
            const auto end = endpoints.find(',', from);
            peers.push_back(endpoints.substr(from, end == std::string::npos ? end : end - from));
            if (end == std::string::npos) break;
            from = end + 1;
        }
        const bool configured = !assignment.empty() || !config_id.empty();
        if (peers.empty() || peers.size() > (configured ? 16u : 3u))
            throw std::runtime_error("network experts: choose 1..16 configured workers (1..3 in legacy mode)");
        if (configured) {
            const std::string digits = "0123456789abcdef";
            if (experts != 512 || assignment.size() != 512 || config_id.size() != 64 ||
                config_id.find_first_not_of(digits) != std::string::npos)
                throw std::runtime_error("network experts: invalid static assignment or configuration digest");
            std::vector<bool> used(peers.size(), false);
            for (char c : assignment) {
                const auto owner = digits.find(c);
                if (owner >= peers.size()) throw std::runtime_error("network experts: invalid expert owner");
                impl_->assignment.push_back(owner); used[owner] = true;
            }
            if (std::find(used.begin(), used.end(), false) != used.end())
                throw std::runtime_error("network experts: configured worker owns no experts");
        }
        const uint64_t fingerprint = identity(blob, layers, experts);
        for (size_t i = 0; i < peers.size(); ++i) {
            auto w = std::make_unique<Impl::Worker>();
            w->endpoint = peers[i];
            const auto colon = peers[i].find(':');
            if (colon == std::string::npos) throw std::runtime_error("network experts: expected IPv4:port");
            const std::string port_text = peers[i].substr(colon + 1);
            if (port_text.empty() || port_text.find_first_not_of("0123456789") != std::string::npos)
                throw std::runtime_error("network experts: invalid port");
            const int port = std::stoi(port_text);
            sockaddr_in addr{}; addr.sin_family = AF_INET;
            if (port < 1 || port > 65535 || inet_pton(AF_INET, peers[i].substr(0, colon).c_str(), &addr.sin_addr) != 1)
                throw std::runtime_error("network experts: invalid IPv4 address or port");
            addr.sin_port = htons(static_cast<uint16_t>(port));
            w->socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (w->socket == invalid) throw std::runtime_error("network experts: socket creation failed");
            const int one = 1;
            setsockopt(w->socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
#ifdef _WIN32
            u_long nonblock = 1;
            if (ioctlsocket(w->socket, FIONBIO, &nonblock) != 0) throw std::runtime_error("network experts: nonblocking socket failed");
#else
            if (fcntl(w->socket, F_SETFL, O_NONBLOCK) < 0) throw std::runtime_error("network experts: nonblocking socket failed");
#endif
            const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
            const int rc = connect(w->socket, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
            if (rc != 0) {
                ready(w->socket, true, deadline);
                int fault = 0;
#ifdef _WIN32
                int sz = sizeof(fault);
#else
                socklen_t sz = sizeof(fault);
#endif
                if (getsockopt(w->socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&fault), &sz) != 0 || fault)
                    throw std::runtime_error("network experts: connect failed for " + peers[i]);
            }
            Bytes hello;
            put(hello, fingerprint, 8); put(hello, layers); put(hello, experts);
            put(hello, i); put(hello, peers.size()); put(hello, strata::kernels::cpu::H);
            if (configured) {
                hello.insert(hello.end(), config_id.begin(), config_id.end());
                hello.insert(hello.end(), assignment.begin(), assignment.end());
            }
            hello.insert(hello.end(), key.begin(), key.end());
            const auto reply = exchange(w->socket, 0, configured ? 3 : 1, std::move(hello), deadline);
            if (!reply.empty()) throw std::runtime_error("network experts: invalid handshake result");
            w->start_io();
            impl_->workers.push_back(std::move(w));
        }
        return true;
    } catch (const std::exception& e) { error = e.what(); impl_->failed = true; return false; }
}

bool NetworkExperts::begin(int64_t layer, const float* x, const int32_t* ids, int64_t tokens, int64_t k,
                           int32_t* kind, std::string& error) {
    try {
        if (impl_->failed) throw std::runtime_error("network experts: connection unusable; restart the engine");
        if (tokens < 1 || tokens > strata::kernels::cpu::MAXT || k < 1 || k > 10 ||
            layer < 0 || layer >= impl_->layers)
            throw std::runtime_error("network experts: invalid task geometry");
        impl_->owned.assign(static_cast<size_t>(tokens * k), 0);
        for (auto& w : impl_->workers) {
            if (w->pending.valid()) throw std::runtime_error("network experts: previous layer still pending");
            w->rows.clear();
        }
        for (int64_t r = 0; r < tokens * k; ++r) {
            if (ids[r] < 0 || ids[r] >= impl_->experts) throw std::runtime_error("network experts: invalid expert id");
            if (kind[r] >= 0) continue;
            const auto owner = impl_->assignment.empty() ? static_cast<size_t>(ids[r]) % impl_->workers.size()
                : impl_->assignment[static_cast<size_t>(ids[r])];
            auto& w = impl_->workers[owner];
            w->rows.push_back(static_cast<int32_t>(r));
            impl_->owned[static_cast<size_t>(r)] = 1;
            kind[r] = 3;
        }
        for (auto& wp : impl_->workers) {
            auto* w = wp.get();
            if (w->rows.empty()) continue;
            Bytes task;
            put(task, layer); put(task, tokens); put(task, k); put(task, w->rows.size());
            for (auto r : w->rows) { put(task, r); put(task, ids[r]); }
            floats(task, x, static_cast<size_t>(tokens) * strata::kernels::cpu::H);
            w->start = Clock::now(); ++w->requests; w->entries += w->rows.size(); w->tx += task.size() + 24;
            const auto deadline = w->start + std::chrono::milliseconds(impl_->timeout_ms);
            const auto seq = ++w->sequence;
            w->submit(std::packaged_task<Bytes()>([w, seq, deadline, task = std::move(task)]() mutable {
                return exchange(w->socket, seq, 2, std::move(task), deadline);
            }));
        }
        return true;
    } catch (const std::exception& e) { error = e.what(); impl_->failed = true; return false; }
}
bool NetworkExperts::finish(float* out, std::string& error) {
    try {
        for (auto& w : impl_->workers) {
            if (!w->pending.valid()) continue;
            const auto reply = w->pending.get();
            const size_t expected = w->rows.size() * strata::kernels::cpu::H * 4;
            if (reply.size() != expected) throw std::runtime_error("network experts: wrong result size");
            w->rx += reply.size() + 24;
            w->ms += std::chrono::duration<double, std::milli>(Clock::now() - w->start).count();
            for (size_t j = 0; j < w->rows.size(); ++j)
                for (size_t h = 0; h < strata::kernels::cpu::H; ++h) {
                    const uint32_t bits = static_cast<uint32_t>(get(reply.data() + (j * strata::kernels::cpu::H + h) * 4));
                    float v; std::memcpy(&v, &bits, 4);
                    if (!std::isfinite(v)) throw std::runtime_error("network experts: non-finite result");
                    out[static_cast<size_t>(w->rows[j]) * strata::kernels::cpu::H + h] = v;
                }
        }
        return true;
    } catch (const std::exception& e) { error = e.what(); impl_->failed = true; return false; }
}
bool NetworkExperts::owns(int64_t row) const {
    return row >= 0 && static_cast<size_t>(row) < impl_->owned.size() && impl_->owned[static_cast<size_t>(row)];
}
void NetworkExperts::report() const {
    for (const auto& w : impl_->workers)
        std::fprintf(stderr, "strata network: %s requests=%llu entries=%llu tx=%llu rx=%llu roundtrip_ms=%.3f\n",
                     w->endpoint.c_str(), static_cast<unsigned long long>(w->requests),
                     static_cast<unsigned long long>(w->entries), static_cast<unsigned long long>(w->tx),
                     static_cast<unsigned long long>(w->rx), w->ms);
}
}  // namespace strata::core
