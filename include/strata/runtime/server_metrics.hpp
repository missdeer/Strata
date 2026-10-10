#pragma once
#include "strata/runtime/text.hpp"
#include <mutex>
#include <array>
namespace strata::runtime {
class NativeLatencies {
    std::mutex mutex_;
    struct Histogram {
        std::array<uint64_t, 22> counts{};
        double sum = 0;
        uint64_t n = 0;
    };
    std::array<Histogram, 3> values_;

  public:
    void observe(const TextJson &first, int output, double decode, double wall);
    TextJson snapshot();
};
std::string prometheus_metrics(const TextJson &metrics, const TextJson &latencies);
TextJson conversation_cache(const TextJson &engine, const TextJson &requests, const TextJson &totals,
                            const TextJson &parked);
} // namespace strata::runtime
