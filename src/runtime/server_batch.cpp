#include "strata/runtime/server_engine.hpp"
#include <algorithm>
#include <cmath>
#include <sstream>
namespace strata::runtime {
namespace {
using J = TextJson;
using Clock = std::chrono::steady_clock;
struct Finally {
    std::function<void()> f;
    ~Finally() { f(); }
};
double seconds(Clock::time_point t) { return std::chrono::duration<double>(Clock::now() - t).count(); }
J done_line(std::istringstream &in) {
    int64_t generated = 0, prompt = 0;
    double pp = 0, decode = 0;
    std::string finish;
    if (!(in >> generated >> prompt >> pp >> decode >> finish) || generated < 0 || prompt < 0 ||
        !std::isfinite(pp) || !std::isfinite(decode) || pp < 0 || decode < 0 ||
        (finish != "stop" && finish != "length" && finish != "cancel"))
        throw std::runtime_error("invalid batch DONE");
    J result{{"generated", generated}, {"prompt_tokens", prompt}, {"prompt_ms", pp},
             {"decode_ms", decode},    {"finish", finish},        {"reused", 0}};
    for (auto key :
         {"drafts_accepted", "drafts_offered", "reused", "hits", "lookups", "ram_blobs", "file_blobs"}) {
        int64_t n = 0;
        if (!(in >> n))
            return result;
        if (n < 0)
            throw std::runtime_error("negative engine counter");
        result[key] = n;
    }
    double mb = 0;
    if (in >> mb) {
        if (!std::isfinite(mb) || mb < 0)
            throw std::runtime_error("invalid file byte count");
        result["file_mb"] = mb;
    }
    int64_t n = 0;
    if (in >> n)
        result["prompt_read"] = n;
    if (in >> n)
        result["offloaded"] = n;
    return result;
}
} // namespace
TextJson ServerEngine::generate_batch(const std::vector<int> &ids, int max_new, const J &sampling,
                                      const std::function<bool(int)> &token,
                                      const std::function<bool()> &cancelled, const std::string &embeddings) {
    std::vector<int> observed;
    J reused = nullptr;
    int solo_again = 0;
    bool solo = max_new > 1;
    {
        std::lock_guard<std::mutex> lock(slots_mutex_);
        solo = solo && !std::any_of(slots_.begin(), slots_.end(), [](const Slot &s) { return s.busy; });
    }
    auto deliver = [&](int id) {
        if (id >= 0)
            observed.push_back(id);
        return token(id);
    };
    for (;;) {
        std::vector<int> prompt = ids;
        prompt.insert(prompt.end(), observed.begin(), observed.end());
        const auto left = max_new - static_cast<int>(observed.size());
        if (left <= 0)
            return {{"finish", "length"},
                    {"generated", observed.size()},
                    {"prompt_tokens", ids.size()},
                    {"reused", reused.is_null() ? J(0) : reused}};
        J last = nullptr;
        int reserved = -1, yield_to = -1;
        bool promoted = false, armed = false;
        std::shared_ptr<EngineChannel> owner;
        {
            std::lock_guard<std::recursive_mutex> lock(lifecycle_mutex_);
            owner = process_;
        }
        Finally cleanup{[&] {
            if (reserved < 0)
                return;
            std::lock_guard<std::mutex> lock(slots_mutex_);
            if (reserved < static_cast<int>(slots_.size()) && slots_[reserved].owner.lock() == owner) {
                slots_[reserved].busy = false;
                slots_[reserved].engine_active = false;
                slots_[reserved].live = nullptr;
                slots_changed_.notify_all();
            }
        }};
        if (solo) {
            const size_t before = observed.size();
            auto stop = [&] {
                if (cancelled() || shutting_down_)
                    return true;
                if (!armed || !can_stop_)
                    return false;
                if (promoted)
                    return true;
                bool waiting = false;
                {
                    std::lock_guard<std::mutex> lock(queue_mutex_);
                    waiting = !queue_.empty();
                }
                waiting = waiting || slot_waiters_ > 0;
                if (!waiting)
                    return false;
                const bool prefill = observed.size() == before;
                if (prefill && !shorter_waiting(prompt.size()))
                    return false;
                promoted = true;
                if (embeddings.empty()) {
                    std::lock_guard<std::mutex> lock(slots_mutex_);
                    for (size_t i = 0; i < slots_.size(); ++i)
                        if (!slots_[i].busy) {
                            reserved = static_cast<int>(i);
                            auto &entry = slots_[i];
                            entry.busy = true;
                            entry.owner = owner;
                            entry.held.clear();
                            entry.live = {{"slot", i},
                                          {"state", "reading"},
                                          {"prompt_tokens", prompt.size()},
                                          {"generated", 0}};
                            if (prefill)
                                yield_to = reserved;
                            break;
                        }
                }
                return true;
            };
            try {
                last = generate_serial(
                    prompt, left, sampling, deliver, stop, nullptr, embeddings, &yield_to, [&] {
                        std::lock_guard<std::mutex> lock(slots_mutex_);
                        if (std::any_of(slots_.begin(), slots_.end(), [](const Slot &s) { return s.busy; })) {
                            promoted = true;
                            throw ApiError(499, "batch admission required", "server_error", {},
                                           "batch_admission");
                        }
                        armed = true;
                    });
            } catch (const ApiError &error) {
                if (error.code != "batch_admission")
                    throw;
            }
            if (!last.is_null()) {
                if (reused.is_null())
                    reused = last.value("reused", J(nullptr));
                if (!promoted || cancelled() || last.value("finish", "") == "stop" ||
                    last.value("finish", "") == "length" || observed.size() >= static_cast<size_t>(max_new)) {
                    last.erase("_yielded_slot");
                    last.erase("_yielded_tokens");
                    last["prompt_tokens"] = ids.size();
                    if (!reused.is_null())
                        last["reused"] = reused;
                    return last;
                }
                if (yield_to >= 0 && last.value("_yielded_slot", -1) != yield_to) {
                    std::lock_guard<std::mutex> lock(slots_mutex_);
                    if (reserved < static_cast<int>(slots_.size()) &&
                        slots_[reserved].owner.lock() == owner) {
                        slots_[reserved].busy = false;
                        slots_[reserved].live = nullptr;
                        slots_changed_.notify_all();
                    }
                    reserved = -1;
                }
            }
            prompt = ids;
            prompt.insert(prompt.end(), observed.begin(), observed.end());
        }
        const auto transfer = reserved;
        reserved = -1;
        last = generate_batch_slot(prompt, max_new - static_cast<int>(observed.size()), sampling, deliver,
                                   cancelled, embeddings, transfer, solo_again < 2);
        if (reused.is_null())
            reused = last.value("reused", J(nullptr));
        const bool again = last.value("_continue_solo", false);
        last.erase("_continue_solo");
        last["prompt_tokens"] = ids.size();
        if (!reused.is_null())
            last["reused"] = reused;
        if (!again || cancelled() || observed.size() >= static_cast<size_t>(max_new))
            return last;
        solo = true;
        ++solo_again;
    }
}
TextJson ServerEngine::generate_batch_slot(const std::vector<int> &ids, int max_new, const J &sampling,
                                           const std::function<bool(int)> &token,
                                           const std::function<bool()> &cancelled,
                                           const std::string &embeddings, int reserved_slot,
                                           bool allow_solo) {
    if (static_cast<int64_t>(ids.size()) + max_new + 8 > context())
        throw ApiError(400, "prompt and max_tokens exceed engine context");
    std::shared_ptr<EngineChannel> channel;
    uint64_t epoch = 0;
    {
        std::lock_guard<std::recursive_mutex> l(lifecycle_mutex_);
        channel = process_;
        epoch = process_epoch_;
    }
    if (!channel || !loaded())
        throw ApiError(503, "engine is unavailable", "server_error");
    int slot = reserved_slot;
    if (slot < 0) {
        ++slot_waiters_;
        try {
            std::unique_lock<std::mutex> lock(slots_mutex_);
            for (;;) {
                if (cancelled() || shutting_down_)
                    throw ApiError(499, "request cancelled", "server_error");
                if (!loaded())
                    throw ApiError(503, "engine stopped while waiting for a batch slot", "server_error");
                if (epoch != process_epoch_)
                    throw ApiError(503, "engine restarted while waiting; this request was not sent",
                                   "server_error");
                const int n = static_cast<int>(slots_.size()),
                          groups = n > 0 && n % slot_groups_ == 0 ? slot_groups_ : 1,
                          gs = groups ? n / groups : n;
                std::vector<int> free;
                for (int t = 0; t < gs; ++t)
                    for (int g = 0; g < groups; ++g) {
                        const auto b = g * gs + t;
                        if (!slots_[b].busy)
                            free.push_back(b);
                    }
                if (!free.empty()) {
                    if (groups > 1) {
                        const auto hole = [&](int b) {
                            for (int x = b + 1; x < (b / gs + 1) * gs; ++x)
                                if (slots_[x].busy)
                                    return true;
                            return false;
                        };
                        slot = *std::min_element(free.begin(), free.end(),
                                                 [&](int a, int b) { return hole(a) > hole(b); });
                    } else {
                        auto prefix = [&](int b) {
                            const auto &held = slots_[b].held;
                            return !held.empty() && held.size() < ids.size() &&
                                           std::equal(held.begin(), held.end(), ids.begin())
                                       ? held.size()
                                       : size_t(0);
                        };
                        slot = *std::max_element(free.begin(), free.end(),
                                                 [&](int a, int b) { return prefix(a) < prefix(b); });
                        if (!prefix(slot))
                            slot = *std::min_element(free.begin(), free.end(), [&](int a, int b) {
                                return std::pair{!slots_[a].held.empty(), slots_[a].used} <
                                       std::pair{!slots_[b].held.empty(), slots_[b].used};
                            });
                    }
                    auto &entry = slots_[slot];
                    entry.busy = true;
                    entry.owner = channel;
                    entry.held.clear();
                    entry.live = {{"slot", slot},
                                  {"state", "reading"},
                                  {"prompt_tokens", ids.size()},
                                  {"generated", 0}};
                    break;
                }
                slots_changed_.wait_for(lock, std::chrono::milliseconds(100));
            }
        } catch (...) {
            --slot_waiters_;
            throw;
        }
        --slot_waiters_;
    } else {
        std::lock_guard<std::mutex> lock(slots_mutex_);
        if (slot >= static_cast<int>(slots_.size()) || !slots_[slot].busy ||
            slots_[slot].owner.lock() != channel)
            throw ApiError(503, "yielded slot belonged to an ended engine", "server_error");
        slots_[slot].live = {
            {"slot", slot}, {"state", "reading"}, {"prompt_tokens", ids.size()}, {"generated", 0}};
    }
    std::vector<int> raw;
    bool completed = false, stopping = false;
    auto heard = Clock::now(), stopped_at = heard, beat = heard;
    Finally release{[&] {
        std::lock_guard<std::mutex> lock(slots_mutex_);
        if (slot < static_cast<int>(slots_.size()) && slots_[slot].owner.lock() == channel) {
            auto &entry = slots_[slot];
            entry.busy = false;
            entry.engine_active = false;
            entry.used = ++slot_clock_;
            entry.live = nullptr;
            entry.held = completed && embeddings.empty() ? ids : std::vector<int>();
            if (completed && embeddings.empty() && !raw.empty())
                entry.held.insert(entry.held.end(), raw.begin(), raw.end() - 1);
        }
        slots_changed_.notify_all();
    }};
    J last = nullptr;
    bool continuation = false, going_solo = false;
    Watch watch;
    int yields = 0;
    try {
        for (;;) {
            enter(cancelled, ids.size());
            Finally control{[&] { leave(); }};
            bool yielded = false, asked = false;
            int64_t read_at = 0;
            {
                std::lock_guard<std::recursive_mutex> l(lifecycle_mutex_);
                if (process_ != channel || !loaded())
                    throw std::runtime_error("batch process changed before admission");
            }
            std::string command = (embeddings.empty() ? "BGEN " : "BGENI ") + std::to_string(slot) + " " +
                                  std::to_string(max_new) + sampling_keys(sampling) + " " +
                                  (embeddings.empty() ? std::string() : embeddings + " ");
            {
                std::lock_guard<std::mutex> lock(slots_mutex_);
                slots_[slot].engine_active = true;
            }
            for (size_t i = 0; i < ids.size(); ++i) {
                if (i)
                    command += ',';
                command += std::to_string(ids[i]);
            }
            channel->send(command);
            heard = beat = Clock::now();
            auto allowance = silence_ + std::min<size_t>(ids.size(), 32768) / 50.;
            for (;;) {
                if (!stopping && (cancelled() || shutting_down_)) {
                    stopping = true;
                    stopped_at = Clock::now();
                    channel->send("STOP");
                }
                if ((stopping && seconds(stopped_at) > stop_timeout_) ||
                    (silence_ > 0 && seconds(heard) > allowance))
                    throw std::runtime_error("batch admission deadline exceeded");
                std::string line;
                if (!channel->receive(line, std::chrono::milliseconds(100))) {
                    check_stall(channel, heard, watch);
                    if (!stopping && seconds(beat) >= 10) {
                        beat = Clock::now();
                        if (!token(-1)) {
                            stopping = true;
                            stopped_at = Clock::now();
                            channel->send("STOP");
                        }
                    }
                    continue;
                }
                heard = beat = Clock::now();
                watch = Watch{};
                std::istringstream in(line);
                std::string kind;
                in >> kind;
                if (kind == "T") {
                    int id = -1;
                    if (!(in >> id) || id < 0)
                        throw std::runtime_error("invalid admission token");
                    raw.push_back(id);
                    if (!stopping && !token(id)) {
                        stopping = true;
                        stopped_at = Clock::now();
                        channel->send("STOP");
                    }
                } else if (kind == "PP") {
                    int read = 0, total = 0;
                    double ms = 0, rate = 0;
                    in >> read >> total >> ms >> rate;
                    {
                        std::lock_guard<std::mutex> state(state_mutex_);
                        state_["progress"] = {{"read", read}, {"total", total}, {"ms", ms}};
                    }
                    if (!stopping && !token(-1)) {
                        stopping = true;
                        stopped_at = Clock::now();
                        channel->send("STOP");
                    }
                } else if (kind == "YIELDED") {
                    int b = -1;
                    if (!(in >> b >> read_at) || b != slot || read_at < 0 ||
                        read_at > static_cast<int64_t>(ids.size()))
                        throw std::runtime_error("invalid admission yield");
                    yielded = true;
                } else if (kind == "DONE")
                    last = done_line(in);
                else if (kind == "BADM") {
                    int b = -1, cont = -1;
                    if (!(in >> b >> cont) || b != slot || (cont != 0 && cont != 1) || last.is_null())
                        throw std::runtime_error("invalid batch admission result");
                    continuation = cont == 1;
                    break;
                } else if (kind == "ERR") {
                    std::string message;
                    std::getline(in, message);
                    throw ApiError(400, trim_text(message));
                }
                if (kind == "PP" && !asked && !stopping && raw.empty() && embeddings.empty() && yields < 2 &&
                    shorter_waiting(ids.size())) {
                    channel->send("BYIELD " + std::to_string(slot));
                    asked = true;
                }
            }
            if (yielded && !continuation && !cancelled() && !stopping) {
                ++yields;
                {
                    std::lock_guard<std::mutex> lock(slots_mutex_);
                    if (slot < static_cast<int>(slots_.size()) && slots_[slot].owner.lock() == channel) {
                        slots_[slot].held.assign(ids.begin(), ids.begin() + read_at);
                        slots_[slot].engine_active = false;
                    }
                }
                continue;
            }
            break;
        }
    } catch (const ApiError &) {
        throw;
    } catch (...) {
        discard(channel);
        throw ApiError(503, "engine failed during batch admission; this request was not replayed",
                       "server_error");
    }
    if (!continuation) {
        completed = true;
        if (stopping)
            last["finish"] = "cancel";
        return last;
    }
    {
        std::lock_guard<std::mutex> lock(slots_mutex_);
        if (slot < static_cast<int>(slots_.size()) && slots_[slot].owner.lock() == channel)
            slots_[slot].live["state"] = "generating";
    }
    try {
        if (stopping)
            channel->send("BSTOP " + std::to_string(slot));
        heard = beat = Clock::now();
        watch = Watch{};
        for (;;) {
            if (!stopping && (cancelled() || shutting_down_)) {
                stopping = true;
                stopped_at = Clock::now();
                channel->send("BSTOP " + std::to_string(slot));
            }
            if ((stopping && seconds(stopped_at) > stop_timeout_) ||
                (silence_ > 0 && seconds(heard) > silence_))
                throw std::runtime_error("batch slot deadline exceeded");
            std::string line;
            if (!channel->receive_slot(slot, line, std::chrono::milliseconds(100))) {
                check_stall(channel, heard, watch);
                if (!stopping && seconds(beat) >= 10) {
                    beat = Clock::now();
                    if (!token(-1)) {
                        stopping = true;
                        stopped_at = Clock::now();
                        channel->send("BSTOP " + std::to_string(slot));
                    }
                }
                continue;
            }
            heard = beat = Clock::now();
            watch = Watch{};
            std::istringstream in(line);
            std::string kind;
            int b = -1;
            in >> kind >> b;
            if (b != slot)
                throw std::runtime_error("batch result routed to wrong slot");
            if (kind == "BT") {
                int id = -1;
                if (!(in >> id) || id < 0)
                    throw std::runtime_error("invalid batch token");
                raw.push_back(id);
                bool alone = false;
                {
                    std::lock_guard<std::mutex> lock(slots_mutex_);
                    if (slot < static_cast<int>(slots_.size()) && slots_[slot].owner.lock() == channel)
                        slots_[slot].live["generated"] = raw.size();
                    alone = std::count_if(slots_.begin(), slots_.end(),
                                          [](const Slot &s) { return s.busy; }) == 1;
                }
                if (!stopping && !token(id)) {
                    stopping = true;
                    stopped_at = Clock::now();
                    channel->send("BSTOP " + std::to_string(slot));
                }
                bool cache = false;
                {
                    std::lock_guard<std::mutex> lock(state_mutex_);
                    cache = state_["info"].value("slot_cache", 0) != 0;
                }
                bool waiting = false;
                {
                    std::lock_guard<std::mutex> lock(queue_mutex_);
                    waiting = !queue_.empty();
                }
                if (!stopping && !going_solo && allow_solo && cache && alone && !waiting &&
                    slot_waiters_ == 0 && embeddings.empty() &&
                    max_new - static_cast<int>(raw.size()) >= 32 &&
                    server_environment("STRATA_PARALLEL_SOLO") != "0") {
                    channel->send("BSTOP " + std::to_string(slot));
                    going_solo = true;
                }
            } else if (kind == "BDONE") {
                int64_t generated = 0;
                double ms = 0;
                std::string finish;
                if (!(in >> generated >> finish >> ms) || generated < 0 || !std::isfinite(ms) || ms < 0 ||
                    (finish != "stop" && finish != "length" && finish != "cancel"))
                    throw std::runtime_error("invalid batch completion");
                last["generated"] = generated;
                last["decode_ms"] = ms;
                last["finish"] = stopping ? "cancel" : finish;
                if (going_solo && !stopping && finish == "cancel" && !cancelled())
                    last["_continue_solo"] = true;
                completed = true;
                return last;
            } else
                throw std::runtime_error("invalid slot protocol line");
        }
    } catch (...) {
        discard(channel);
        throw ApiError(503, "engine failed during batch decode; this request was not replayed",
                       "server_error");
    }
}
} // namespace strata::runtime
