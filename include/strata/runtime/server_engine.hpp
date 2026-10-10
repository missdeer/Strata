#pragma once
#include "strata/runtime/engine_channel.hpp"
#include "strata/runtime/text.hpp"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <algorithm>

namespace strata::runtime {
struct ApiError : std::runtime_error {
    int status;
    std::string kind, param, code;
    TextJson details = TextJson::object();
    ApiError(int status_, const std::string &message, std::string kind_ = "invalid_request_error",
             std::string param_ = {}, std::string code_ = {})
        : std::runtime_error(message), status(status_), kind(std::move(kind_)), param(std::move(param_)),
          code(std::move(code_)) {}
    TextJson body(bool anthropic = false) const;
};
void validate_server_config(const TextJson &cfg);
std::vector<std::string> server_engine_args(const TextJson &cfg);
std::vector<int> server_gpu_indices(const TextJson &cfg);
std::vector<int> server_ordered_gpus(const TextJson &cfg);
std::string server_layer_split(const TextJson &cfg);
std::vector<std::string> server_aliases(const TextJson &cfg);
void apply_engine_environment(const TextJson &cfg);
std::map<std::string, std::string> child_environment(const TextJson &cfg);
std::vector<std::string> child_environment_removed(const TextJson &cfg);
std::string server_environment(const char *name);
std::string sampling_keys(const TextJson &sampling);

class ServerEngine {
  public:
    class Lease;

  private:
    TextJson cfg_;
    std::shared_ptr<EngineChannel> process_;
    std::atomic<uint64_t> process_epoch_{0};
    std::recursive_mutex lifecycle_mutex_;
    std::atomic<int> batch_{0}, parallel_leases_{0}, slot_waiters_{0};
    int requested_batch_ = 0, slot_groups_ = 1;
    uint64_t slot_clock_ = 0;
    struct Slot {
        bool busy = false, engine_active = false;
        std::vector<int> held;
        uint64_t used = 0;
        TextJson live = nullptr;
        std::weak_ptr<EngineChannel> owner;
    };
    std::vector<Slot> slots_;
    std::mutex slots_mutex_;
    std::condition_variable slots_changed_;
    std::mutex queue_mutex_, state_mutex_;
    std::condition_variable queue_changed_;
    std::deque<uint64_t> queue_;
    std::map<uint64_t, size_t> waiting_lengths_;
    uint64_t next_ = 0;
    bool occupied_ = false;
    bool maintenance_ = false;
    std::atomic<bool> can_stop_{false};
    std::atomic<bool> shutting_down_{false}, loaded_{false};
    std::atomic<bool> unloaded_{false};
    std::atomic<int> context_{4096};
    TextJson state_ = TextJson::object();
    double silence_ = 300, ready_timeout_ = 900, stop_timeout_ = 300;
    double stall_timeout_ = 90;
    std::function<bool()> gpu_busy_;
    struct Watch {
        std::optional<std::pair<double, uint64_t>> baseline;
        double next = 0;
        bool warned = false;
    };
    void check_stall(const std::shared_ptr<EngineChannel> &, std::chrono::steady_clock::time_point, Watch &);
    void start(const std::function<bool()> &cancelled = {});
    void enter(const std::function<bool()> &cancelled, size_t prompt_length = 0);
    void leave();
    void discard(const std::shared_ptr<EngineChannel> &expected = {});
    TextJson generate_batch(const std::vector<int> &, int, const TextJson &, const std::function<bool(int)> &,
                            const std::function<bool()> &, const std::string &);
    TextJson generate_batch_slot(const std::vector<int> &, int, const TextJson &,
                                 const std::function<bool(int)> &, const std::function<bool()> &,
                                 const std::string &, int reserved = -1, bool allow_solo = false);
    TextJson generate_serial(const std::vector<int> &, int, const TextJson &,
                             const std::function<bool(int)> &, const std::function<bool()> &, const Lease *,
                             const std::string &, int *yield_slot = nullptr,
                             const std::function<void()> &started = {});
    bool shorter_waiting(size_t);
    std::function<void()> before_start_, after_unload_;
    bool started_once_ = false;
    std::atomic<double> last_activity_{0};
    std::thread idle_thread_;
    std::mutex idle_mutex_;
    std::condition_variable idle_changed_;
    std::filesystem::path log_path_;
    uint64_t log_start_ = 0, log_position_ = 0, log_epoch_ = 0;
    std::mutex cache_mutex_;
    TextJson cache_state_ = TextJson::object();
    TextJson vram_reserve_ = nullptr;
    TextJson exchange(const std::string &, const std::string &, const std::function<bool()> &);

  public:
    class Lease {
        friend class ServerEngine;
        ServerEngine *owner_;
        bool exclusive_;
        bool lifecycle_ = false;
        explicit Lease(ServerEngine *owner, bool exclusive = true) : owner_(owner), exclusive_(exclusive) {}

      public:
        ~Lease() {
            if (lifecycle_)
                owner_->lifecycle_mutex_.unlock();
            if (exclusive_)
                owner_->leave();
            else {
                owner_->last_activity_ =
                    std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
                        .count();
                --owner_->parallel_leases_;
                owner_->slots_changed_.notify_all();
            }
        }
        Lease(const Lease &) = delete;
        Lease &operator=(const Lease &) = delete;
    };
    std::unique_ptr<Lease> acquire(const std::function<bool()> &cancelled);
    std::unique_ptr<Lease> exclusive(const std::function<bool()> &cancelled);
    void ensure_loaded(const Lease *lease, const std::function<bool()> &cancelled);
    explicit ServerEngine(TextJson cfg, std::function<void()> before_start = {},
                          std::function<void()> after_unload = {});
    ~ServerEngine();
    int context() const { return context_.load(); }
    bool loaded() const { return loaded_.load(); }
    bool unloaded() const { return unloaded_.load(); }
    void set_gpu_busy(std::function<bool()> callback) { gpu_busy_ = std::move(callback); }
    int parallel() const { return batch_.load(); }
    int asked_parallel() const { return std::max(1, requested_batch_); }
    TextJson snapshot();
    TextJson cache_view();
    // Called for tokens and heartbeats (token == -1). False requests STOP.
    TextJson generate(const std::vector<int> &ids, int max_new, const TextJson &sampling,
                      const std::function<bool(int)> &token, const std::function<bool()> &cancelled,
                      const Lease *lease = nullptr, const std::string &embeddings = {});
    TextJson control(
        const std::string &action, const std::function<bool()> &cancelled = [] { return false; },
        bool idle = false);
    TextJson vram(const TextJson &reserve, const std::function<bool()> &cancelled);
    TextJson session_file(const std::string &action, const std::filesystem::path &path,
                          const std::function<bool()> &cancelled);
    void shutdown();
};
} // namespace strata::runtime
