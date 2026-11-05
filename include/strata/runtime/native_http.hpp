#pragma once
#include <functional>
#include <map>
#include <string>
namespace strata::runtime {
struct HttpResult {
    int status = 0;
    std::map<std::string, std::string> headers;
    std::string body;
};
HttpResult native_http(const std::string &, const std::string &, const std::map<std::string, std::string> &,
                       const std::string &, double, size_t, const std::function<bool()> &cancelled = {},
                       const std::function<bool(const std::string &)> &chunk = {});
} // namespace strata::runtime
