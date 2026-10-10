// Native pipe fixture. Python drives HTTP tests; neither service nor child uses Python.
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
namespace {
std::string setting(const char *key, const char *fallback = "") {
    auto p = std::getenv(key);
    return p ? p : fallback;
}
std::mutex output_mutex;
void print(const std::string &s) {
    std::lock_guard<std::mutex> l(output_mutex);
    std::cout << s << std::endl;
}
std::vector<int> parse(const std::string &s) {
    std::vector<int> out;
    std::istringstream in(s);
    std::string t;
    while (std::getline(in, t, ','))
        if (!t.empty())
            out.push_back(std::stoi(t));
    return out;
}
} // namespace
int main(int argc, char **argv) {
    bool elastic = false;
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == "--vram-elastic")
            elastic = true;
    auto script = setting("STRATA_TEST_TOKENS");
    const auto script_file = setting("STRATA_TEST_SCRIPT_FILE");
    if (!script_file.empty()) {
        std::ifstream file(std::filesystem::u8path(script_file));
        script.assign(std::istreambuf_iterator<char>(file), {});
    }
    const auto request_file = setting("STRATA_TEST_REQUEST_FILE");
    const auto counter_file = setting("STRATA_TEST_COUNTER_FILE");
    int requests = 0;
    if (!counter_file.empty()) {
        std::ifstream file(std::filesystem::u8path(counter_file));
        file >> requests;
    }
    if (setting("STRATA_TEST_READY_DELAY") != "")
        std::this_thread::sleep_for(std::chrono::milliseconds(std::stoi(setting("STRATA_TEST_READY_DELAY"))));
    print("INFO engine=test-native batch_slots=0");
    print("READY " + setting("STRATA_TEST_CONTEXT", "4096") + " stop");
    std::thread worker;
    std::atomic<bool> stop{false};
    std::vector<int> held;
    int request = 0;
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line == "STOP") {
            stop = true;
            continue;
        }
        if (line == "QUIT") {
            stop = true;
            break;
        }
        if (line == "VRAM" || line.starts_with("VRAM ")) {
            if (!elastic)
                print("ERR the engine was not started with --vram-elastic");
            else
                print("VRAM expert_slots=16 expert_cache_mib=32 vram_free_mib=2048 reserve_mib=" +
                      (line == "VRAM" ? std::string("700") : line.substr(5)));
            continue;
        }
        if (line.starts_with("SAVE ") || line.starts_with("RESTORE ")) {
            if (worker.joinable())
                worker.join();
            const bool save = line.starts_with("SAVE ");
            const auto path = line.substr(save ? 5 : 8);
            if (save) {
                std::ofstream out(std::filesystem::u8path(path), std::ios::binary);
                if (!out) {
                    print("SERR io 0 write failed");
                    continue;
                }
                for (auto t : held)
                    out << t << ',';
                out.close();
            } else {
                std::ifstream in(std::filesystem::u8path(path), std::ios::binary);
                if (!in) {
                    print("SERR invalid 0 no saved session");
                    continue;
                }
                std::string content((std::istreambuf_iterator<char>(in)), {});
                held = parse(content);
            }
            print(std::string(save ? "SAVED " : "RESTORED ") + std::to_string(held.size()) + " " +
                  std::to_string(std::filesystem::file_size(std::filesystem::u8path(path))) + " 1");
            continue;
        }
        if (!line.starts_with("GEN ") && !line.starts_with("GENI ")) {
            print("ERR unexpected command");
            continue;
        }
        ++requests;
        if (!counter_file.empty()) {
            std::ofstream file(std::filesystem::u8path(counter_file));
            file << requests;
        }
        const auto crash_once = setting("STRATA_TEST_CRASH_ONCE");
        if (!crash_once.empty() && !std::filesystem::exists(crash_once)) {
            std::ofstream marker(crash_once);
            marker << "crashed";
            return 7;
        }
        if (worker.joinable())
            worker.join();
        stop = false;
        if (!request_file.empty()) {
            std::ifstream response_file(std::filesystem::u8path(request_file));
            std::string response((std::istreambuf_iterator<char>(response_file)), {});
            if (response == "FATAL") {
                print("ERR verify: timed out at layer 18; test failure");
                print("DONE 0 1 0 1 stop");
                continue;
            }
            if (response == "REFUSE") {
                print("ERR prompt exceeds context");
                continue;
            }
            if (response == "MALFORMED") {
                print("T invalid");
                continue;
            }
        }
        const auto capture = setting("STRATA_TEST_CAPTURE");
        if (!capture.empty()) {
            std::ofstream file(capture, std::ios::app);
            file << line << '\n';
        }
        std::istringstream in(line);
        std::string word, ids;
        int max_new = 0;
        in >> word >> max_new;
        while (in >> word)
            ids = word;
        const auto prompt = parse(ids);
        int reused = 0;
        while (reused < static_cast<int>(prompt.size()) && reused < static_cast<int>(held.size()) &&
               prompt[reused] == held[reused])
            ++reused;
        reused = std::min(reused, std::max(0, static_cast<int>(prompt.size()) - 1));
        held = prompt;
        std::istringstream scripts(script);
        std::string chosen, part;
        int at = 0;
        while (std::getline(scripts, part, ';')) {
            chosen = part;
            if (at++ == request)
                break;
        }
        ++request;
        if (chosen == "CRASH")
            return 7;
        worker = std::thread([&, chosen, max_new, prompt, reused] {
            print("RESUME " + std::to_string(reused));
            print("PP " + std::to_string(prompt.size()) + " " + std::to_string(prompt.size()) + " 1 1000");
            if (chosen == "STALL") {
                while (!stop)
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            auto tokens = chosen == "STALL" ? std::vector<int>() : parse(chosen);
            int n = 0;
            for (int id : tokens) {
                if (stop || n >= max_new)
                    break;
                print("T " + std::to_string(id));
                held.push_back(id);
                ++n;
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(std::stoi(setting("STRATA_TEST_DELAY", "0"))));
            }
            print("DONE " + std::to_string(n) + " " + std::to_string(prompt.size()) + " 1 2 " +
                  (stop           ? "cancel"
                   : n >= max_new ? "length"
                                  : "stop") +
                  " 0 0 " + std::to_string(reused) + " 0 0 0 0 0 0 0");
        });
    }
    stop = true;
    if (worker.joinable())
        worker.join();
    return 0;
}
