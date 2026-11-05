#pragma once
#include "strata/runtime/text.hpp"
namespace strata::runtime {
// Atomic replacement from a private, exclusive temporary file in the same directory.
void save_server_json(const std::filesystem::path &path, const TextJson &value);
TextJson shared_defaults(const TextJson &value);
TextJson server_config_view(const TextJson &config, const std::filesystem::path &path);
TextJson server_config_edit(TextJson config, const TextJson &edits);
std::string session_filename(const TextJson &value);
} // namespace strata::runtime
