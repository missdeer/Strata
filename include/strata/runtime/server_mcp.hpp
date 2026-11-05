#pragma once
#include "strata/runtime/text.hpp"
#include <functional>
#include <set>
namespace strata::runtime {
void validate_mcp(const TextJson &config);
class McpHub {
    struct Impl;
    std::unique_ptr<Impl> impl_;

  public:
    explicit McpHub(const TextJson &config);
    ~McpHub();
    void wait(double seconds);
    TextJson tools(const std::set<std::string> &exclude = {});
    TextJson status();
    TextJson call(const std::string &name, const TextJson &arguments, const std::function<bool()> &cancelled);
    int max_rounds() const;
};
} // namespace strata::runtime
