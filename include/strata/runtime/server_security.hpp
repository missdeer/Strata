#pragma once
#include "strata/runtime/text.hpp"
#include <set>
#include <map>
namespace strata::runtime {
class ServerSecurity {
    std::set<std::string> names_;
    std::vector<std::string> allowed_, cors_, trusted_;

  public:
    ServerSecurity(const TextJson &cfg, const std::string &bind);
    static std::string host_name(std::string);
    bool host_allowed(const std::string &, bool keyed) const;
    bool origin_allowed(const std::string &, const std::string &) const;
    bool foreign_origin(const std::string &, const std::string &) const;
    void own_page(const std::string &origin, const std::string &host, const std::string &content_type) const;
    std::map<std::string, std::string> cors(const std::string &origin,
                                            const std::string &requested_headers) const;
};
void validate_security(const TextJson &cfg);
} // namespace strata::runtime
