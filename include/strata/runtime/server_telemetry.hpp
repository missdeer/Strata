#pragma once
#include "strata/runtime/text.hpp"
#include <functional>
namespace strata::runtime {
class ServerTelemetry {
    struct Impl;
    std::unique_ptr<Impl> impl_;

  public:
    ServerTelemetry(int gpu, bool amd, std::function<TextJson()> extra, const std::vector<int> &indices = {});
    ~ServerTelemetry();
    TextJson snapshot();
    void close();
};
} // namespace strata::runtime
