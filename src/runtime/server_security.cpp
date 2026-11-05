#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#endif
#include "strata/runtime/server_security.hpp"
#include "strata/runtime/server_engine.hpp"
#include <algorithm>
#include <sstream>
namespace strata::runtime {
namespace {
using J = TextJson;
std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}
std::string origin(std::string s) {
    s = trim_text(s);
    while (!s.empty() && s.back() == '/')
        s.pop_back();
    return s;
}
std::vector<std::string> list(const J &value, const std::string &key) {
    if (value.is_null() || value == "")
        return {};
    if (value.is_string())
        return {value.get<std::string>()};
    if (!value.is_array())
        throw std::invalid_argument(key + " must be a string or array");
    std::vector<std::string> result;
    for (const auto &x : value) {
        if (!x.is_string())
            throw std::invalid_argument(key + " must contain strings");
        result.push_back(x.get<std::string>());
    }
    return result;
}
std::vector<std::string> origins(const J &value, const std::string &key, bool wildcard) {
    auto result = list(value, key);
    for (auto &s : result) {
        s = origin(s);
        if (s == "*" && wildcard)
            continue;
        const auto p = s.find("://");
        if (p == std::string::npos || (s.substr(0, p) != "http" && s.substr(0, p) != "https") ||
            s.size() <= p + 3 || s.find('/', p + 3) != std::string::npos ||
            s.find('*', p + 3) != std::string::npos)
            throw std::invalid_argument(key + " must contain HTTP origins without paths");
    }
    return result;
}
bool in(const std::string &name, const std::set<std::string> &names) {
    if (names.count(name))
        return true;
    for (const auto &s : names)
        if (s.starts_with('.') && (name == s.substr(1) || name.ends_with(s)))
            return true;
    return false;
}
bool ip(const std::string &name) {
    unsigned char bytes[16];
    const auto v6 = name.substr(0, name.find('%'));
    return inet_pton(AF_INET, name.c_str(), bytes) == 1 || inet_pton(AF_INET6, v6.c_str(), bytes) == 1;
}
std::vector<std::string> allowed(const J &cfg) {
    auto result = list(cfg.value("allowed_hosts", J(nullptr)), "allowed_hosts");
    std::istringstream environment(server_environment("STRATA_ALLOWED_HOSTS"));
    std::string s;
    while (std::getline(environment, s, ','))
        if (!trim_text(s).empty())
            result.push_back(s);
    for (auto &name : result) {
        name = lower(trim_text(name));
        if (name == "*")
            continue;
        const auto scheme = name.find("://");
        if (scheme != std::string::npos)
            name.erase(0, scheme + 3);
        name = name.substr(0, name.find('/'));
        const bool dot = name.starts_with('.');
        name = ServerSecurity::host_name(dot ? name.substr(1) : name);
        if (name.empty())
            throw std::invalid_argument("allowed_hosts contains an invalid host");
        if (dot)
            name = '.' + name;
    }
    return result;
}
} // namespace
std::string ServerSecurity::host_name(std::string value) {
    value = lower(trim_text(value));
    if (value.starts_with('[')) {
        const auto end = value.find(']');
        if (end == std::string::npos)
            return {};
        const auto tail = value.substr(end + 1);
        if (!tail.empty() &&
            (tail.front() != ':' || tail.size() == 1 ||
             !std::all_of(tail.begin() + 1, tail.end(), [](unsigned char c) { return std::isdigit(c); })))
            return {};
        return value.substr(1, end - 1);
    }
    const auto colons = std::count(value.begin(), value.end(), ':');
    if (colons == 1) {
        const auto p = value.find(':');
        const auto port = value.substr(p + 1);
        if (port.empty() ||
            !std::all_of(port.begin(), port.end(), [](unsigned char c) { return std::isdigit(c); }))
            return {};
        value.resize(p);
    } else if (colons > 1)
        return value;
    while (!value.empty() && value.back() == '.')
        value.pop_back();
    if (value.empty() || !std::all_of(value.begin(), value.end(), [](unsigned char c) {
            return std::isalnum(c) || c == '-' || c == '.' || c == '_';
        }))
        return {};
    return value;
}
void validate_security(const J &cfg) {
    origins(cfg.value("cors_origins", J(nullptr)), "cors_origins", true);
    origins(cfg.value("trusted_origins", J(nullptr)), "trusted_origins", false);
    allowed(cfg);
}
ServerSecurity::ServerSecurity(const J &cfg, const std::string &bind)
    : allowed_(allowed(cfg)), cors_(origins(cfg.value("cors_origins", J(nullptr)), "cors_origins", true)),
      trusted_(origins(cfg.value("trusted_origins", J(nullptr)), "trusted_origins", false)) {
    names_ = {"localhost", "127.0.0.1", "::1"};
    const auto name = host_name(bind);
    if (!name.empty() && name != "0.0.0.0" && name != "::")
        names_.insert(name);
    for (const auto &s : allowed_)
        if (s != "*")
            names_.insert(s);
    for (const auto &s : trusted_)
        names_.insert(host_name(s.substr(s.find("://") + 3)));
    if (bind != "localhost" && bind != "127.0.0.1" && bind != "::1") {
#ifdef _WIN32
        WSADATA data{};
        const bool owns = WSAStartup(MAKEWORD(2, 2), &data) == 0;
#endif
        char host[256]{};
        if (gethostname(host, sizeof(host)) == 0) {
            names_.insert(lower(host));
            names_.insert(lower(host) + ".local");
            addrinfo *addresses = nullptr;
            if (getaddrinfo(host, nullptr, nullptr, &addresses) == 0) {
                for (auto *p = addresses; p; p = p->ai_next) {
                    char ip[NI_MAXHOST]{};
                    if (getnameinfo(p->ai_addr, static_cast<int>(p->ai_addrlen), ip, sizeof(ip), nullptr, 0,
                                    NI_NUMERICHOST) == 0)
                        names_.insert(ip);
                }
                freeaddrinfo(addresses);
            }
        }
        names_.insert("host.docker.internal");
#ifdef _WIN32
        if (owns)
            WSACleanup();
#endif
    }
}
bool ServerSecurity::host_allowed(const std::string &host, bool keyed) const {
    if (keyed || trim_text(host).empty() ||
        std::find(allowed_.begin(), allowed_.end(), "*") != allowed_.end())
        return true;
    const auto name = host_name(host);
    return !name.empty() &&
           (ip(name) || name == "localhost" || name.ends_with(".localhost") || in(name, names_));
}
bool ServerSecurity::origin_allowed(const std::string &given, const std::string &host) const {
    const auto normalized = origin(given);
    for (const auto *list : {&trusted_, &cors_})
        if (std::find(list->begin(), list->end(), normalized) != list->end() ||
            std::find(list->begin(), list->end(), "*") != list->end())
            return true;
    const auto value = lower(normalized);
    const auto p = value.find("://");
    if (p == std::string::npos || p == 0)
        return false;
    const auto scheme = value.substr(0, p);
    if (scheme != "http" && scheme != "https")
        return true;
    const auto rest = value.substr(p + 3);
    if (!host.empty() && rest == lower(trim_text(host)))
        return true;
    const auto name = host_name(rest);
    return !name.empty() && (name == "localhost" || name.ends_with(".localhost") || in(name, names_));
}
bool ServerSecurity::foreign_origin(const std::string &given, const std::string &host) const {
    const auto normalized = origin(given);
    if (normalized.empty())
        return false;
    const auto p = normalized.find("://");
    const auto authority = p == std::string::npos ? normalized : normalized.substr(p + 3);
    return authority != host && std::find(trusted_.begin(), trusted_.end(), normalized) == trusted_.end();
}
void ServerSecurity::own_page(const std::string &given, const std::string &host,
                              const std::string &type) const {
    if (!type.starts_with("application/json"))
        throw ApiError(415, "send application/json");
    if (foreign_origin(given, host))
        throw ApiError(403, "this action requires Strata's own page or a trusted origin", "forbidden");
}
std::map<std::string, std::string> ServerSecurity::cors(const std::string &given,
                                                        const std::string &asked) const {
    auto value = origin(given);
    std::map<std::string, std::string> result;
    if (std::find(cors_.begin(), cors_.end(), "*") != cors_.end())
        result["Access-Control-Allow-Origin"] = "*";
    else if (!value.empty() && std::find(cors_.begin(), cors_.end(), value) != cors_.end()) {
        result["Access-Control-Allow-Origin"] = value;
        result["Vary"] = "Origin";
    } else
        return result;
    result["Access-Control-Allow-Methods"] = "GET, POST, OPTIONS";
    result["Access-Control-Allow-Headers"] =
        asked.empty() ? "Authorization, Content-Type, x-api-key, anthropic-version, anthropic-beta" : asked;
    result["Access-Control-Max-Age"] = "600";
    return result;
}
} // namespace strata::runtime
