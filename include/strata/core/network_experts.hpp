#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace strata::core {

// Optional LAN tier. Results occupy the original router rows; the GPU applies weights once.
class NetworkExperts {
public:
    NetworkExperts();
    ~NetworkExperts();
    NetworkExperts(const NetworkExperts&) = delete;
    NetworkExperts& operator=(const NetworkExperts&) = delete;
    bool open(const std::string& endpoints, const std::string& key, int timeout_ms,
              const std::function<const uint8_t*(int64_t, int64_t)>& blob,
              int64_t layers, int64_t experts, std::string& error,
              const std::string& assignment = {}, const std::string& config_id = {}, bool reduce = false);
    bool begin(int64_t layer, const float* x, const int32_t* ids, int64_t tokens, int64_t k,
               int32_t* kind, std::string& error, const float* weights = nullptr,
               const uint8_t* local = nullptr);
    bool finish(float* out, std::string& error, float* sum = nullptr, int32_t* mask = nullptr);
    bool owns(int64_t row) const;
    void report() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}  // namespace strata::core
