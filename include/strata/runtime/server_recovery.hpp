#pragma once
#include "strata/runtime/server_api.hpp"
namespace strata::runtime {
double reasoning_repeat_coverage(const std::string &text);
std::optional<std::vector<int>> focused_recovery_prompt(const Tokenizer &, const std::vector<int> &prompt,
                                                        const std::vector<int> &generated);
std::vector<int> effort_prompt(const TemplateRequest &, const Tokenizer &, const ChatTemplate &,
                               bool effort_end);
TextJson codex_metadata(const TextJson &request);
std::string codex_title(const TextJson &request);
} // namespace strata::runtime
