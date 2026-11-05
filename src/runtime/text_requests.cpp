#include "strata/runtime/text.hpp"
#include <algorithm>
#include <cctype>
#include <set>
#include <stdexcept>

namespace strata::runtime {
namespace {
TextJson get(const TextJson& obj, const std::string& key, const TextJson& fallback = nullptr) {
    return obj.is_object() && obj.contains(key) ? obj[key] : fallback;
}
bool truth(const TextJson& v) {
    if (v.is_null()) return false;
    if (v.is_boolean()) return v.get<bool>();
    if (v.is_number()) return v.get<double>() != 0;
    if (v.is_string()) return !v.get_ref<const std::string&>().empty();
    return !v.empty();
}
std::string text_of(const TextJson& c) {
    if (c.is_string()) return c.get<std::string>();
    std::string out;
    if (c.is_array()) for (const auto& p : c) if (p.is_object() && (get(p,"type").is_null() || get(p,"type") == "text" || get(p,"type") == "input_text")) out += p.value("text", "");
    return out;
}
bool image(const TextJson& p) { return get(p,"type") == "image" || get(p,"type") == "image_url" || get(p,"type") == "input_image"; }
bool has_image(const TextJson& c) { return c.is_array() && std::any_of(c.begin(),c.end(),image); }
std::string image_source(const TextJson& p) {
    if (get(p,"type") == "image") {
        auto s = get(p,"source",TextJson::object());
        if (get(s,"type") == "base64") return "data:" + s.value("media_type","image/png") + ";base64," + s.value("data","");
        return truth(get(s,"url")) ? s["url"].get<std::string>() : s.value("path", "");
    }
    auto s = get(p,"image_url"); if (s.is_object()) s = get(s,"url"); return s.is_string() ? s.get<std::string>() : "";
}
TextJson parts_of(const TextJson& c) {
    if (!has_image(c)) return text_of(c);
    auto out = TextJson::array();
    for (const auto& p : c) {
        if (!p.is_object()) continue;
        if (image(p)) out.push_back({{"type","image"},{"source",image_source(p)}});
        else if ((get(p,"type").is_null() || get(p,"type") == "text" || get(p,"type") == "input_text") && p.contains("text"))
            out.push_back({{"type","text"},{"text",p["text"]}});
    }
    return out;
}
TextJson object_list(TextJson v, const std::string& name) {
    if (v.is_null()) return TextJson::array();
    if (v.is_string()) v = parse_text_json(v.get<std::string>());
    if (!v.is_array() || !std::all_of(v.begin(),v.end(),[](const auto& m) { return m.is_object(); }))
        throw std::invalid_argument(name + " must be a list of objects");
    return v;
}
TextJson tools_of(const TextJson& v, bool openai) {
    auto out = TextJson::array();
    if (!truth(v)) return nullptr;
    for (const auto& t : object_list(v,"tools")) {
        const auto fn = openai && get(t,"type") == "function" ? get(t,"function",t) : t;
        if (!fn.is_object() || !get(fn,"name").is_string() || fn["name"].get<std::string>().empty())
            throw std::invalid_argument("tool has no name");
        const auto schema = get(fn,openai ? "parameters" : "input_schema");
        if (!schema.is_null() && !schema.is_object()) throw std::invalid_argument("tool parameters must be an object");
        if (openai) out.push_back(fn);
        else out.push_back({{"name",fn["name"]},{"description",get(fn,"description","")},{"parameters",get(fn,"input_schema",TextJson::object())}});
    }
    return out.empty() ? TextJson(nullptr) : out;
}
TextJson arguments(const TextJson& raw) {
    if (raw.is_null() || (raw.is_string() && trim_text(raw.get<std::string>()).empty())) return TextJson::object();
    if (raw.is_object()) return raw;
    if (raw.is_string()) {
        auto value = TextJson::parse(raw.get<std::string>(),nullptr,false);
        if (value.is_object()) return value;
        return {{"arguments",raw}};
    }
    return {{"arguments",python_json(raw)}};
}
void late_system(TextJson& messages) {
    for (size_t i = 1; i < messages.size(); ++i) if (get(messages[i],"role") == "system") messages[i]["role"] = "user";
}
std::string pin_billing(std::string s) {
    if (!s.starts_with("x-anthropic-billing-header:")) return s;
    const auto cch = s.find("cch=",26), cv = s.find("cc_version=");
    if (cch <= 160) { const auto v = cch+4, e = s.find(';',v); if (e != std::string::npos && e > v && e-v <= 16) s.replace(v,e-v,e-v,'f'); }
    if (cv <= 160) {
        const auto v = cv+11, e = s.find(';',cv);
        if (e != std::string::npos && v < e && e-v <= 64) {
            auto tail = v;
            for (int i = 0; i < 3 && tail < e; ++i) { const auto dot = s.find('.',tail); tail = dot != std::string::npos && dot < e ? dot+1 : e; }
            if (tail < e) s.replace(tail,e-tail,e-tail,'f');
        }
    }
    return s;
}
}
TextJson effort_kwargs(const TextJson& v) {
    if (v.is_null() || v == "") return TextJson::object();
    if (v == false) return {{"enable_thinking",false}};
    auto key = trim_text(v.is_string() ? v.get<std::string>() : python_json(v));
    std::transform(key.begin(),key.end(),key.begin(),[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (key == "none" || key == "off" || key == "minimal" || key == "disabled" || key == "false") return {{"enable_thinking",false}};
    if (key == "low" || key == "medium") return {{"reasoning_effort",key}};
    if (key == "high" || key == "xhigh" || key == "max" || key == "maximum") return {{"reasoning_effort","xhigh"}};
    throw std::invalid_argument("unknown reasoning effort: use none, low, medium or high");
}
TemplateRequest openai_to_messages(const TextJson& req) {
    TemplateRequest out;
    for (const auto& m : object_list(get(req,"messages"),"messages")) {
        auto role = get(m,"role"); if (role == "developer") role = "system";
        TextJson msg = {{"role",role},{"content",role == "user" || role == "tool" || role == "assistant" ? parts_of(get(m,"content")) : TextJson(text_of(get(m,"content")))}};
        if (truth(get(m,"reasoning_content"))) msg["reasoning_content"] = m["reasoning_content"];
        if (truth(get(m,"tool_calls"))) {
            auto calls = TextJson::array();
            for (const auto& c : object_list(m["tool_calls"],"tool_calls")) {
                auto fn = get(c,"function",c); if (!fn.is_object()) throw std::invalid_argument("tool call function must be an object");
                calls.push_back({{"function",{{"name",get(fn,"name")},{"arguments",arguments(get(fn,"arguments"))}}}});
            }
            msg["tool_calls"] = calls;
        }
        out.messages.push_back(msg);
    }
    out.tools = tools_of(get(req,"tools"),true);
    auto effort = get(req,"reasoning_effort"); if (!truth(effort)) effort = get(get(req,"reasoning"),"effort");
    out.kwargs = effort_kwargs(effort);
    auto kw = get(req,"chat_template_kwargs",TextJson::object());
    if (truth(kw) && !kw.is_object()) throw std::invalid_argument("chat_template_kwargs must be an object");
    if (kw.is_object()) for (const auto& item : kw.items()) {
        if (item.key() == "enable_thinking" && !truth(item.value())) out.kwargs = {{"enable_thinking",false}};
        else if (item.key() == "reasoning_effort" && !out.kwargs.contains("enable_thinking")) out.kwargs.update(effort_kwargs(item.value()));
    }
    late_system(out.messages); return out;
}
TemplateRequest anthropic_to_messages(const TextJson& req, bool think_unasked) {
    TemplateRequest out;
    if (truth(get(req,"system"))) out.messages.push_back({{"role","system"},{"content",pin_billing(text_of(req["system"]))}});
    for (const auto& m : object_list(get(req,"messages"),"messages")) {
        auto c = get(m,"content"), role = get(m,"role");
        if (c.is_string()) { out.messages.push_back({{"role",role},{"content",c}}); continue; }
        if (!c.is_null() && !c.is_array()) throw std::invalid_argument("Anthropic content must be string or blocks");
        if (role == "user" && has_image(c) && !std::any_of(c.begin(),c.end(),[](const auto& b) { return get(b,"type") == "tool_result"; })) {
            out.messages.push_back({{"role","user"},{"content",parts_of(c)}}); continue;
        }
        std::string text, reasoning; auto calls = TextJson::array(), parts = TextJson::array();
        bool have_text = false, have_reasoning = false;
        for (const auto& b : c) {
            if (!b.is_object()) throw std::invalid_argument("content block must be an object");
            auto kind = get(b,"type");
            if (kind == "text") { text += b.value("text",""); parts.push_back(b); have_text = true; }
            else if (image(b)) parts.push_back(b);
            else if (kind == "thinking") { reasoning += b.value("thinking",""); have_reasoning = true; }
            else if (kind == "tool_use") calls.push_back({{"function",{{"name",get(b,"name")},{"arguments",arguments(get(b,"input"))}}}});
            else if (kind == "tool_result") out.messages.push_back({{"role","tool"},{"content",parts_of(get(b,"content"))}});
        }
        if (have_text || have_reasoning || !calls.empty() || !parts.empty()) {
            TextJson msg = {{"role",role},{"content",has_image(parts) ? parts_of(parts) : TextJson(text)}};
            if (have_reasoning) msg["reasoning_content"] = reasoning;
            if (!calls.empty()) msg["tool_calls"] = calls;
            out.messages.push_back(msg);
        }
    }
    out.tools = tools_of(get(req,"tools"),false);
    auto thinking = get(req,"thinking"), effort = get(get(req,"output_config"),"effort");
    if (get(thinking,"type") == "disabled") out.kwargs = {{"enable_thinking",false}};
    else if (truth(effort)) out.kwargs = effort_kwargs(effort);
    else if (truth(get(thinking,"budget_tokens"))) {
        try {
            const auto b = thinking["budget_tokens"];
            double n;
            if (b.is_string()) {
                const auto s = trim_text(b.get<std::string>()); size_t used = 0;
                n = static_cast<double>(std::stoll(s,&used)); if (used != s.size()) throw std::invalid_argument("invalid budget");
            } else if (b.is_boolean()) n = b.get<bool>() ? 1 : 0;
            else n = b.get<double>();
            out.kwargs = {{"reasoning_effort",n < 2048 ? "low" : n < 8192 ? "medium" : "xhigh"}};
        } catch (const std::exception&) {}
    } else if (thinking.is_null() && !truth(get(req,"reasoning_budget_tokens")) && !think_unasked) out.kwargs = {{"enable_thinking",false}};
    late_system(out.messages); return out;
}
std::vector<std::string> images_of(const TextJson& messages) {
    std::vector<std::string> out;
    for (const auto& m : messages) if (get(m,"content").is_array()) for (const auto& p : m["content"])
        if (get(p,"type") == "image") out.push_back(p.value("source",""));
    return out;
}
std::string forced_call(const TextJson& choice, const TextJson& tools) {
    std::string kind = "unknown", name;
    if (choice.is_null()) kind = "auto";
    else if (choice.is_string()) kind = choice.get<std::string>();
    else if (choice.is_object()) {
        auto t = get(choice,"type");
        if (t == "auto" || t == "none") kind = t.get<std::string>();
        else if (t == "any" || t == "required") kind = "required";
        else if (t == "function" || t == "tool") {
            auto n = get(get(choice,"function"),"name"); if (!truth(n)) n = get(choice,"name");
            if (n.is_string() && !n.get<std::string>().empty()) { kind = "named"; name = n.get<std::string>(); }
        }
    }
    std::set<std::string> names;
    if (tools.is_array()) for (const auto& t : tools) if (get(t,"name").is_string()) names.insert(t["name"].get<std::string>());
    if (kind == "required" && names.size() == 1) { kind = "named"; name = *names.begin(); }
    if (kind == "required" && !names.empty()) return "<tool_call>\n<function=";
    if (kind == "named" && names.contains(name)) return "<tool_call>\n<function=" + name + ">\n";
    return {};
}
} // namespace strata::runtime
