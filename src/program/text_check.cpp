// JSON-lines parity driver. This is a native text runtime diagnostic, not an API
// server. One tokenizer/template is retained across requests to test shared use.
#include "strata/runtime/text.hpp"
#include <iostream>
#include <map>

using namespace strata::runtime;
namespace {
TextJson call_json(const ToolCall& call) { return {{"name",call.name},{"arguments",call.arguments},{"id",call.id}}; }
TextJson event_json(const OutputEvent& event) {
    TextJson out = {{"kind",event.kind},{"text",event.text}};
    out["call"] = event.call ? call_json(*event.call) : TextJson(); return out;
}
}
static int run(const std::vector<std::string>& argv) {
    try {
        std::optional<Tokenizer> tokenizer;
        std::optional<ChatTemplate> chat;
        for (size_t i = 1; i < argv.size(); ++i) {
            const std::string flag = argv[i];
            if (i+1 == argv.size()) throw std::invalid_argument("missing argument");
            const auto& arg = argv[++i];
            const auto path = std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(arg.data()),arg.size()));
            if (flag == "--tokenizer") tokenizer.emplace(Tokenizer::load(path));
            else if (flag == "--template") chat.emplace(path);
            else throw std::invalid_argument("unknown flag: " + flag);
        }
        std::string line;
        while (std::getline(std::cin,line)) {
            try {
                const auto req = parse_text_json(line); const auto op = req.at("op").get<std::string>();
                TextJson out;
                if (op == "encode") {
                    if (!tokenizer) throw std::invalid_argument("no tokenizer loaded");
                    out = tokenizer->encode(req.at("text"),req.value("parse_special",false),req.value("plain",PlainSpans{}));
                } else if (op == "decode") {
                    if (!tokenizer) throw std::invalid_argument("no tokenizer loaded");
                    out = tokenizer->decode(req.at("ids"));
                } else if (op == "detokenize") {
                    if (!tokenizer) throw std::invalid_argument("no tokenizer loaded");
                    Utf8Decoder decoder; out = TextJson::array();
                    for (int id : req.at("ids")) out.push_back(decoder.push(tokenizer->token_bytes(id)));
                    if (req.value("final",false)) out.push_back(decoder.push({},true));
                } else if (op == "utf8") {
                    Utf8Decoder decoder; out = TextJson::array();
                    for (const auto& chunk : req.at("chunks")) {
                        std::string bytes; for (int b : chunk) { if (b < 0 || b > 255) throw std::invalid_argument("invalid byte"); bytes += static_cast<char>(b); }
                        out.push_back(decoder.push(bytes));
                    }
                    if (req.value("final",false)) out.push_back(decoder.push({},true));
                } else if (op == "normalize") {
                    const auto value = req.value("api","openai") == "anthropic" ? anthropic_to_messages(req.at("request"),req.value("think_unasked",true)) : openai_to_messages(req.at("request"));
                    out = {{"messages",value.messages},{"tools",value.tools},{"kwargs",value.kwargs},{"images",images_of(value.messages)}};
                } else if (op == "render" || op == "prompt") {
                    if (!chat) throw std::invalid_argument("no template loaded");
                    TemplateRequest value{req.at("messages"),req.value("tools",TextJson()),req.value("kwargs",TextJson::object())};
                    if (op == "render") out = chat->render(value,req.value("generation_prompt",true));
                    else {
                        const auto rendered = chat->render_prompt(value,tokenizer ? tokenizer->controls() : std::vector<std::string>{},req.value("generation_prompt",true));
                        out = {{"text",rendered.text},{"plain",rendered.plain}};
                        if (tokenizer) out["ids"] = tokenizer->encode(rendered.text,true,rendered.plain);
                    }
                } else if (op == "capabilities") {
                    if (!chat) throw std::invalid_argument("no template loaded"); out = chat->capabilities();
                } else if (op == "parse") {
                    OutputParser parser(req.value("thinking",true),req.value("tools",TextJson()),req.value("stream_tools",false),req.value("recover",false));
                    out = {{"events",TextJson::array()}};
                    for (const auto& chunk : req.at("chunks")) for (const auto& e : parser.feed(chunk)) out["events"].push_back(event_json(e));
                    for (const auto& e : parser.finish(req.value("finish","stop"))) out["events"].push_back(event_json(e));
                    out["rescued"] = parser.rescued(); out["refused"] = parser.refused();
                } else if (op == "call") out = call_json(parse_tool_call(req.at("body"),req.value("schema",TextJson())));
                else if (op == "forced_call") out = forced_call(req.value("choice",TextJson()),req.value("tools",TextJson()));
                else if (op == "stop") {
                    StopMatcher matcher(stop_strings(req.value("request",TextJson::object()))); out = {{"chunks",TextJson::array()}};
                    for (const auto& s : req.at("chunks")) out["chunks"].push_back(matcher.push(s));
                    out["tail"] = matcher.flush(); out["hit"] = matcher.hit ? TextJson(*matcher.hit) : TextJson();
                } else if (op == "structured") {
                    auto messages = req.value("messages",TextJson::array());
                    auto format = StructuredFormat::prepare(req.value("format",TextJson()),messages,req.value("with_tools",false));
                    out = {{"messages",messages},{"enabled",format.enabled()}};
                    if (req.contains("text")) out["text"] = format.validate(req["text"],req.value("finish","stop"));
                } else throw std::invalid_argument("unknown operation: " + op);
                std::cout << TextJson{{"result",out}}.dump() << '\n';
            } catch (const std::exception& e) { std::cout << TextJson{{"error",e.what()}}.dump() << '\n'; }
            std::cout.flush();
        }
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
    return 0;
}
#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    std::vector<std::string> args;
    for (int i = 0; i < argc; ++i) {
        const auto value = std::filesystem::path(argv[i]).u8string();
        args.emplace_back(reinterpret_cast<const char*>(value.data()),value.size());
    }
    return run(args);
}
#else
int main(int argc, char** argv) { return run(std::vector<std::string>(argv,argv+argc)); }
#endif
