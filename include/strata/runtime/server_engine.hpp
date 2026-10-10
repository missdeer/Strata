#pragma once
#include "strata/runtime/engine_process.hpp"
#include "strata/runtime/text.hpp"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>

namespace strata::runtime {
struct ApiError : std::runtime_error {
    int status;
    std::string kind, param, code;
    ApiError(int status_, const std::string &message, std::string kind_ = "invalid_request_error",
             std::string param_ = {}, std::string code_ = {})
        : std::runtime_error(message), status(status_), kind(std::move(kind_)), param(std::move(param_)),
          code(std::move(code_)) {}
    TextJson body(bool anthropic = false) const;
};
void validate_server_config(const TextJson &cfg);
std::vector<std::string> server_engine_args(const TextJson &cfg);
void apply_engine_environment(const TextJson &cfg);
std::string server_environment(const char *name);
std::string sampling_keys(const TextJson &sampling);

class ServerEngine {
    TextJson cfg_;
    std::unique_ptr<EngineProcess> process_;
    std::mutex queue_mutex_, state_mutex_;
    std::condition_variable queue_changed_;
    std::deque<uint64_t> queue_;
    uint64_t next_ = 0;
    bool occupied_ = false, can_stop_ = false;
    std::atomic<bool> shutting_down_{false}, loaded_{false};
    std::atomic<int> context_{4096};
    TextJson state_ = TextJson::object();
    double silence_ = 300, ready_timeout_ = 900, stop_timeout_ = 300;
    void start();
    void enter(const std::function<bool()> &cancelled);
    void leave();
    void discard();

  public:
    class Lease {
        friend class ServerEngine;
        ServerEngine *owner_;
        explicit Lease(ServerEngine *owner) : owner_(owner) {}

      public:
        ~Lease() { owner_->leave(); }
        Lease(const Lease &) = delete;
        Lease &operator=(const Lease &) = delete;
    };
    std::unique_ptr<Lease> acquire(const std::function<bool()> &cancelled);
    explicit ServerEngine(TextJson cfg);
    ~ServerEngine();
    int context() const { return context_.load(); }
    bool loaded() const { return loaded_.load(); }
    TextJson snapshot();
    // Called for tokens and heartbeats (token == -1). False requests STOP.
    TextJson generate(const std::vector<int> &ids, int max_new, const TextJson &sampling,
                      const std::function<bool(int)> &token, const std::function<bool()> &cancelled,
                      const Lease *lease = nullptr);
    TextJson
    control(const std::string &action, const std::function<bool()> &cancelled = [] { return false; });
    void shutdown();
};
} // namespace strata::runtime
