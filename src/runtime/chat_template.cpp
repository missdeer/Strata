#include "strata/runtime/text.hpp"
#include "jinja/lexer.h"
#include "jinja/parser.h"
#include "jinja/runtime.h"
#include "json.h"

#include <algorithm>
#include <cstdlib>
#include <mutex>
#include <stdexcept>

namespace strata::runtime {
struct ChatTemplate::Impl {
    std::string source;
    jinja::program program;
    // The upstream interpreter's AST contains statistics/macro state. Keep a
    // template instance serialized; contexts and caller JSON remain per request.
    mutable std::mutex mutex;
    explicit Impl(std::string text) : source(std::move(text)), program(jinja::parse_from_tokens(jinja::lexer().tokenize(source))) {}
};
static std::string text_of(const TextJson& content) {
    if (content.is_string()) return content.get<std::string>();
    std::string text;
    if (content.is_array()) for (const auto& p : content) if (p.is_object() &&
        (p.value("type", TextJson()) == "text" || p.value("type", TextJson()) == "input_text" || p.value("type", TextJson()).is_null()))
        text += p.value("text", "");
    return text;
}
static bool has_image(const TextJson& content) {
    if (!content.is_array()) return false;
    return std::any_of(content.begin(), content.end(), [](const auto& p) {
        if (!p.is_object()) return false;
        const auto t = p.value("type", TextJson()); return t == "image" || t == "image_url" || t == "input_image";
    });
}
ChatTemplate::ChatTemplate(const std::filesystem::path& path) : impl_(std::make_shared<Impl>(read_text(path))) {}
std::string ChatTemplate::render(const TemplateRequest& request, bool generation_prompt) const {
    auto messages = TextJson::array();
    bool keep_empty = false;
#ifdef _WIN32
    char* keep = nullptr; size_t length = 0;
    if (_dupenv_s(&keep,&length,"STRATA_KEEP_EMPTY_TURNS") == 0 && keep) keep_empty = std::string(keep) == "1";
    std::free(keep);
#else
    const char* keep = std::getenv("STRATA_KEEP_EMPTY_TURNS");
    keep_empty = keep && std::string(keep) == "1";
#endif
    for (size_t i = 0; i < request.messages.size(); ++i) {
        const auto& m = request.messages[i];
        if (i+1 != request.messages.size() && !keep_empty && m.value("role", "") == "assistant" &&
            trim_text(text_of(m.value("content", TextJson()))).empty() && !has_image(m.value("content", TextJson())) &&
            m.value("tool_calls", TextJson::array()).empty()) continue;
        messages.push_back(m);
    }
    auto context = request.kwargs;
    context["messages"] = messages; context["tools"] = request.tools; context["add_generation_prompt"] = generation_prompt;
    std::lock_guard lock(impl_->mutex);
    jinja::context ctx(impl_->source);
    jinja::global_from_json(ctx, common_json::parse(context.dump()), false);
    jinja::runtime runtime(ctx);
    return jinja::render_string_parts(jinja::runtime::gather_string_parts(runtime.execute(impl_->program)));
}

static std::string cpt_utf8(uint32_t c) {
    std::string s;
    s += static_cast<char>(0xf0 | (c >> 18)); s += static_cast<char>(0x80 | ((c >> 12) & 63));
    s += static_cast<char>(0x80 | ((c >> 6) & 63)); s += static_cast<char>(0x80 | (c & 63)); return s;
}
static void replace_all(std::string& s, const std::string& a, const std::string& b) {
    for (size_t p = 0; (p = s.find(a,p)) != std::string::npos; p += b.size()) s.replace(p,a.size(),b);
}
RenderedPrompt ChatTemplate::render_prompt(const TemplateRequest& request, const std::vector<std::string>& controls, bool generation_prompt) const {
    std::vector<std::pair<std::string, std::string>> tags;
    const std::vector<std::string> fixed = {"<think>", "</think>", "<|vision_start|>", "<|image_pad|>", "<|vision_end|>", "<|video_pad|>"};
    for (size_t i = 0; i < fixed.size(); ++i) tags.emplace_back(fixed[i], cpt_utf8(0xf0e01 + static_cast<uint32_t>(i)));
    uint32_t next = 0xf0e10;
    for (const auto& c : controls) if (std::find(fixed.begin(), fixed.end(), c) == fixed.end()) tags.emplace_back(c, cpt_utf8(next++));
    std::stable_sort(tags.begin(), tags.end(), [](const auto& a, const auto& b) { return a.first.size() > b.first.size(); });
    auto mark = [&](std::string s) { for (const auto& [tag, marker] : tags) replace_all(s,tag,marker); return s; };
    auto deep = [&](auto&& self, TextJson& v) -> void {
        if (v.is_string()) v = mark(v.get<std::string>());
        else if (v.is_structured()) for (auto& x : v) self(self,x);
    };
    auto marked = request;
    for (auto& m : marked.messages) {
        const auto content = m.value("content", TextJson());
        for (auto& item : m.items()) if (item.key() != "role") deep(deep,item.value());
        if (m.value("role", "") == "assistant" && content.is_string()) {
            const auto s = content.get<std::string>(); const auto i = s.find("<think>"), j = s.find("</think>");
            if (trim_text(s).starts_with("<think>") && j != std::string::npos && i != std::string::npos) {
                m["content"] = s.substr(0,i+7) + mark(s.substr(i+7,j-i-7)) + "</think>" + mark(s.substr(j+8));
            }
        }
    }
    deep(deep,marked.tools);
    const auto raw = render(marked,generation_prompt);
    RenderedPrompt result;
    for (size_t p = 0; p < raw.size();) {
        bool found = false;
        for (const auto& [tag, marker] : tags) if (raw.compare(p,marker.size(),marker) == 0) {
            const auto start = result.text.size(); result.text += tag;
            result.plain.emplace_back(start,result.text.size()); p += marker.size(); found = true; break;
        }
        if (!found) result.text += raw[p++];
    }
    return result;
}
TextJson ChatTemplate::capabilities() const {
    auto probe = [&](TextJson messages, TextJson tools = nullptr) {
        try { return render({std::move(messages),std::move(tools),TextJson::object()}); } catch (const std::exception&) { return std::string(); }
    };
    const TextJson user = {{"role","user"},{"content","strata_caps_user"}};
    TextJson tools = TextJson::array();
    for (int i = 0; i < 2; ++i) tools.push_back({{"name","strata_caps_call_" + std::to_string(i)},
        {"description","strata_caps_description"},{"parameters",{{"type","object"},{"properties",{{"arg",{{"type","string"}}}}}}}});
    const auto tool_prompt = probe(TextJson::array({user}),tools);
    auto supported = [&](int count) {
        auto messages = TextJson::array({user}); auto calls = TextJson::array();
        for (int i = 0; i < count; ++i) calls.push_back({{"function",{{"name","strata_caps_call_"+std::to_string(i)},
            {"arguments",{{"arg","strata_caps_arg_"+std::to_string(i)}}}}}});
        messages.push_back({{"role","assistant"},{"content",""},{"tool_calls",calls}});
        for (int i = 0; i < count; ++i) messages.push_back({{"role","tool"},{"content","strata_caps_result_"+std::to_string(i)}});
        messages.push_back(user);
        try {
            const auto prompt = probe(messages,tools);
            int got = 0;
            for (size_t p = 0; (p = prompt.find("<function=strata_caps_call_",p)) != std::string::npos;) {
                const auto end = prompt.find("</function>",p);
                if (end == std::string::npos) return false;
                const auto call = parse_tool_call(prompt.substr(p,end+11-p));
                if (got >= count || call.name != calls[got]["function"]["name"].get<std::string>() || call.arguments != calls[got]["function"]["arguments"]) return false;
                ++got;
                p = end+11;
            }
            for (int i = 0; i < count; ++i) if (prompt.find("strata_caps_result_"+std::to_string(i)) == std::string::npos) return false;
            return got == count;
        } catch (const std::exception&) { return false; }
    };
    return {{"supports_tools", tool_prompt.find("strata_caps_call_0") != std::string::npos && tool_prompt.find("strata_caps_description") != std::string::npos &&
                              tool_prompt.find("<tool_call>") != std::string::npos && tool_prompt.find("<function=") != std::string::npos},
        {"supports_tool_calls",supported(1)},{"supports_parallel_tool_calls",supported(2)},
        {"supports_system_role",probe(TextJson::array({{{"role","system"},{"content","strata_caps_system"}},user})).find("strata_caps_system") != std::string::npos},
        {"supports_preserve_reasoning",probe(TextJson::array({user,{{"role","assistant"},{"content","strata_caps_answer"},{"reasoning_content","strata_caps_reasoning"}},user})).find("strata_caps_reasoning") != std::string::npos}};
}
} // namespace strata::runtime
