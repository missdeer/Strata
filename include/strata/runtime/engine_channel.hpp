#pragma once
#include "strata/runtime/engine_process.hpp"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <array>
#include <functional>

namespace strata::runtime {
// A process owns its dispatcher. Callers retain this object across restart, so
// an old request can never consume a successor process's control/slot messages.
class EngineChannel {
    EngineProcess process_;
    std::mutex mutex_, write_mutex_;
    std::condition_variable changed_;
    std::deque<std::string> control_;
    std::array<std::deque<std::string>, 256> slots_;
    std::thread reader_;
    std::atomic<bool> stopped_{false};
    bool ended_ = false;
    bool invalidated_ = false;
    std::function<void()> on_end_;
    std::string failure_;
    bool receive_queue(std::string &, std::chrono::milliseconds, int);

  public:
    EngineChannel(const std::vector<std::string> &, const std::filesystem::path &,
                  const std::filesystem::path &, const std::map<std::string, std::string> &environment = {},
                  std::function<void()> on_end = {},
                  const std::vector<std::string> &removed_environment = {});
    ~EngineChannel();
    void send(const std::string &);
    bool receive(std::string &, std::chrono::milliseconds);
    bool receive_slot(int, std::string &, std::chrono::milliseconds);
    bool alive();
    std::optional<std::pair<double, uint64_t>> activity();
    void finish() noexcept;
    void close() noexcept;
};
} // namespace strata::runtime
