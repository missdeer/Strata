#pragma once
#include "strata/runtime/text.hpp"
#include <functional>
namespace strata::runtime {
class ServerTelemetry {
    struct Impl;
    std::unique_ptr<Impl> impl_;

  public:
    ServerTelemetry(int gpu, bool amd, std::function<TextJson()> extra);
    ~ServerTelemetry();
    TextJson snapshot();
};
} // namespace strata::runtime
