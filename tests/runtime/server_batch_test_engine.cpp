#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>
namespace {
std::mutex output;
std::string env(const char *key, const char *fallback = "") {
    auto p = std::getenv(key);
    return p ? p : fallback;
}
void line(const std::string &text) {
    std::lock_guard<std::mutex> lock(output);
    std::cout << text << std::endl;
}
std::vector<int> tokens(const std::string &raw) {
    std::vector<int> out;
    std::istringstream in(raw);
    std::string word;
    while (std::getline(in, word, ','))
        if (!word.empty())
            out.push_back(std::stoi(word));
    return out;
}
struct Task {
    std::vector<int> prompt, answer;
    std::atomic<bool> complete{false};
};
} // namespace
int main(int argc, char **argv) {
    int count = 2;
    for (int i = 1; i + 1 < argc; ++i)
        if (std::string(argv[i]) == "--batch" || std::string(argv[i]) == "--slots")
            count = std::stoi(argv[i + 1]);
    line("INFO engine=batch-test batch_slots=" + std::to_string(count) + " slot_cache=1 batch_groups=1");
    line("READY 4096 stop");
    std::array<std::thread, 256> workers;
    std::array<std::atomic<bool>, 256> stop{};
    std::array<std::vector<int>, 256> held;
    std::thread solo;
    std::atomic<bool> solo_stop{false};
    std::atomic<int> yield{-1};
    std::vector<std::shared_ptr<Task>> tasks;
    std::vector<int> solo_held;
    int request = 0;
    std::string command;
    const auto script = env("STRATA_TEST_TOKENS");
    while (std::getline(std::cin, command)) {
        if (command == "QUIT")
            break;
        if (command == "STOP") {
            solo_stop = true;
            continue;
        }
        if (command.starts_with("BYIELD ")) {
            yield = std::stoi(command.substr(7));
            solo_stop = true;
            continue;
        }
        if (command.starts_with("BSTOP ")) {
            stop[std::stoi(command.substr(6))] = true;
            continue;
        }
        const bool batch = command.starts_with("BGEN ") || command.starts_with("BGENI ");
        if (!batch && !command.starts_with("GEN ") && !command.starts_with("GENI ")) {
            line("ERR unexpected command");
            continue;
        }
        std::istringstream in(command);
        std::string kind, word, raw_ids;
        int slot = -1, max_new = 0;
        in >> kind;
        if (batch)
            in >> slot;
        in >> max_new;
        while (in >> word)
            raw_ids = word;
        const auto prompt = tokens(raw_ids);
        if (batch && (slot < 0 || slot >= count)) {
            line("ERR invalid slot");
            continue;
        }
        if (batch && workers[slot].joinable())
            workers[slot].join();
        if (!batch && solo.joinable())
            solo.join();
        std::shared_ptr<Task> task;
        size_t offset = 0;
        for (auto it = tasks.rbegin(); it != tasks.rend(); ++it) {
            const auto &previous = *it;
            if (previous->complete || prompt.size() < previous->prompt.size() ||
                !std::equal(previous->prompt.begin(), previous->prompt.end(), prompt.begin()))
                continue;
            const auto generated = prompt.size() - previous->prompt.size();
            if (generated <= previous->answer.size() && std::equal(prompt.begin() + previous->prompt.size(),
                                                                   prompt.end(), previous->answer.begin())) {
                task = previous;
                offset = generated;
                break;
            }
        }
        if (!task) {
            std::istringstream scripts(script);
            std::string chosen, part;
            int i = 0;
            while (std::getline(scripts, part, ';')) {
                chosen = part;
                if (i++ == request)
                    break;
            }
            ++request;
            if (chosen == "CRASH")
                return 7;
            task = std::make_shared<Task>();
            task->prompt = prompt;
            task->answer = tokens(chosen);
            if (task->answer.empty())
                task->answer.push_back(0);
            tasks.push_back(task);
        }
        if (offset >= task->answer.size())
            offset = task->answer.size() - 1;
        auto &cache = batch ? held[slot] : solo_held;
        int reused = 0;
        while (reused < static_cast<int>(prompt.size()) && reused < static_cast<int>(cache.size()) &&
               prompt[reused] == cache[reused])
            ++reused;
        reused = std::min(reused, std::max(0, static_cast<int>(prompt.size()) - 1));
        cache = prompt;
        if (!batch) {
            solo_stop = false;
            yield = -1;
            solo = std::thread([&, task, prompt, offset, max_new, reused] {
                line("PP " + std::to_string(prompt.size()) + " " + std::to_string(prompt.size()) + " 1 1000");
                int produced = 0;
                const auto delay = std::stoi(env("STRATA_TEST_DELAY", "3"));
                for (size_t i = offset; i < task->answer.size() && produced < max_new; ++i) {
                    if (solo_stop)
                        break;
                    line("T " + std::to_string(task->answer[i]));
                    solo_held.push_back(task->answer[i]);
                    ++produced;
                    std::this_thread::sleep_for(std::chrono::milliseconds(delay));
                }
                if (yield >= 0)
                    line("YIELDED " + std::to_string(yield.load()) + " " + std::to_string(prompt.size()));
                const auto finish = solo_stop ? "cancel" : produced >= max_new ? "length" : "stop";
                if (!solo_stop || offset + static_cast<size_t>(produced) >= task->answer.size() ||
                    produced >= max_new)
                    task->complete = true;
                line("DONE " + std::to_string(produced) + " " + std::to_string(prompt.size()) + " 1 2 " +
                     finish + " 0 0 " + std::to_string(reused) + " 0 0 0 0 0 " +
                     std::to_string(prompt.size() - reused) + " 0");
            });
            continue;
        }
        stop[slot] = false;
        line("PP " + std::to_string(prompt.size()) + " " + std::to_string(prompt.size()) + " 1 1000");
        line("T " + std::to_string(task->answer[offset]));
        const bool more = max_new > 1 && offset + 1 < task->answer.size();
        line("DONE 1 " + std::to_string(prompt.size()) + " 1 1 " + (more ? "length" : "stop") + " 0 0 " +
             std::to_string(reused) + " 0 0 0 0 0 " + std::to_string(prompt.size() - reused) + " 0");
        line("BADM " + std::to_string(slot) + (more ? " 1" : " 0"));
        if (!more) {
            task->complete = true;
            continue;
        }
        workers[slot] = std::thread([&, slot, task, offset, max_new] {
            int produced = 1;
            held[slot].push_back(task->answer[offset]);
            for (size_t i = offset + 1; i < task->answer.size() && produced < max_new; ++i) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(std::stoi(env("STRATA_TEST_DELAY", "3"))));
                if (stop[slot])
                    break;
                line("BT " + std::to_string(slot) + " " + std::to_string(task->answer[i]));
                held[slot].push_back(task->answer[i]);
                ++produced;
            }
            if (!stop[slot] || offset + static_cast<size_t>(produced) >= task->answer.size() ||
                produced >= max_new)
                task->complete = true;
            line("BDONE " + std::to_string(slot) + " " + std::to_string(produced) + " " +
                 (stop[slot]            ? "cancel"
                  : produced >= max_new ? "length"
                                        : "stop") +
                 " 2");
        });
    }
    solo_stop = true;
    for (auto &s : stop)
        s = true;
    if (solo.joinable())
        solo.join();
    for (auto &worker : workers)
        if (worker.joinable())
            worker.join();
}
