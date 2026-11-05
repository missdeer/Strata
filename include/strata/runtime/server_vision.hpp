#pragma once
#include "strata/runtime/text.hpp"
#include <functional>
namespace strata::runtime {
struct ApiRequest;
class ServerEngine;
struct VisionJob {
    std::filesystem::path path;
    std::string engine_path;
    std::vector<std::shared_ptr<void>> retained;
    ~VisionJob();
};
class NativeVision {
    struct Impl;
    std::unique_ptr<Impl> impl_;

  public:
    explicit NativeVision(const TextJson &config);
    ~NativeVision();
    void ensure_loaded(const std::function<bool()> &cancelled = {});
    void unload();
    std::pair<std::filesystem::path, int> encode(const std::string &normalized,
                                                 const std::function<bool()> &cancelled,
                                                 std::vector<std::shared_ptr<void>> &retained);
    std::filesystem::path directory() const;
    std::string command_path(const std::filesystem::path &) const;
    static std::string load(const std::string &source, const std::function<bool()> &cancelled);
    static std::string normalize(const std::string &bytes);
};
void prepare_vision(ApiRequest &, NativeVision *, ServerEngine &, const Tokenizer &, const ChatTemplate &,
                    int, bool, const std::function<bool()> &);
} // namespace strata::runtime
