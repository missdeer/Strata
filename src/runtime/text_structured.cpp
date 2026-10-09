#include "strata/runtime/text.hpp"
#include <jsoncons/json.hpp>
#include <jsoncons_ext/jsonschema/jsonschema.hpp>
#include <set>
#include <stdexcept>

namespace strata::runtime {
namespace {
namespace js = jsoncons::jsonschema;
using SchemaJson = jsoncons::json;
bool only_objects(const TextJson& node, const TextJson& root, std::set<std::string> refs = {}) {
    if (!node.is_object()) return false;
    if (node.value("type",TextJson()) == "object") return true;
    const auto ref = node.value("$ref",TextJson());
    if (ref.is_string() && ref.get<std::string>().starts_with('#') && refs.insert(ref.get<std::string>()).second) {
        try { return only_objects(root.at(TextJson::json_pointer(ref.get<std::string>().substr(1))),root,refs); }
        catch (const std::exception&) { return false; }
    }
    for (const auto* key : {"anyOf","oneOf"}) if (node.contains(key) && node[key].is_array() && !node[key].empty()) {
        bool all = true; for (const auto& branch : node[key]) all &= only_objects(branch,root,refs);
        if (all) return true;
    }
    if (node.contains("allOf") && node["allOf"].is_array()) for (const auto& b : node["allOf"]) if (only_objects(b,root,refs)) return true;
    return false;
}
void check_refs(const TextJson& node) {
    if (node.is_object()) for (const auto& item : node.items()) {
        if ((item.key() == "$ref" || item.key() == "$dynamicRef") && item.value().is_string() && !item.value().get<std::string>().starts_with('#'))
            throw std::invalid_argument("response_format supports only local schema references (#...)");
        check_refs(item.value());
    } else if (node.is_array()) for (const auto& v : node) check_refs(v);
}
auto compile_schema(const TextJson& schema) {
    const auto version = schema.value("$schema",js::schema_version::draft202012());
    const auto meta = js::meta_resolver<SchemaJson>(jsoncons::uri(version));
    if (meta.is_null()) throw std::invalid_argument("unsupported JSON Schema dialect: " + version);
    auto no_remote = [](const jsoncons::uri&) { return SchemaJson::null(); };
    js::evaluation_options options;
    options.default_version(version).require_format_validation(false);
    auto parsed = SchemaJson::parse(schema.dump());
    // make_json_schema is a compiler, not a substitute for check_schema.
    auto meta_validator = js::make_json_schema(meta,no_remote,options);
    meta_validator.validate(parsed);
    return js::make_json_schema(std::move(parsed),no_remote,options);
}
std::string extract_json(const std::string& text) {
    auto s = trim_text(text); if (s.empty()) return {};
    if (s.starts_with("```")) {
        auto first = s.find('\n');
        if (first != std::string::npos) { auto end = s.find("```",first+1); if (end != std::string::npos) s = trim_text(s.substr(first+1,end-first-1)); }
    }
    const auto start = s.find('{'); if (start == std::string::npos) return s;
    size_t depth = 0; bool quoted = false, escape = false;
    for (size_t i = start; i < s.size(); ++i) {
        const auto c = s[i];
        if (quoted) { if (escape) escape = false; else if (c == '\\') escape = true; else if (c == '"') quoted = false; }
        else if (c == '"') quoted = true;
        else if (c == '{') ++depth;
        else if (c == '}' && --depth == 0) return s.substr(start,i+1-start);
    }
    return s.substr(start);
}
}
struct StructuredFormat::Impl {
    js::json_schema<SchemaJson> compiled;
    explicit Impl(const TextJson& schema) : compiled(compile_schema(schema)) {}
};
StructuredFormat StructuredFormat::prepare(const TextJson& format, TextJson& messages, bool tools) {
    StructuredFormat out;
    if (format.is_null()) return out;
    if (!format.is_object()) throw std::invalid_argument("response_format must be an object");
    const auto kind = format.value("type",TextJson());
    if (kind == "text") return out;
    if (kind == "json_object") out.schema_ = {{"type","object"}};
    else if (kind == "json_schema") {
        const auto spec = format.value("json_schema",TextJson());
        if (!spec.is_object() || !spec.contains("schema") || !spec["schema"].is_object()) throw std::invalid_argument("response_format.json_schema needs a schema object");
        if (!spec.value("name",TextJson()).is_string() || spec["name"].get<std::string>().empty()) throw std::invalid_argument("response_format.json_schema needs a name");
        if (spec.contains("strict") && !spec["strict"].is_boolean()) throw std::invalid_argument("response_format.json_schema.strict must be boolean");
        out.schema_ = spec["schema"];
        if (!only_objects(out.schema_,out.schema_)) throw std::invalid_argument("response_format schema must accept only JSON objects at its root");
    } else throw std::invalid_argument("response_format.type must be text, json_object or json_schema");
    check_refs(out.schema_);
    try { out.impl_ = std::make_shared<Impl>(out.schema_); }
    catch (const std::exception& e) { throw std::invalid_argument(std::string("invalid response_format schema: ")+e.what()); }
    std::string directive = "OUTPUT FORMAT REQUIREMENT: Return exactly one JSON object matching the JSON Schema below. "
        "No Markdown, headings, code fences, commentary, or text outside the JSON. "
        "Use every required field, correct types, and only allowed fields. "
        "Put all requested writing inside the appropriate JSON string fields.\nJSON Schema:\n" + python_json(out.schema_,false,true);
    if (tools) directive += "\nThis applies only to your final answer. To use a tool, call it as usual; the JSON object is what you write once you are done with the tools.";
    if (!messages.empty() && messages[0].value("role", "") == "system") {
        auto& content = messages[0]["content"];
        if (content.is_array()) content.push_back({{"type","text"},{"text",directive}});
        else content = (content.is_null() ? "" : content.get<std::string>()) + "\n\n" + directive;
    } else messages.insert(messages.begin(),TextJson{{"role","system"},{"content",directive}});
    return out;
}
std::string StructuredFormat::validate(const std::string& text, const std::string& finish) const {
    if (!enabled()) return text;
    if (finish != "stop") throw std::runtime_error("structured output was incomplete (finish_reason=" + finish + "); increase the output budget");
    TextJson value;
    try { value = parse_text_json(extract_json(text)); }
    catch (const std::exception& e) { throw std::runtime_error(std::string("model did not return valid JSON: ")+e.what()); }
    const auto canonical = python_json(value,false,true);
    try { impl_->compiled.validate(SchemaJson::parse(canonical)); }
    catch (const std::exception& e) { throw std::runtime_error(std::string("model output failed the JSON Schema: ")+e.what()); }
    return canonical;
}
} // namespace strata::runtime
