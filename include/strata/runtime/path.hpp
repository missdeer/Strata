#pragma once
#include <filesystem>
#include <string>

namespace strata::runtime {
inline std::filesystem::path utf8_path(const std::string& text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}
inline std::string path_utf8(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return std::string(text.begin(), text.end());
}
} // namespace strata::runtime
