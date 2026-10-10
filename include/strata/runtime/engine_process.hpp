#pragma once
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace strata::runtime {
// One reader and one writer; close() is called after both have stopped.
class EngineProcess {
    struct Impl;
    std::unique_ptr<Impl> impl_;

  public:
    EngineProcess(const std::vector<std::string> &argv, const std::filesystem::path &cwd,
                  const std::filesystem::path &log);
    ~EngineProcess();
    EngineProcess(const EngineProcess &) = delete;
    EngineProcess &operator=(const EngineProcess &) = delete;
    void send(const std::string &line);
    // False is a timeout, EOF throws. Lines and buffered output have fixed limits.
    bool receive(std::string &line, std::chrono::milliseconds timeout);
    void finish() noexcept;
    void close() noexcept;
};
} // namespace strata::runtime
