#pragma once
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>
#include <map>
#include <optional>
#include <cstdint>

namespace strata::runtime {
// One reader and one writer; close() is called after both have stopped.
class EngineProcess {
    struct Impl;
    std::unique_ptr<Impl> impl_;

  public:
    EngineProcess(const std::vector<std::string> &argv, const std::filesystem::path &cwd,
                  const std::filesystem::path &log,
                  const std::map<std::string, std::string> &environment = {}, size_t maximum_line = 1u << 20,
                  const std::vector<std::string> &removed_environment = {});
    ~EngineProcess();
    EngineProcess(const EngineProcess &) = delete;
    EngineProcess &operator=(const EngineProcess &) = delete;
    void send(const std::string &line);
    // False is a timeout, EOF throws. Lines and buffered output have fixed limits.
    bool receive(std::string &line, std::chrono::milliseconds timeout);
    bool alive() const;
    int exit_code() const;
    std::optional<std::pair<double, uint64_t>> activity() const;
    void finish() noexcept;
    void close() noexcept;
};
} // namespace strata::runtime
