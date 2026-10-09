#include "strata/runtime/text.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace strata::runtime {
namespace {
const std::string think_end = "</think>", call_start = "<tool_call>", call_close = "</tool_call>",
    param_start = "<parameter=", param_close = "</parameter>", func_start = "<function=", func_close = "</function>";
using Events = std::vector<OutputEvent>;
using Pos = std::ptrdiff_t;
Pos find(const std::string& s, const std::string& tag, size_t p = 0) {
    auto i = s.find(tag,p); return i == std::string::npos ? -1 : static_cast<Pos>(i);
}
std::string lstrip(std::string s, bool newline = false) {
    if (newline) { const auto p = s.find_first_not_of('\n'); return p == std::string::npos ? "" : s.substr(p); }
    const auto stripped = trim_text(s);
    if (stripped.empty()) return {};
    // trim_text only removes whitespace, so its first byte identifies the start.
    return s.substr(s.find(stripped));
}
std::string rstrip_newlines(std::string s) { while (!s.empty() && s.back() == '\n') s.pop_back(); return s; }
void append(Events& a, Events b) { a.insert(a.end(),std::make_move_iterator(b.begin()),std::make_move_iterator(b.end())); }
size_t hold(const std::string& s, const std::vector<std::string>& tags) {
    size_t best = 0;
    for (const auto& tag : tags) for (size_t n = 1; n < tag.size(); ++n) if (s.ends_with(tag.substr(0,n))) best = std::max(best,n);
    return best;
}
Pos parameter_end(const std::string& s, bool final = false) {
    auto at = find(s,param_close);
    while (at >= 0) {
        const auto after = lstrip(s.substr(static_cast<size_t>(at)+param_close.size()));
        if (after.starts_with(param_start) || after.starts_with(func_close)) return at;
        if (after.empty() || param_start.starts_with(after) || func_close.starts_with(after)) return final ? at : -2;
        at = find(s,param_close,static_cast<size_t>(at)+1);
    }
    return -1;
}
Pos function_end(const std::string& text) {
    auto s = lstrip(text); size_t pos = text.size()-s.size();
    if (!s.starts_with(func_start)) return -1;
    auto gt = find(text,">",pos); if (gt < 0) return -1;
    pos = static_cast<size_t>(gt)+1;
    while (true) {
        auto rest = text.substr(pos); s = lstrip(rest); pos += rest.size()-s.size();
        if (s.starts_with(param_start)) {
            gt = find(text,">",pos); if (gt < 0) return -1;
            auto end = parameter_end(text.substr(static_cast<size_t>(gt)+1)); if (end < 0) return -1;
            pos = static_cast<size_t>(gt+1+end)+param_close.size();
        } else if (s.starts_with(func_close)) return static_cast<Pos>(pos+func_close.size());
        else return -1;
    }
}
Pos call_end(const std::string& text) {
    auto s = lstrip(text); size_t pos = text.size()-s.size();
    if (!s.starts_with(func_start)) return func_start.starts_with(s) ? -1 : find(text,call_close);
    auto gt = find(text,">",pos); if (gt < 0) return -1;
    pos = static_cast<size_t>(gt)+1;
    while (true) {
        auto rest = text.substr(pos); s = lstrip(rest); pos += rest.size()-s.size();
        if (s.starts_with(param_start)) {
            gt = find(text,">",pos); if (gt < 0) return -1;
            auto end = parameter_end(text.substr(static_cast<size_t>(gt)+1)); if (end < 0) return -1;
            pos = static_cast<size_t>(gt+1+end)+param_close.size();
        } else if (s.starts_with(func_close)) {
            rest = text.substr(pos+func_close.size()); s = lstrip(rest);
            if (s.starts_with(call_close)) return static_cast<Pos>(pos+func_close.size()+rest.size()-s.size());
            return call_close.starts_with(s) ? -1 : find(text,call_close,pos);
        } else if (s.empty() || param_start.starts_with(s) || func_close.starts_with(s)) return -1;
        else return find(text,call_close,pos);
    }
}
std::string call_name(const std::string& body) {
    const auto s = trim_text(body);
    if (s.size() < func_start.size()) return {};
    return trim_text(s.substr(func_start.size(),s.find('>')-func_start.size()));
}
std::string new_id() {
    static std::atomic<uint64_t> counter{0};
    const auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    std::ostringstream out; out << "call_" << std::hex << std::setfill('0') << std::setw(16) << now << std::setw(8) << ++counter;
    return out.str();
}
TextJson properties(const TextJson& schema) {
    if (!schema.is_object()) return TextJson::object();
    auto p = schema.value("parameters",TextJson::object());
    if (!p.is_object()) return TextJson::object();
    auto props = p.value("properties",TextJson::object()); return props.is_object() ? props : TextJson::object();
}
TextJson loose_json(const std::string& s) {
    auto v = TextJson::parse(s,nullptr,false); return v.is_discarded() ? TextJson(s) : v;
}
}
ToolCall parse_tool_call(std::string body, const TextJson& schema) {
    body = trim_text(body);
    auto gt = find(body,">");
    if (!body.starts_with(func_start) || gt < 0) throw std::invalid_argument("malformed tool call: " + body.substr(0,80));
    ToolCall call{call_name(body),TextJson::object(),new_id()};
    auto rest = body.substr(static_cast<size_t>(gt)+1); const auto props = properties(schema);
    while (rest.find(param_start) != std::string::npos) {
        rest = rest.substr(rest.find(param_start)+param_start.size());
        gt = find(rest,">"); if (gt < 0) throw std::invalid_argument("malformed parameter");
        auto pname = rest.substr(0,static_cast<size_t>(gt)); rest = rest.substr(static_cast<size_t>(gt)+1);
        auto end = parameter_end(rest,true); auto value = end >= 0 ? rest.substr(0,static_cast<size_t>(end)) : rest;
        rest = end >= 0 ? rest.substr(static_cast<size_t>(end)+param_close.size()) : "";
        if (value.starts_with('\n')) value.erase(0,1);
        if (value.ends_with('\n')) value.pop_back();
        const auto prop = props.value(pname,TextJson::object());
        call.arguments[pname] = prop.is_object() && prop.value("type",TextJson()) == "string" ? TextJson(value) : loose_json(value);
    }
    return call;
}

struct OutputParser::Impl {
    std::string state, buf, fence, line;
    bool lead = false, stream, recover, bare = false, batch = false;
    size_t ticks = 0, rescued = 0, refused = 0;
    TextJson schemas = TextJson::object();
    std::vector<std::pair<std::string,std::optional<ToolCall>>> pending;
    size_t sp = 0;
    std::string ss = "name";
    std::optional<ToolCall> scall;
    bool sfirst = true, sval_started = false;
    TextJson declared = TextJson::object();
    Impl(bool thinking, const TextJson& tools, bool streaming, bool recovery)
        : state(thinking ? "reasoning" : "content"), stream(streaming), recover(recovery) {
        if (tools.is_array()) for (const auto& t : tools) if (t.is_object() && t.contains("name") && t["name"].is_string()) schemas[t["name"].get<std::string>()] = t;
    }
    std::string track(const std::string& text) {
        size_t pos = 0;
        while (true) {
            auto end = text.find('\n',pos);
            if (end == std::string::npos) { line += text.substr(pos); break; }
            line += text.substr(pos,end-pos); const auto s = lstrip(line);
            if (!fence.empty()) { if (s.starts_with(std::string(3,fence[0]))) fence.clear(); }
            else if ((s.starts_with("```") || s.starts_with("~~~")) && s.substr(3).find(std::string(3,s[0])) == std::string::npos) fence = s.substr(0,1);
            else if (s.empty()) ticks = 0;
            else ticks += static_cast<size_t>(std::count(line.begin(),line.end(),'`'));
            line.clear(); pos = end+1;
        }
        return text;
    }
    bool opener_ok() const { return fence.empty() && trim_text(line).empty() && ticks%2 == 0; }
    bool in_code() const { return !fence.empty() || (ticks+static_cast<size_t>(std::count(line.begin(),line.end(),'`')))%2 == 1; }
    bool ok_at(size_t p) {
        auto f = fence, l = line; auto t = ticks; track(buf.substr(0,p)); const auto ok = opener_ok(); fence = f; line = l; ticks = t; return ok;
    }
    std::string follower(const std::string& after) const {
        if (after.starts_with(func_start)) return "call";
        if (after.starts_with(param_start)) {
            auto gt = after.find('>');
            if (gt == std::string::npos) return after.find('\n') == std::string::npos ? "wait" : "text";
            return schemas.contains(after.substr(param_start.size(),gt-param_start.size())) ? "param" : "text";
        }
        if (after.starts_with('{')) return schemas.empty() ? "text" : "json";
        if (after.empty() || func_start.starts_with(after) || param_start.starts_with(after)) return "wait";
        return "text";
    }
    std::pair<Pos,bool> bare_opener() {
        auto p = find(buf,func_start);
        while (p >= 0) {
            if (ok_at(static_cast<size_t>(p))) {
                auto gt = find(buf,">",static_cast<size_t>(p));
                auto name = buf.substr(static_cast<size_t>(p)+func_start.size(),gt < 0 ? std::string::npos : static_cast<size_t>(gt-p)-func_start.size());
                if (gt < 0 && trim_text(name) == name && name.find_first_of(" \t\r\n") == std::string::npos) return {p,false};
                if (gt >= 0 && schemas.contains(name)) return {p,true};
            }
            p = find(buf,func_start,static_cast<size_t>(p)+1);
        }
        return {-1,false};
    }
    Events release(bool deliver) {
        Events out;
        for (const auto& [raw,call] : pending) {
            if (call && deliver) { out.push_back({"tool_call","",call}); ++rescued; }
            else { if (call) ++refused; if (!raw.empty()) out.push_back({"reasoning",track(raw),{}}); }
        }
        pending.clear(); return out;
    }
    void reset_scan() { sp = 0; ss = "name"; scall.reset(); sfirst = true; sval_started = false; declared = TextJson::object(); }
    Events scan() {
        Events out;
        auto args = [&](const std::string& s) { if (!s.empty()) out.push_back({"tool_args",s,scall}); };
        while (true) {
            auto rest = buf.substr(sp);
            if (ss == "name") {
                auto a = find(rest,func_start), b = a >= 0 ? find(rest,">",static_cast<size_t>(a)+func_start.size()) : -1;
                if (b < 0) return out;
                const auto name = rest.substr(static_cast<size_t>(a)+func_start.size(),static_cast<size_t>(b-a)-func_start.size());
                scall = ToolCall{name,TextJson::object(),new_id()};
                const auto props = properties(schemas.value(name,TextJson()));
                for (const auto& p : props.items()) declared[p.key()] = p.value().is_object() ? p.value().value("type",TextJson()) : TextJson();
                out.push_back({"tool_start","",scall}); args("{"); sp += static_cast<size_t>(b)+1; ss = "between";
            } else if (ss == "between") {
                const auto stripped = lstrip(rest); sp += rest.size()-stripped.size();
                if (stripped.starts_with(param_start)) {
                    auto b = find(stripped,">"); if (b < 0) return out;
                    const auto name = stripped.substr(param_start.size(),static_cast<size_t>(b)-param_start.size());
                    args((sfirst ? "" : ",") + python_json(name,true) + ":"); sfirst = false; sp += static_cast<size_t>(b)+1;
                    if (declared.value(name,TextJson()) == "string") { args("\""); ss = "str"; sval_started = false; }
                    else ss = "raw";
                } else if (stripped.starts_with(func_close)) { args("}"); sp += func_close.size(); ss = "done"; }
                else return out;
            } else if (ss == "str") {
                if (!sval_started) { if (rest.empty()) return out; if (rest[0] == '\n') { ++sp; rest.erase(0,1); } sval_started = true; }
                auto end = parameter_end(rest);
                if (end >= 0) {
                    auto value = rest.substr(0,static_cast<size_t>(end)); if (value.ends_with('\n')) value.pop_back();
                    const auto encoded = python_json(value,true); args(encoded.substr(1,encoded.size()-2)+"\"");
                    sp += static_cast<size_t>(end)+param_close.size(); ss = "between"; continue;
                }
                auto safe = end == -2 ? rest.find(param_close) : rest.size()-hold(rest,{param_close});
                if (safe > 0 && rest[safe-1] == '\n') --safe;
                if (safe > 0) { const auto encoded = python_json(rest.substr(0,safe),true); args(encoded.substr(1,encoded.size()-2)); sp += safe; }
                return out;
            } else if (ss == "raw") {
                auto end = parameter_end(rest); if (end < 0) return out;
                auto value = rest.substr(0,static_cast<size_t>(end)); if (value.starts_with('\n')) value.erase(0,1); if (value.ends_with('\n')) value.pop_back();
                args(python_json(loose_json(value))); sp += static_cast<size_t>(end)+param_close.size(); ss = "between";
            } else return out;
        }
    }
    Events close_scan() {
        if (!scall || ss == "done") return {};
        std::string tail;
        if (ss == "str") tail = "\"";
        else if (ss == "raw") tail = python_json(rstrip_newlines(lstrip(buf.substr(sp),true)),true);
        ss = "done"; return {{"tool_args",tail+"}",scall}};
    }
    Events finish_call(const std::string& body) {
        const auto name = call_name(body); const bool was_bare = bare; bare = false;
        ToolCall call;
        try { call = parse_tool_call(body,schemas.value(name,TextJson())); }
        catch (const std::exception&) {
            if (!recover) throw;
            reset_scan(); state = "content"; return {{"content",track(was_bare ? body : call_start+body+call_close),{}}};
        }
        if (scall) call.id = scall->id;
        reset_scan(); state = "content"; lead = true; return {{"tool_call","",call}};
    }
    std::optional<ToolCall> json_call(const std::string& body) {
        auto obj = TextJson::parse(body,nullptr,false);
        if (!obj.is_object() || !obj.value("name",TextJson()).is_string() || !schemas.contains(obj["name"].get<std::string>())) return {};
        auto args = obj.value("arguments",obj.value("parameters",TextJson::object()));
        if (args.is_string()) args = TextJson::parse(args.get<std::string>(),nullptr,false);
        if (args.is_null()) args = TextJson::object();
        if (!args.is_object()) return {};
        return ToolCall{obj["name"].get<std::string>(),args,new_id()};
    }
    Events feed(const std::string& delta) {
        buf += delta; Events out;
        while (true) {
            if (state == "rcall") {
                const auto body = buf.substr(call_start.size()); const auto end = call_end(body), think = find(body,think_end);
                if (end >= 0 && (think < 0 || think >= end)) {
                    std::optional<ToolCall> call;
                    try { const auto name = call_name(body.substr(0,static_cast<size_t>(end))); if (schemas.contains(name)) call = parse_tool_call(body.substr(0,static_cast<size_t>(end)),schemas[name]); }
                    catch (const std::exception&) {}
                    auto raw = buf.substr(0,call_start.size()+static_cast<size_t>(end)+call_close.size());
                    if (call || !pending.empty()) pending.emplace_back(raw,call);
                    else out.push_back({"reasoning",track(raw),{}});
                    buf = body.substr(static_cast<size_t>(end)+call_close.size()); state = "reasoning";
                } else if (think >= 0) {
                    append(out,release(false)); out.push_back({"reasoning",track(buf.substr(0,call_start.size()+static_cast<size_t>(think))),{}});
                    buf = body.substr(static_cast<size_t>(think)); state = "reasoning";
                } else if (static_cast<size_t>(std::count_if(body.begin(),body.end(),[](unsigned char c) { return (c & 0xc0) != 0x80; })) > 32768) {
                    append(out,release(false)); out.push_back({"reasoning",track(buf),{}}); buf.clear(); state = "reasoning";
                } else return out;
            } else if (state == "reasoning") {
                if (!pending.empty()) {
                    auto stripped = lstrip(buf);
                    if (stripped.size() < buf.size()) { pending.back().first += buf.substr(0,buf.size()-stripped.size()); buf = stripped; }
                    if (buf.empty()) return out;
                    if (buf.starts_with(call_start)) state = "rcall";
                    else if (buf.starts_with(think_end)) append(out,release(true));
                    else if (call_start.starts_with(buf) || think_end.starts_with(buf)) return out;
                    else append(out,release(false));
                    continue;
                }
                const auto i = find(buf,think_end), tool = schemas.empty() ? -1 : find(buf,call_start);
                if (tool >= 0 && (i < 0 || tool < i)) {
                    if (tool) out.push_back({"reasoning",track(buf.substr(0,static_cast<size_t>(tool))),{}});
                    if (opener_ok()) { buf = buf.substr(static_cast<size_t>(tool)); state = "rcall"; }
                    else { out.push_back({"reasoning",track(call_start),{}}); buf = buf.substr(static_cast<size_t>(tool)+call_start.size()); }
                    continue;
                }
                if (i < 0) {
                    auto keep = hold(buf,schemas.empty() ? std::vector<std::string>{think_end} : std::vector<std::string>{think_end,call_start});
                    if (buf.size() > keep) { out.push_back({"reasoning",track(buf.substr(0,buf.size()-keep)),{}}); buf = buf.substr(buf.size()-keep); }
                    return out;
                }
                if (i) out.push_back({"reasoning",track(buf.substr(0,static_cast<size_t>(i))),{}});
                buf = buf.substr(static_cast<size_t>(i)+think_end.size()); state = "content"; lead = true; fence.clear(); line.clear(); ticks = 0;
            } else if (state == "content") {
                if (lead) { auto stripped = lstrip(buf,true); if (stripped.empty()) { buf.clear(); return out; } buf = stripped; lead = false; }
                const auto i = find(buf,call_start); const auto [b,decided] = recover && !schemas.empty() ? bare_opener() : std::pair<Pos,bool>{-1,false};
                if (b >= 0 && (i < 0 || b < i)) {
                    auto j = static_cast<size_t>(b); while (j > 0 && buf[j-1] == '\n') --j;
                    if (j > 0 && !trim_text(buf.substr(0,j)).empty()) out.push_back({"content",track(buf.substr(0,j)),{}});
                    if (!decided) { buf = buf.substr(j); return out; }
                    buf = buf.substr(static_cast<size_t>(b)); state = "call"; bare = true; continue;
                }
                if (i < 0) {
                    auto j = buf.size()-hold(buf,{call_start});
                    if (recover && !schemas.empty()) {
                        auto k = buf.rfind('\n'); k = k == std::string::npos ? 0 : k+1;
                        auto tail = buf.substr(k); if (!tail.empty() && func_start.starts_with(tail) && ok_at(k)) j = std::min(j,k);
                    }
                    while (j > 0 && buf[j-1] == '\n') --j;
                    if (j > 0) { out.push_back({"content",track(buf.substr(0,j)),{}}); buf = buf.substr(j); }
                    return out;
                }
                auto f = fence, l = line; auto t = ticks; track(buf.substr(0,static_cast<size_t>(i))); auto code = in_code(); fence = f; line = l; ticks = t;
                if (code) { auto n = static_cast<size_t>(i)+call_start.size(); out.push_back({"content",track(buf.substr(0,n)),{}}); buf = buf.substr(n); continue; }
                auto after = lstrip(buf.substr(static_cast<size_t>(i)+call_start.size())); auto kind = recover ? follower(after) : "";
                if (kind == "param" || kind == "json") {
                    if (i && !trim_text(buf.substr(0,static_cast<size_t>(i))).empty()) out.push_back({"content",track(rstrip_newlines(buf.substr(0,static_cast<size_t>(i)))),{}});
                    auto rest = buf.substr(static_cast<size_t>(i)+call_start.size());
                    if (kind == "param") rest = rest.substr(0,rest.size()-after.size()) + func_start + after.substr(param_start.size());
                    buf = rest; state = kind == "param" ? "call" : "json"; continue;
                }
                if (kind == "wait") after.clear();
                if (!after.empty() && !after.starts_with(func_start) && !func_start.starts_with(after)) {
                    auto n = static_cast<size_t>(i)+call_start.size(); out.push_back({"content",track(buf.substr(0,n)),{}}); buf = buf.substr(n); continue;
                }
                if (!after.starts_with(func_start)) {
                    auto j = static_cast<size_t>(i); while (j > 0 && buf[j-1] == '\n') --j;
                    if (j > 0) { out.push_back({"content",track(buf.substr(0,j)),{}}); buf = buf.substr(j); } return out;
                }
                if (i && !trim_text(buf.substr(0,static_cast<size_t>(i))).empty()) out.push_back({"content",track(rstrip_newlines(buf.substr(0,static_cast<size_t>(i)))),{}});
                buf = buf.substr(static_cast<size_t>(i)+call_start.size()); state = "call";
            } else if (state == "json") {
                auto i = find(buf,call_close); if (i < 0) return out;
                auto call = json_call(trim_text(buf.substr(0,static_cast<size_t>(i))));
                if (call) out.push_back({"tool_call","",call}); else out.push_back({"content",track(call_start+buf.substr(0,static_cast<size_t>(i)+call_close.size())),{}});
                buf = buf.substr(static_cast<size_t>(i)+call_close.size()); state = "content"; lead = call.has_value();
            } else {
                Pos drop = -1, i;
                if (bare) {
                    i = function_end(buf);
                    if (i >= 0) { auto after = lstrip(buf.substr(static_cast<size_t>(i)));
                        if (after.starts_with(call_close)) drop = static_cast<Pos>(buf.size()-static_cast<size_t>(i)-after.size()+call_close.size());
                        else if (!after.empty() && !call_close.starts_with(after)) drop = 0;
                    }
                } else {
                    i = call_end(buf); if (i >= 0) drop = static_cast<Pos>(call_close.size());
                    if (recover) {
                        const auto f = function_end(buf); auto next = f >= 0 ? lstrip(buf.substr(static_cast<size_t>(f))) : "";
                        if (next.starts_with(param_start) && follower(next) == "param") { auto at = buf.size()-next.size(); buf = buf.substr(0,at)+func_start+next.substr(param_start.size()); next = buf.substr(at); }
                        if (next.starts_with(func_start)) { i = f; drop = 0; batch = true; }
                    }
                }
                if (stream) {
                    if (i >= 0) { auto whole = buf; buf = buf.substr(0,static_cast<size_t>(i)); append(out,scan()); append(out,close_scan()); buf = whole; }
                    else append(out,scan());
                }
                if (i < 0 || drop < 0) return out;
                auto body = buf.substr(0,static_cast<size_t>(i)); buf = buf.substr(static_cast<size_t>(i+drop));
                bool was_batch = batch; batch = false; append(out,finish_call(body));
                if (was_batch && out.back().kind == "tool_call") { state = "call"; lead = false; }
            }
        }
    }
    Events finish(const std::string& reason) {
        Events out;
        if (!pending.empty()) append(out,release((reason.empty() || reason == "stop") && state == "reasoning" && buf.empty()));
        if (state == "call" && bare && function_end(buf) >= 0) {
            const auto i = static_cast<size_t>(function_end(buf)); const auto body = buf.substr(0,i), rest = trim_text(buf.substr(i)); buf.clear();
            append(out,finish_call(body)); if (!rest.empty() && !call_close.starts_with(rest)) out.push_back({"content",track(rest),{}}); return out;
        }
        if (state == "call" && stream && scall) {
            append(out,scan()); if (ss == "done") out.push_back({"tool_call","",scall}); buf.clear(); reset_scan(); return out;
        }
        if (!buf.empty()) {
            auto kind = state == "reasoning" || state == "rcall" ? "reasoning" : "content";
            auto text = (state == "call" || state == "json") && !bare ? call_start+buf : buf;
            out.push_back({kind,text,{}}); buf.clear();
        }
        return out;
    }
};
OutputParser::OutputParser(bool thinking, const TextJson& tools, bool stream_tools, bool recover)
    : impl_(std::make_unique<Impl>(thinking,tools,stream_tools,recover)) {}
OutputParser::~OutputParser() = default;
OutputParser::OutputParser(OutputParser&&) noexcept = default;
OutputParser& OutputParser::operator=(OutputParser&&) noexcept = default;
std::vector<OutputEvent> OutputParser::feed(const std::string& delta) { return impl_->feed(delta); }
std::vector<OutputEvent> OutputParser::finish(const std::string& reason) { return impl_->finish(reason); }
size_t OutputParser::rescued() const { return impl_->rescued; }
size_t OutputParser::refused() const { return impl_->refused; }
} // namespace strata::runtime
