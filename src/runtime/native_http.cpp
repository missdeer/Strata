#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>
#else
#include <curl/curl.h>
#endif
#include "strata/runtime/native_http.hpp"
#include "strata/runtime/path.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <thread>
namespace strata::runtime {
namespace {
void header_line(HttpResult &result, std::string line) {
    const auto p = line.find(':');
    if (p == std::string::npos)
        return;
    auto key = line.substr(0, p);
    std::transform(key.begin(), key.end(), key.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    auto value = line.substr(p + 1);
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
        value.erase(value.begin());
    while (!value.empty() && (value.back() == '\r' || value.back() == '\n' || value.back() == ' '))
        value.pop_back();
    result.headers[key] = value;
}
} // namespace
static HttpResult native_http_once(const std::string &method, const std::string &url,
                                   const std::map<std::string, std::string> &headers, const std::string &body,
                                   double timeout, size_t maximum, const std::function<bool()> &cancelled,
                                   const std::function<bool(const std::string &)> &chunk) {
    if ((!url.starts_with("http://") && !url.starts_with("https://")) ||
        url.find_first_of("\r\n") != std::string::npos || url.find('\0') != std::string::npos)
        throw std::invalid_argument("HTTP URL must use http or https");
    for (const auto &[key, value] : headers)
        if (key.empty() || key.find_first_of("\r\n:") != std::string::npos ||
            key.find('\0') != std::string::npos || value.find_first_of("\r\n") != std::string::npos ||
            value.find('\0') != std::string::npos)
            throw std::invalid_argument("invalid HTTP header");
    HttpResult result;
#ifdef _WIN32
    struct Handle {
        HINTERNET value = nullptr;
        ~Handle() {
            if (value)
                WinHttpCloseHandle(value);
        }
    };
    auto wide = utf8_path(url).native();
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = DWORD(-1);
    parts.dwUrlPathLength = DWORD(-1);
    parts.dwExtraInfoLength = DWORD(-1);
    if (!WinHttpCrackUrl(wide.c_str(), static_cast<DWORD>(wide.size()), 0, &parts))
        throw std::runtime_error("invalid HTTP URL");
    Handle session{WinHttpOpen(L"strata", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                               WINHTTP_NO_PROXY_BYPASS, 0)};
    if (!session.value)
        throw std::runtime_error("cannot initialize HTTP client");
    const int ms = static_cast<int>(std::min(2147483000., std::max(1., timeout * 1000.)));
    WinHttpSetTimeouts(session.value, ms, ms, ms, ms);
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    Handle connection{WinHttpConnect(session.value, host.c_str(), parts.nPort, 0)};
    if (!connection.value)
        throw std::runtime_error("cannot connect HTTP client");
    auto path = std::wstring(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (path.empty())
        path = L"/";
    if (parts.dwExtraInfoLength)
        path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    HINTERNET request = WinHttpOpenRequest(connection.value, utf8_path(method).c_str(), path.c_str(), nullptr,
                                           WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                           parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    if (!request)
        throw std::runtime_error("cannot create HTTP request");
    std::atomic<HINTERNET> owned{request};
    std::atomic<bool> done{false}, aborted{false};
    const auto born = std::chrono::steady_clock::now();
    std::thread watch([&] {
        while (!done) {
            if ((cancelled && cancelled()) ||
                std::chrono::duration<double>(std::chrono::steady_clock::now() - born).count() > timeout) {
                aborted = true;
                if (auto h = owned.exchange(nullptr))
                    WinHttpCloseHandle(h);
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    });
    struct Finish {
        std::function<void()> fn;
        ~Finish() { fn(); }
    } finish{[&] {
        done = true;
        watch.join();
        if (auto h = owned.exchange(nullptr))
            WinHttpCloseHandle(h);
    }};
    DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
    std::wstring raw_headers;
    for (const auto &[key, value] : headers)
        raw_headers += utf8_path(key + ": " + value + "\r\n").native();
    if (!raw_headers.empty() &&
        !WinHttpAddRequestHeaders(request, raw_headers.c_str(), static_cast<DWORD>(raw_headers.size()),
                                  WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE))
        throw std::runtime_error("cannot set HTTP headers");
    if (aborted ||
        !WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char *>(body.data()),
                            static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0) ||
        aborted || !WinHttpReceiveResponse(request, nullptr))
        throw std::runtime_error(aborted ? "HTTP request cancelled or timed out" : "HTTP request failed");
    DWORD status = 0, size = sizeof(status);
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX))
        throw std::runtime_error("invalid HTTP response");
    result.status = static_cast<int>(status);
    size = 0;
    WinHttpQueryHeaders(request, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &size,
                        WINHTTP_NO_HEADER_INDEX);
    std::wstring raw(size / sizeof(wchar_t), L'\0');
    if (size && WinHttpQueryHeaders(request, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX,
                                    raw.data(), &size, WINHTTP_NO_HEADER_INDEX)) {
        while (!raw.empty() && raw.back() == L'\0')
            raw.pop_back();
        std::istringstream in(path_utf8(std::filesystem::path(raw)));
        std::string row;
        while (std::getline(in, row))
            header_line(result, row);
    }
    for (;;) {
        if (aborted)
            throw std::runtime_error("HTTP request cancelled or timed out");
        char data[16384];
        DWORD read = 0;
        if (!WinHttpReadData(request, data, sizeof(data), &read))
            throw std::runtime_error("HTTP response read failed");
        if (!read)
            break;
        if (result.body.size() + read > maximum)
            throw std::runtime_error("HTTP response exceeds size limit");
        std::string bytes(data, read);
        result.body += bytes;
        if (chunk && !chunk(bytes))
            break;
    }
#else
    static const auto initialized = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (initialized != CURLE_OK)
        throw std::runtime_error("cannot initialize HTTP client");
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> request(curl_easy_init(), curl_easy_cleanup);
    if (!request)
        throw std::runtime_error("cannot create HTTP client");
    struct Context {
        HttpResult *result;
        size_t maximum;
        const std::function<bool()> *cancelled;
        const std::function<bool(const std::string &)> *chunk;
        bool finished = false;
        std::exception_ptr error;
    } ctx{&result, maximum, &cancelled, &chunk};
    curl_easy_setopt(request.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(request.get(), CURLOPT_CUSTOMREQUEST, method.c_str());
    curl_easy_setopt(request.get(), CURLOPT_TIMEOUT_MS, static_cast<long>(std::max(1., timeout * 1000.)));
    curl_easy_setopt(request.get(), CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(request.get(), CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(
        request.get(), CURLOPT_XFERINFOFUNCTION,
        +[](void *p, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
            auto &c = *static_cast<Context *>(p);
            return *c.cancelled && (*c.cancelled)() ? 1 : 0;
        });
    curl_easy_setopt(request.get(), CURLOPT_XFERINFODATA, &ctx);
    curl_easy_setopt(
        request.get(), CURLOPT_WRITEFUNCTION, +[](char *bytes, size_t a, size_t b, void *p) -> size_t {
            auto &c = *static_cast<Context *>(p);
            const auto n = a * b;
            try {
                if (c.result->body.size() + n > c.maximum)
                    throw std::runtime_error("HTTP response exceeds limit");
                std::string text(bytes, n);
                c.result->body += text;
                if (*c.chunk && !(*c.chunk)(text)) {
                    c.finished = true;
                    return 0;
                }
                return n;
            } catch (...) {
                c.error = std::current_exception();
                return 0;
            }
        });
    curl_easy_setopt(request.get(), CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(
        request.get(), CURLOPT_HEADERFUNCTION, +[](char *bytes, size_t a, size_t b, void *p) -> size_t {
            header_line(*static_cast<HttpResult *>(p), std::string(bytes, a * b));
            return a * b;
        });
    curl_easy_setopt(request.get(), CURLOPT_HEADERDATA, &result);
    curl_slist *list = nullptr;
    for (const auto &[key, value] : headers)
        list = curl_slist_append(list, (key + ": " + value).c_str());
    std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> list_owner(list, curl_slist_free_all);
    curl_easy_setopt(request.get(), CURLOPT_HTTPHEADER, list);
    if (!body.empty()) {
        curl_easy_setopt(request.get(), CURLOPT_POSTFIELDS, body.data());
        curl_easy_setopt(request.get(), CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size()));
    }
    const auto code = curl_easy_perform(request.get());
    if (ctx.error)
        std::rethrow_exception(ctx.error);
    if (code != CURLE_OK && !ctx.finished)
        throw std::runtime_error("HTTP request failed or was cancelled");
    long status = 0;
    curl_easy_getinfo(request.get(), CURLINFO_RESPONSE_CODE, &status);
    result.status = static_cast<int>(status);
#endif
    return result;
}
HttpResult native_http(const std::string &initial_method, const std::string &initial_url,
                       const std::map<std::string, std::string> &headers, const std::string &initial_body,
                       double timeout, size_t maximum, const std::function<bool()> &cancelled,
                       const std::function<bool(const std::string &)> &chunk) {
    auto url = initial_url, method = initial_method, body = initial_body;
    const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(timeout);
    const auto authority = [](const std::string &u) {
        const auto start = u.find("://");
        if (start == std::string::npos)
            return std::string();
        const auto stop = u.find('/', start + 3);
        return u.substr(0, stop);
    };
    for (int i = 0; i < 10; ++i) {
        const auto left = std::chrono::duration<double>(end - std::chrono::steady_clock::now()).count();
        if (left <= 0)
            throw std::runtime_error("HTTP redirect deadline exceeded");
        auto response = native_http_once(method, url, headers, body, left, maximum, cancelled, chunk);
        if ((response.status != 301 && response.status != 302 && response.status != 303 &&
             response.status != 307 && response.status != 308) ||
            !response.headers.count("location"))
            return response;
        auto next = response.headers["location"];
        if (next.starts_with("//"))
            next = url.substr(0, url.find(':') + 1) + next;
        else if (next.starts_with('/'))
            next = authority(url) + next;
        else if (!next.starts_with("http://") && !next.starts_with("https://"))
            next = url.substr(0, url.rfind('/') + 1) + next;
        if (authority(next) != authority(url)) {
            if (method != "GET" && method != "HEAD")
                throw std::runtime_error("cross-origin HTTP request redirect refused");
            for (const auto &[key, value] : headers) {
                (void)value;
                auto name = key;
                std::transform(name.begin(), name.end(), name.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (name != "user-agent" && name != "accept")
                    throw std::runtime_error("cross-origin credential redirect refused");
            }
        }
        if (response.status == 303 ||
            ((response.status == 301 || response.status == 302) && method == "POST")) {
            method = "GET";
            body.clear();
        }
        url = next;
    }
    throw std::runtime_error("too many HTTP redirects");
}
} // namespace strata::runtime
