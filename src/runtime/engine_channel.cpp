#include "strata/runtime/engine_channel.hpp"
#include <sstream>
#include <stdexcept>
namespace strata::runtime {
EngineChannel::EngineChannel(const std::vector<std::string> &args, const std::filesystem::path &cwd,
                             const std::filesystem::path &log,
                             const std::map<std::string, std::string> &environment,
                             std::function<void()> on_end,
                             const std::vector<std::string> &removed_environment)
    : process_(args, cwd, log, environment, 1u << 20, removed_environment), on_end_(std::move(on_end)) {
    reader_ = std::thread([this] {
        try {
            while (!stopped_) {
                std::string line;
                if (!process_.receive(line, std::chrono::milliseconds(100)))
                    continue;
                std::unique_lock<std::mutex> lock(mutex_);
                auto *queue = &control_;
                if (line.starts_with("BT ") || line.starts_with("BDONE ")) {
                    std::istringstream in(line);
                    std::string kind;
                    int slot = -1;
                    in >> kind >> slot;
                    if (slot < 0 || slot >= static_cast<int>(slots_.size()))
                        throw std::runtime_error("invalid engine batch slot");
                    queue = &slots_[slot];
                }
                if (queue->size() >= 262144)
                    throw std::runtime_error("engine output queue exceeds limit");
                // A fatal verifier failure invalidates every slot, even when a
                // broken engine later prints a successful-looking DONE.
                if (line.starts_with("FATAL ") || line.starts_with("ERR verify:") ||
                    line.starts_with("ERR verify batch:") || line.starts_with("ERR native verify:")) {
                    invalidated_ = true;
                    throw std::runtime_error("fatal engine failure");
                }
                queue->push_back(std::move(line));
                changed_.notify_all();
            }
        } catch (const std::exception &e) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                failure_ = e.what();
                ended_ = true;
                changed_.notify_all();
            }
            if (on_end_)
                on_end_();
        }
    });
}
EngineChannel::~EngineChannel() { close(); }
void EngineChannel::send(const std::string &line) {
    std::lock_guard<std::mutex> write(write_mutex_);
    if (stopped_)
        throw std::runtime_error("engine channel closed");
    process_.send(line);
}
bool EngineChannel::receive_queue(std::string &line, std::chrono::milliseconds timeout, int slot) {
    std::unique_lock<std::mutex> lock(mutex_);
    auto &queue = slot < 0 ? control_ : slots_.at(static_cast<size_t>(slot));
    if (!changed_.wait_for(lock, timeout, [&] { return ended_ || stopped_ || !queue.empty(); }))
        return false;
    if (invalidated_ || stopped_ || (ended_ && queue.empty()))
        throw std::runtime_error(failure_.empty() ? "engine channel closed" : failure_);
    line = std::move(queue.front());
    queue.pop_front();
    return true;
}
bool EngineChannel::receive(std::string &line, std::chrono::milliseconds timeout) {
    return receive_queue(line, timeout, -1);
}
bool EngineChannel::receive_slot(int slot, std::string &line, std::chrono::milliseconds timeout) {
    return receive_queue(line, timeout, slot);
}
bool EngineChannel::alive() {
    std::lock_guard<std::mutex> lock(mutex_);
    return !ended_ && !stopped_;
}
std::optional<std::pair<double, uint64_t>> EngineChannel::activity() {
    std::lock_guard<std::mutex> lock(write_mutex_);
    if (stopped_)
        return {};
    return process_.activity();
}
void EngineChannel::finish() noexcept {
    try {
        send("QUIT");
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < end) {
            std::string line;
            receive(line, std::chrono::milliseconds(100));
        }
    } catch (...) {
    }
    close();
}
void EngineChannel::close() noexcept {
    stopped_ = true;
    changed_.notify_all();
    if (reader_.joinable())
        reader_.join();
    std::lock_guard<std::mutex> write(write_mutex_);
    process_.close();
}
} // namespace strata::runtime
