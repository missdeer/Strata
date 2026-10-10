#include "strata/runtime/server_recovery.hpp"
#include "unicode.h"
#include <algorithm>
#include <map>
#include <sstream>
namespace strata::runtime {
namespace {
const std::string high = "Reasoning effort is set to xhigh. Please think carefully through the task, "
                         "validate key assumptions, consider plausible alternatives, and prioritize "
                         "correctness, consistency, and clarity in the final answer.";
const std::string low = "Reasoning effort is set to low. Keep your thinking brief and focused, moving "
                        "directly to the conclusion without unnecessary elaboration.";
const std::string medium =
    "Reasoning effort is set to medium. Think as much as the task needs, without unnecessary elaboration.";
std::vector<std::string> words(const std::string &text) {
    std::vector<std::string> result;
    std::string current;
    for (auto c : unicode_cpts_from_utf8(text)) {
        const auto f = unicode_cpt_flags_from_cpt(c);
        const bool word = f.is_letter || f.is_number || c == '_';
        if (word) {
            current += unicode_cpt_to_utf8(unicode_tolower(c));
            continue;
        }
        if (!current.empty()) {
            result.push_back(current);
            current.clear();
        }
        if (!f.is_whitespace)
            result.push_back(unicode_cpt_to_utf8(unicode_tolower(c)));
    }
    if (!current.empty())
        result.push_back(current);
    if (result.size() > 30000)
        result.erase(result.begin(), result.end() - 30000);
    return result;
}
} // namespace
double reasoning_repeat_coverage(const std::string &text) {
    const auto tokens = words(text);
    if (tokens.size() < 2000)
        return 0;
    std::map<std::vector<std::string>, int> counts;
    std::vector<std::vector<std::string>> grams;
    for (size_t i = 0; i + 12 <= tokens.size(); ++i) {
        grams.emplace_back(tokens.begin() + i, tokens.begin() + i + 12);
        ++counts[grams.back()];
    }
    const auto start = tokens.size() - 2000;
    std::vector<bool> covered(2000, false);
    for (size_t i = start; i < grams.size(); ++i)
        if (counts[grams[i]] >= 3)
            for (size_t k = i; k < i + 12; ++k)
                covered[k - start] = true;
    return static_cast<double>(std::count(covered.begin(), covered.end(), true)) / covered.size();
}
std::optional<std::vector<int>> focused_recovery_prompt(const Tokenizer &tok, const std::vector<int> &ids,
                                                        const std::vector<int> &generated) {
    const auto end = tok.encode("<|im_end|>", true);
    if (end.size() != 1)
        return {};
    const auto stop = std::find(ids.begin(), ids.end(), end[0]);
    if (stop == ids.end())
        return {};
    std::vector<int> head(ids.begin(), stop);
    if (!tok.decode(head).starts_with("<|im_start|>system\n"))
        return {};
    const auto old = tok.encode(high), next = tok.encode(low);
    const auto position = std::search(head.begin(), head.end(), old.begin(), old.end());
    if (position == head.end())
        return {};
    const auto n = static_cast<size_t>(position - head.begin());
    std::vector<int> out(ids.begin(), ids.begin() + n);
    out.insert(out.end(), next.begin(), next.end());
    out.insert(out.end(), ids.begin() + n + old.size(), ids.end());
    out.insert(out.end(), generated.begin(), generated.end());
    return out;
}
std::vector<int> effort_prompt(const TemplateRequest &request, const Tokenizer &tok, const ChatTemplate &chat,
                               bool effort_end) {
    const bool off = request.kwargs.value("enable_thinking", true) == false;
    const auto effort = request.kwargs.value("reasoning_effort", "");
    const auto full = chat.render_prompt(request, tok.controls());
    if (!effort_end || (!off && (effort.empty() || effort == "xhigh" || effort == "high")))
        return tok.encode(full.text, true, full.plain);
    auto base = request;
    base.kwargs.erase("reasoning_effort");
    base.kwargs.erase("enable_thinking");
    const auto history = chat.render_prompt(base, tok.controls(), false),
               mine = chat.render_prompt(request, tok.controls(), false);
    if (!full.text.starts_with(mine.text))
        return tok.encode(full.text, true, full.plain);
    const auto tail =
        off ? std::string() : "<|im_start|>system\n" + (effort == "low" ? low : medium) + "<|im_end|>\n";
    const auto text = history.text + tail + full.text.substr(mine.text.size());
    auto plain = history.plain;
    for (const auto &span : full.plain)
        if (span.first >= mine.text.size())
            plain.emplace_back(span.first - mine.text.size() + history.text.size() + tail.size(),
                               span.second - mine.text.size() + history.text.size() + tail.size());
    return tok.encode(text, true, plain);
}
TextJson codex_metadata(const TextJson &request) {
    const auto meta = request.value("client_metadata", TextJson(nullptr));
    if (!meta.is_object())
        return TextJson::object();
    const auto raw = meta.value("x-codex-turn-metadata", TextJson(nullptr));
    if (!raw.is_string())
        return TextJson::object();
    const auto parsed = TextJson::parse(raw.get<std::string>(), nullptr, false);
    return parsed.is_object() ? parsed : TextJson::object();
}
std::string codex_title(const TextJson &request) {
    const auto input = request.value("input", TextJson(nullptr));
    std::string text;
    if (input.is_string())
        text = input.get<std::string>();
    else if (input.is_array())
        for (const auto &item : input)
            if (item.is_object() && item.value("role", "") == "user") {
                if (!text.empty())
                    text += '\n';
                const auto c = item.value("content", TextJson(nullptr));
                if (c.is_string())
                    text += c.get<std::string>();
                else if (c.is_array())
                    for (const auto &p : c)
                        if (p.is_object() && p.value("text", TextJson(nullptr)).is_string())
                            text += p["text"].get<std::string>();
            }
    const auto at = text.find("User prompt:");
    if (at != std::string::npos)
        text = text.substr(at + 12);
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        line = trim_text(line);
        if (!line.empty())
            break;
    }
    while (!line.empty() &&
           (line.front() == '"' || line.front() == '\'' || line.front() == '`' || line.front() == ' '))
        line.erase(line.begin());
    auto cps = unicode_cpts_from_utf8(line);
    while (!cps.empty() &&
           (cps.back() == ' ' || cps.back() == '"' || cps.back() == '\'' || cps.back() == '`' ||
            std::string(".!?,;:").find(static_cast<char>(cps.back())) != std::string::npos ||
            cps.back() == 0x3002 || cps.back() == 0xff01 || cps.back() == 0xff1f || cps.back() == 0xff0c ||
            cps.back() == 0xff1b || cps.back() == 0xff1a))
        cps.pop_back();
    if (cps.size() > 36)
        cps.resize(36);
    std::string out;
    for (auto c : cps)
        out += unicode_cpt_to_utf8(c);
    return trim_text(out).empty() ? "Untitled" : trim_text(out);
}
} // namespace strata::runtime
