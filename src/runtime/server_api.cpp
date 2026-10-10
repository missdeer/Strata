#include "strata/runtime/server_api.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <random>
#include <sstream>

namespace strata::runtime {
namespace {
using J = TextJson;
std::string text_content(const J &value) {
    if (value.is_null())
        return "";
    if (value.is_string())
        return value.get<std::string>();
    if (!value.is_array())
        throw ApiError(400, "content must be text or an array");
    std::string out;
    for (const auto &p : value) {
        if (p.is_string()) {
            out += p.get<std::string>();
            continue;
        }
        if (!p.is_object())
            throw ApiError(400, "content parts must be objects");
        const auto kind = p.value("type", "");
        if (kind == "input_text" || kind == "output_text" || kind == "text" || kind == "reasoning_text" ||
            kind == "summary_text")
            out += p.value("text", "");
        else if (kind == "refusal")
            out += p.value("refusal", "");
        else
            throw ApiError(400, "native text server does not support content part: " + kind);
    }
    return out;
}
std::string base64(const std::string &bytes) {
    const char *alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    uint32_t held = 0;
    int bits = 0;
    for (unsigned char c : bytes) {
        held = (held << 8) | c;
        bits += 8;
        while (bits >= 6) {
            bits -= 6;
            result += alphabet[(held >> bits) & 63];
        }
    }
    if (bits)
        result += alphabet[(held << (6 - bits)) & 63];
    while (result.size() % 4)
        result += '=';
    return result;
}
std::string unbase64(const std::string &input) {
    std::string result;
    uint32_t held = 0;
    int bits = 0;
    bool padded = false;
    const std::string alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (char c : input) {
        if (c == '=') {
            padded = true;
            continue;
        }
        const auto p = alphabet.find(c);
        if (p == std::string::npos || padded)
            throw ApiError(400, "damaged reasoning encrypted_content");
        held = (held << 6) | static_cast<uint32_t>(p);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            result += static_cast<char>((held >> bits) & 255);
        }
    }
    if (input.size() % 4)
        throw ApiError(400, "damaged reasoning encrypted_content");
    return result;
}
J responses_messages(const J &req) {
    J messages = J::array();
    if (req.contains("instructions") && !req["instructions"].is_null())
        messages.push_back({{"role", "system"}, {"content", text_content(req["instructions"])}});
    if (!req.contains("input"))
        throw ApiError(400, "input is required", "invalid_request_error", "input",
                       "missing_required_parameter");
    auto input = req["input"];
    if (input.is_string())
        input = J::array({J{{"role", "user"}, {"content", input}}});
    if (!input.is_array())
        throw ApiError(400, "input must be a string or an array");
    std::string thinking;
    int turn = -1;
    auto new_turn = [&] {
        messages.push_back({{"role", "assistant"}, {"content", ""}});
        turn = static_cast<int>(messages.size()) - 1;
        if (!thinking.empty()) {
            messages[turn]["reasoning_content"] = thinking;
            thinking.clear();
        }
    };
    for (const auto &item : input) {
        if (!item.is_object())
            throw ApiError(400, "input items must be objects");
        const auto kind = item.value("type", item.contains("role") ? "message" : "");
        if (kind == "message") {
            auto role = item.value("role", "");
            if (role != "user" && role != "assistant" && role != "system" && role != "developer")
                throw ApiError(400, "unknown message role");
            if (role == "assistant") {
                new_turn();
                messages[turn]["content"] = text_content(item.value("content", J(nullptr)));
            } else {
                turn = -1;
                thinking.clear();
                messages.push_back({{"role", role == "developer" ? "system" : role},
                                    {"content", text_content(item.value("content", J(nullptr)))}});
            }
        } else if (kind == "reasoning") {
            auto text = text_content(item.value("content", J::array()));
            const auto enc = item.value("encrypted_content", "");
            if (text.empty() && enc.starts_with("strata.r1:")) {
                try {
                    text = parse_text_json(unbase64(enc.substr(10))).at("text").get<std::string>();
                } catch (...) {
                    throw ApiError(400, "damaged reasoning encrypted_content");
                }
            }
            if (!text.empty()) {
                if (turn >= 0 && (messages[turn].contains("tool_calls") || messages[turn]["content"] != ""))
                    turn = -1;
                thinking += text;
            }
        } else if (kind == "function_call" || kind == "custom_tool_call") {
            auto name = item.value("name", "");
            if (name.empty())
                throw ApiError(400, "function_call needs a name");
            if (item.contains("namespace") && !item["namespace"].is_null())
                name = item["namespace"].get<std::string>() + "." + name;
            J args = J::object();
            if (kind == "custom_tool_call")
                args = {{"input", item.value("input", "")}};
            else {
                args = item.value("arguments", J::object());
                if (args.is_string()) {
                    const auto raw = args.get<std::string>();
                    args = J::parse(raw, nullptr, false);
                    if (!args.is_object())
                        args = {{"arguments", raw}};
                }
                if (args.is_null())
                    args = J::object();
            }
            if (turn < 0 || !thinking.empty())
                new_turn();
            if (!messages[turn].contains("tool_calls"))
                messages[turn]["tool_calls"] = J::array();
            messages[turn]["tool_calls"].push_back({{"function", {{"name", name}, {"arguments", args}}},
                                                    {"_call_id", item.value("call_id", J(nullptr))}});
        } else if (kind == "function_call_output" || kind == "custom_tool_call_output") {
            const auto output = item.value("output", J(nullptr));
            messages.push_back({{"role", "tool"},
                                {"content", output.is_object() ? python_json(output) : text_content(output)},
                                {"_call_id", item.value("call_id", J(nullptr))}});
            turn = -1;
            thinking.clear();
        } else if (kind != "additional_tools")
            throw ApiError(400, "unsupported input item: " + kind, "invalid_request_error", "input",
                           "unsupported_parameter");
    }
    for (size_t i = 0; i < messages.size();) {
        if (messages[i]["role"] != "tool") {
            ++i;
            continue;
        }
        auto j = i;
        while (j < messages.size() && messages[j]["role"] == "tool")
            ++j;
        std::map<std::string, size_t> rank;
        if (i)
            for (const auto &call : messages[i - 1].value("tool_calls", J::array()))
                if (call.value("_call_id", J(nullptr)).is_string())
                    rank[call["_call_id"].get<std::string>()] = rank.size();
        std::stable_sort(messages.begin() + i, messages.begin() + j, [&](const J &a, const J &b) {
            const auto ka = a.value("_call_id", J(nullptr)), kb = b.value("_call_id", J(nullptr));
            const auto ra = ka.is_string() && rank.count(ka.get<std::string>()) ? rank[ka.get<std::string>()]
                                                                                : rank.size();
            const auto rb = kb.is_string() && rank.count(kb.get<std::string>()) ? rank[kb.get<std::string>()]
                                                                                : rank.size();
            return ra < rb;
        });
        i = j;
    }
    for (auto &m : messages) {
        m.erase("_call_id");
        if (m.contains("tool_calls"))
            for (auto &c : m["tool_calls"])
                c.erase("_call_id");
    }
    size_t lead = 0;
    while (lead < messages.size() && messages[lead]["role"] == "system")
        ++lead;
    if (lead > 1) {
        std::string s;
        for (size_t i = 0; i < lead; ++i) {
            if (i)
                s += "\n\n";
            s += messages[i]["content"].get<std::string>();
        }
        messages.erase(messages.begin(), messages.begin() + lead);
        messages.insert(messages.begin(), J{{"role", "system"}, {"content", s}});
    }
    return messages;
}
void response_tools(const J &req, ApiRequest &out) {
    auto tools = req.value("tools", J::array());
    if (tools.is_null())
        tools = J::array();
    if (!tools.is_array())
        throw ApiError(400, "tools must be an array");
    if (req.value("input", J(nullptr)).is_array())
        for (const auto &item : req["input"])
            if (item.is_object() && item.value("type", "") == "additional_tools" &&
                item.value("tools", J(nullptr)).is_array())
                for (const auto &t : item["tools"])
                    tools.push_back(t);
    J definitions = J::array();
    std::function<void(const J &, const std::string &, const std::string &)> add =
        [&](const J &t, const std::string &ns, const std::string &description) {
            if (!t.is_object())
                throw ApiError(400, "tools must contain objects");
            const auto kind = t.value("type", "function"), name = t.value("name", "");
            if (kind == "namespace") {
                for (const auto &child : t.at("tools"))
                    add(child, name, t.value("description", ""));
                return;
            }
            if (kind != "function" && kind != "custom")
                return; // same hosted-tool behavior as Python
            if (name.empty())
                throw ApiError(400, "tool needs a name");
            const auto flat = ns.empty() ? name : ns + "." + name;
            auto desc = t.value("description", "");
            if (!description.empty())
                desc = description + (desc.empty() ? "" : "\n\n" + desc);
            J schema = t.value("parameters", J{{"type", "object"}, {"properties", J::object()}});
            if (kind == "custom") {
                schema = {{"type", "object"},
                          {"properties", {{"input", {{"type", "string"}, {"description", "the raw input"}}}}},
                          {"required", J::array({"input"})}};
                const auto fmt = t.value("format", J::object());
                if (fmt.is_object() && fmt.value("type", "") == "grammar" && fmt.contains("definition"))
                    desc += "\n\nThe input must follow this " + fmt.value("syntax", "") + " grammar:\n" +
                            fmt["definition"].get<std::string>();
            }
            definitions.push_back(
                {{"type", "function"},
                 {"function", {{"name", flat}, {"description", desc}, {"parameters", schema}}}});
            out.names[flat] = {
                {"namespace", ns.empty() ? J(nullptr) : J(ns)}, {"name", name}, {"kind", kind}};
            if (!ns.empty())
                out.names[ns + "__" + name] = out.names[flat];
        };
    for (const auto &t : tools)
        add(t, "", "");
    out.original["tools"] = definitions;
}
} // namespace
std::string api_id(const std::string &prefix) {
    static std::atomic<uint64_t> seq{0};
    thread_local std::mt19937_64 random(std::random_device{}());
    std::ostringstream s;
    s << prefix << '_' << std::hex << std::setfill('0') << std::setw(16) << random() << std::setw(16)
      << ++seq;
    return s.str();
}
ApiRequest prepare_api_request(ApiDialect dialect, J req, const J &defaults, const Tokenizer &tok,
                               const ChatTemplate &chat, int context, bool fit, bool think_unasked) {
    if (!req.is_object())
        throw ApiError(400, "request must be a JSON object");
    for (auto it = defaults.begin(); it != defaults.end(); ++it)
        if (!req.contains(it.key()) || req[it.key()].is_null())
            req[it.key()] = it.value();
    if (req.contains("stream") && !req["stream"].is_boolean())
        throw ApiError(400, "stream must be boolean");
    if (req.value("n", J(1)) != 1)
        throw ApiError(400, "only n=1 is supported");
    for (auto key : {"strata_prefix", "strata_tune"})
        if (req.contains(key) && !req[key].is_null())
            throw ApiError(400, std::string("native server does not yet support ") + key,
                           "invalid_request_error", key, "unsupported_parameter");
    if (req.value("strata_mcp", false))
        throw ApiError(400, "native server does not yet support MCP execution");
    ApiRequest out;
    out.dialect = dialect;
    out.original = req;
    out.wire = req;
    out.stream = req.value("stream", false);
    if (req.contains("reasoning_budget_tokens") && !req["reasoning_budget_tokens"].is_null()) {
        const auto &n = req["reasoning_budget_tokens"];
        if (!n.is_number() || n.get<double>() != std::floor(n.get<double>()) ||
            n.get<double>() > 2147483647 || n.get<double>() < -2147483647)
            throw ApiError(400, "reasoning_budget_tokens must be a whole number");
        out.reasoning_budget = std::max(0, n.get<int>());
    }
    std::string max_key = dialect == ApiDialect::responses        ? "max_output_tokens"
                          : req.contains("max_completion_tokens") ? "max_completion_tokens"
                                                                  : "max_tokens";
    if (req.contains(max_key) && !req[max_key].is_null()) {
        if (!req[max_key].is_number_integer())
            throw ApiError(400, max_key + " must be a whole number");
        const auto n = req[max_key].get<int64_t>();
        if (n > 2147483647 || n < -1 || (dialect == ApiDialect::responses && n < 1))
            throw ApiError(400, "invalid " + max_key);
        out.max_new = static_cast<int>(n);
    }
    J format = req.value("response_format", J(nullptr));
    if (dialect == ApiDialect::responses) {
        for (auto key : {"previous_response_id", "conversation", "background", "prompt"})
            if (req.contains(key) && req[key] != false && !req[key].is_null() && req[key] != "")
                throw ApiError(400, std::string("stateless server does not support ") + key,
                               "invalid_request_error", key, "unsupported_parameter");
        out.original["messages"] = responses_messages(req);
        response_tools(req, out);
        auto choice = req.value("tool_choice", J("auto"));
        if (choice.is_object() && choice.value("type", "") == "function") {
            auto name = choice.value("name", "");
            if (choice.contains("namespace"))
                name = choice["namespace"].get<std::string>() + "." + name;
            choice = {{"type", "function"}, {"function", {{"name", name}}}};
        }
        out.original["tool_choice"] = choice;
        if (req.contains("reasoning") && !req["reasoning"].is_null()) {
            if (!req["reasoning"].is_object())
                throw ApiError(400, "reasoning must be an object");
            out.original["reasoning_effort"] = req["reasoning"].value("effort", J(nullptr));
        }
        if (req.contains("text") && !req["text"].is_null()) {
            if (!req["text"].is_object())
                throw ApiError(400, "text must be an object");
            auto f = req["text"].value("format", J::object());
            const auto type = f.value("type", "text");
            if (type == "json_object")
                format = {{"type", "json_object"}};
            else if (type == "json_schema") {
                f.erase("type");
                format = {{"type", "json_schema"}, {"json_schema", f}};
            } else if (type != "text")
                throw ApiError(400, "unsupported text.format");
        }
        const auto includes = req.value("include", J::array());
        out.encrypted =
            std::find(includes.begin(), includes.end(), J("reasoning.encrypted_content")) != includes.end();
    }
    if (dialect == ApiDialect::anthropic)
        out.prompt = anthropic_to_messages(out.original, think_unasked);
    else
        out.prompt = openai_to_messages(out.original);
    if (!images_of(out.prompt.messages).empty())
        throw ApiError(400, "native server is text-only; images require the Python vision service");
    const auto choice = out.original.value("tool_choice", J(nullptr));
    if (choice == "none" || (choice.is_object() && choice.value("type", "") == "none"))
        out.prompt.tools = nullptr;
    out.force = forced_call(choice, out.prompt.tools);
    out.thinking = out.prompt.kwargs.value("enable_thinking", true);
    out.format = StructuredFormat::prepare(format, out.prompt.messages,
                                           dialect != ApiDialect::responses && !out.prompt.tools.is_null());
    const auto rendered = chat.render_prompt(out.prompt, tok.controls());
    out.ids = tok.encode(rendered.text, true, rendered.plain);
    if (!out.force.empty() && !out.thinking) {
        const auto ids = tok.encode(out.force, true);
        out.ids.insert(out.ids.end(), ids.begin(), ids.end());
    }
    out.stops = stop_strings(req);
    const auto room = static_cast<int64_t>(context) - static_cast<int64_t>(out.ids.size()) - 8;
    if (room < 1)
        throw ApiError(400, "prompt leaves no room in the context; requests are never truncated");
    if (out.max_new <= 0)
        out.max_new = static_cast<int>(room);
    if (out.max_new > room) {
        if (!fit)
            throw ApiError(400, "prompt + max_tokens exceeds context; requests are never truncated");
        out.max_new = static_cast<int>(room);
    }
    out.sampling = req;
    sampling_keys(out.sampling);
    return out;
}
ApiOutput::ApiOutput(ApiRequest request, std::string model,
                     std::function<bool(const std::string &, const J &)> emit)
    : request_(std::move(request)), model_(std::move(model)),
      id_(api_id(request_.dialect == ApiDialect::chat        ? "chatcmpl"
                 : request_.dialect == ApiDialect::anthropic ? "msg"
                                                             : "resp")),
      created_(std::chrono::system_clock::to_time_t(std::chrono::system_clock::now())),
      emit_(std::move(emit)) {
    if (request_.dialect == ApiDialect::responses) {
        const auto &req = request_.wire;
        response_ = {{"id", id_},
                     {"object", "response"},
                     {"created_at", created_},
                     {"status", "in_progress"},
                     {"background", false},
                     {"error", nullptr},
                     {"incomplete_details", nullptr},
                     {"instructions", req.value("instructions", J(nullptr))},
                     {"max_output_tokens", req.value("max_output_tokens", J(nullptr))},
                     {"model", model_},
                     {"output", J::array()},
                     {"parallel_tool_calls", req.value("parallel_tool_calls", true)},
                     {"previous_response_id", nullptr},
                     {"reasoning",
                      {{"effort", req.value("reasoning", J::object()).value("effort", J(nullptr))},
                       {"summary", req.value("reasoning", J::object()).value("summary", J(nullptr))}}},
                     {"store", false},
                     {"temperature", req.value("temperature", J(nullptr))},
                     {"text", req.value("text", J{{"format", {{"type", "text"}}}})},
                     {"tool_choice", req.value("tool_choice", J("auto"))},
                     {"tools", req.value("tools", J::array())},
                     {"top_p", req.value("top_p", J(nullptr))},
                     {"truncation", "disabled"},
                     {"usage", nullptr},
                     {"user", nullptr},
                     {"metadata", req.value("metadata", J::object())}};
    }
}
bool ApiOutput::send(const std::string &type, J body) {
    if (request_.dialect == ApiDialect::responses) {
        body["type"] = type;
        body["sequence_number"] = sequence_++;
    } else if (request_.dialect == ApiDialect::anthropic)
        body["type"] = type;
    return emit_(type, body);
}
bool ApiOutput::chat_chunk(const J &delta, const J &finish, const J &usage) {
    J c{{"id", id_},
        {"object", "chat.completion.chunk"},
        {"created", created_},
        {"model", model_},
        {"choices", J::array({J{{"index", 0}, {"delta", delta}, {"finish_reason", finish}}})}};
    if (!usage.is_null())
        c["usage"] = usage;
    return send("", c);
}
bool ApiOutput::start() {
    if (request_.dialect == ApiDialect::chat)
        return chat_chunk({{"role", "assistant"}, {"content", ""}});
    if (request_.dialect == ApiDialect::anthropic) {
        response_ = {{"id", id_},
                     {"type", "message"},
                     {"role", "assistant"},
                     {"model", model_},
                     {"content", J::array()},
                     {"stop_reason", nullptr},
                     {"stop_sequence", nullptr},
                     {"usage", {{"input_tokens", request_.ids.size()}, {"output_tokens", 0}}}};
        return send("message_start", {{"message", response_}});
    }
    return send("response.created", {{"response", response_}}) &&
           send("response.in_progress", {{"response", response_}});
}
bool ApiOutput::response_open(J item) {
    response_["output"].push_back(item);
    output_ = static_cast<int>(response_["output"].size()) - 1;
    if (!send("response.output_item.added", {{"output_index", output_}, {"item", item}}))
        return false;
    if (item["type"] == "message") {
        J part{{"type", "output_text"}, {"text", ""}, {"annotations", J::array()}, {"logprobs", J::array()}};
        response_["output"][output_]["content"].push_back(part);
        return send(
            "response.content_part.added",
            {{"item_id", item["id"]}, {"output_index", output_}, {"content_index", 0}, {"part", part}});
    }
    return true;
}
bool ApiOutput::response_close(const std::string &status) {
    if (output_ < 0)
        return true;
    auto &item = response_["output"][output_];
    item["status"] = status;
    bool ok = true;
    const auto type = item["type"].get<std::string>();
    if (type == "message") {
        const auto part = item["content"][0];
        ok = send("response.output_text.done", {{"item_id", item["id"]},
                                                {"output_index", output_},
                                                {"content_index", 0},
                                                {"text", part["text"]},
                                                {"logprobs", J::array()}}) &&
             send("response.content_part.done",
                  {{"item_id", item["id"]}, {"output_index", output_}, {"content_index", 0}, {"part", part}});
    } else if (type == "reasoning") {
        const auto text = item["content"][0]["text"].get<std::string>();
        ok = send("response.reasoning_text.done",
                  {{"item_id", item["id"]}, {"output_index", output_}, {"content_index", 0}, {"text", text}});
        if (request_.encrypted)
            item["encrypted_content"] = "strata.r1:" + base64(python_json(J{{"text", text}}));
    } else if (type == "function_call")
        ok = send("response.function_call_arguments.done", {{"item_id", item["id"]},
                                                            {"output_index", output_},
                                                            {"name", item["name"]},
                                                            {"arguments", item["arguments"]}});
    else
        ok = send("response.custom_tool_call_input.done",
                  {{"item_id", item["id"]}, {"output_index", output_}, {"input", item["input"]}});
    ok = send("response.output_item.done", {{"output_index", output_}, {"item", item}}) && ok;
    output_ = -1;
    return ok;
}
bool ApiOutput::anthropic_close() {
    if (block_ < 0 || open_kind_.empty())
        return true;
    open_kind_.clear();
    return send("content_block_stop", {{"index", block_}});
}
bool ApiOutput::event(const OutputEvent &ev) {
    const bool tool = ev.kind == "tool_call" || ev.kind == "tool_start" || ev.kind == "tool_args";
    if (!tool && ev.text.empty())
        return true;
    if (ev.kind == "content")
        content_ += ev.text;
    else if (ev.kind == "reasoning")
        reasoning_ += ev.text;
    if (tool && !ev.call)
        return true;
    int index = -1;
    if (tool) {
        const auto found = call_index_.find(ev.call->id);
        if (found == call_index_.end()) {
            index = static_cast<int>(calls_.size());
            call_index_[ev.call->id] = index;
            calls_.push_back({{"id", ev.call->id},
                              {"type", "function"},
                              {"function", {{"name", ev.call->name}, {"arguments", ""}}}});
        } else
            index = found->second;
        if (ev.kind == "tool_args")
            calls_[index]["function"]["arguments"] =
                calls_[index]["function"]["arguments"].get<std::string>() + ev.text;
        if (ev.kind == "tool_call") {
            completed_calls_.insert(ev.call->id);
            if (!calls_[index].value("announced", false))
                calls_[index]["function"]["arguments"] = python_json(ev.call->arguments);
        }
    }
    if (request_.dialect == ApiDialect::chat) {
        if (ev.kind == "content" || ev.kind == "reasoning")
            return chat_chunk({{ev.kind == "content" ? "content" : "reasoning_content", ev.text}});
        if (ev.kind == "tool_call" && !calls_[index].value("announced", false))
            return chat_chunk({{"tool_calls", J::array({J{{"index", index},
                                                          {"id", ev.call->id},
                                                          {"type", "function"},
                                                          {"function", calls_[index]["function"]}}})}});
        if (ev.kind == "tool_start") {
            calls_[index]["announced"] = true;
            return chat_chunk(
                {{"tool_calls", J::array({J{{"index", index},
                                            {"id", ev.call->id},
                                            {"type", "function"},
                                            {"function", {{"name", ev.call->name}, {"arguments", ""}}}}})}});
        }
        if (ev.kind == "tool_args")
            return chat_chunk(
                {{"tool_calls", J::array({J{{"index", index}, {"function", {{"arguments", ev.text}}}}})}});
        return true;
    }
    if (request_.dialect == ApiDialect::anthropic) {
        if (ev.kind == "tool_call" && calls_[index].value("announced", false)) {
            if (block_ >= 0 && blocks_[block_].value("id", "") == ev.call->id)
                blocks_[block_]["input"] = ev.call->arguments;
            return true;
        }
        if (ev.kind == "tool_args") {
            blocks_[block_]["_json"] = blocks_[block_].value("_json", "") + ev.text;
            return send(
                "content_block_delta",
                {{"index", block_}, {"delta", {{"type", "input_json_delta"}, {"partial_json", ev.text}}}});
        }
        const auto want = ev.kind == "reasoning" ? "thinking" : ev.kind == "content" ? "text" : "tool_use";
        if (open_kind_ != want || tool) {
            if (!anthropic_close())
                return false;
            ++block_;
            open_kind_ = want;
            J b = want == std::string("thinking")
                      ? J{{"type", "thinking"}, {"thinking", ""}, {"signature", ""}}
                  : want == std::string("text") ? J{{"type", "text"}, {"text", ""}}
                                                : J{{"type", "tool_use"},
                                                    {"id", ev.call->id},
                                                    {"name", ev.call->name},
                                                    {"input", J::object()}};
            blocks_.push_back(b);
            if (!send("content_block_start", {{"index", block_}, {"content_block", b}}))
                return false;
        }
        if (!tool) {
            const auto key = ev.kind == "content" ? "text" : "thinking";
            blocks_[block_][key] = blocks_[block_][key].get<std::string>() + ev.text;
            return send(
                "content_block_delta",
                {{"index", block_},
                 {"delta",
                  {{"type", ev.kind == "content" ? "text_delta" : "thinking_delta"}, {key, ev.text}}}});
        }
        if (ev.kind == "tool_start") {
            calls_[index]["announced"] = true;
            return true;
        }
        blocks_[block_]["input"] = ev.call->arguments;
        return send(
            "content_block_delta",
            {{"index", block_},
             {"delta", {{"type", "input_json_delta"}, {"partial_json", python_json(ev.call->arguments)}}}});
    }
    const auto type = ev.kind == "reasoning" ? "reasoning"
                      : ev.kind == "content" ? "message"
                                             : "function_call";
    if (!tool) {
        if (output_ < 0 || response_["output"][output_]["type"] != type) {
            if (!response_close())
                return false;
            const auto item = ev.kind == "reasoning"
                                  ? J{{"id", api_id("rs")},
                                      {"type", "reasoning"},
                                      {"status", "in_progress"},
                                      {"summary", J::array()},
                                      {"content", J::array({J{{"type", "reasoning_text"}, {"text", ""}}})}}
                                  : J{{"id", api_id("msg")},
                                      {"type", "message"},
                                      {"status", "in_progress"},
                                      {"role", "assistant"},
                                      {"content", J::array()}};
            if (!response_open(item))
                return false;
        }
        auto &item = response_["output"][output_];
        item["content"][0]["text"] = item["content"][0]["text"].get<std::string>() + ev.text;
        J body{{"item_id", item["id"]}, {"output_index", output_}, {"content_index", 0}, {"delta", ev.text}};
        if (ev.kind == "content")
            body["logprobs"] = J::array();
        return send(ev.kind == "content" ? "response.output_text.delta" : "response.reasoning_text.delta",
                    body);
    }
    if (ev.kind == "tool_args") {
        if (output_ < 0)
            return true;
        auto &item = response_["output"][output_];
        if (item["type"] != "function_call")
            return true;
        item["arguments"] = item["arguments"].get<std::string>() + ev.text;
        return send("response.function_call_arguments.delta",
                    {{"item_id", item["id"]}, {"output_index", output_}, {"delta", ev.text}});
    }
    if (output_ < 0 || response_["output"][output_].value("call_id", "") != ev.call->id) {
        if (!response_close())
            return false;
        auto info = request_.names.value(
            ev.call->name, J{{"namespace", nullptr}, {"name", ev.call->name}, {"kind", "function"}});
        const auto custom = info["kind"] == "custom";
        J item{{"id", api_id(custom ? "ctc" : "fc")},
               {"type", custom ? "custom_tool_call" : "function_call"},
               {"status", "in_progress"},
               {"call_id", ev.call->id},
               {"name", info["name"]},
               {custom ? "input" : "arguments", ""}};
        if (!info["namespace"].is_null())
            item["namespace"] = info["namespace"];
        if (!response_open(item))
            return false;
        if (ev.kind == "tool_call" && !custom) {
            response_["output"][output_]["arguments"] = python_json(ev.call->arguments);
            if (!send("response.function_call_arguments.delta", {{"item_id", item["id"]},
                                                                 {"output_index", output_},
                                                                 {"delta", python_json(ev.call->arguments)}}))
                return false;
        }
    }
    if (ev.kind == "tool_call") {
        auto &item = response_["output"][output_];
        if (item["type"] == "custom_tool_call") {
            auto value = ev.call->arguments.value("input", J(nullptr));
            item["input"] = value.is_string() ? value : J(python_json(ev.call->arguments));
            if (!send("response.custom_tool_call_input.delta",
                      {{"item_id", item["id"]}, {"output_index", output_}, {"delta", item["input"]}}))
                return false;
        }
        return response_close();
    }
    return true;
}
bool ApiOutput::ping(const J &progress) {
    if (request_.dialect == ApiDialect::responses)
        return send("response.in_progress", {{"response", response_}});
    if (request_.dialect == ApiDialect::anthropic)
        return send("ping", J::object());
    if (request_.original.value("return_progress", false) && !progress.is_null()) {
        J c{{"id", id_},
            {"object", "chat.completion.chunk"},
            {"created", created_},
            {"model", model_},
            {"choices", J::array({J{{"index", 0}, {"delta", J::object()}, {"finish_reason", nullptr}}})},
            {"prompt_progress", progress}};
        return send("", c);
    }
    return emit_("", nullptr);
}
J ApiOutput::finish(const J &done, const std::optional<std::string> &stop) {
    const auto pt = done.value("prompt_tokens", static_cast<int>(request_.ids.size())),
               n = done.value("completion_tokens", done.value("generated", 0));
    const auto cached = std::clamp(done.value("reused", 0), 0, std::max(0, pt - 1));
    const auto reason = stop ? "stop" : done.value("finish", "stop");
    const auto all_calls = !calls_.empty() && completed_calls_.size() == calls_.size();
    J clean = J::array();
    for (auto c : calls_) {
        c.erase("announced");
        if (completed_calls_.count(c["id"].get<std::string>()) ||
            !J::parse(c["function"]["arguments"].get<std::string>(), nullptr, false).is_discarded())
            clean.push_back(c);
    }
    J usage{{"prompt_tokens", pt},
            {"completion_tokens", n},
            {"total_tokens", pt + n},
            {"prompt_tokens_details", {{"cached_tokens", cached}}}};
    J timings = nullptr;
    if (done.contains("prompt_ms")) {
        const auto pn = std::max(0, pt - cached), decoded = done.value("generated", n);
        const auto pp = done.value("prompt_ms", 0.), ms = done.value("decode_ms", 0.);
        const auto round = [](double v, int digits) {
            const auto scale = std::pow(10., digits);
            return std::round(v * scale) / scale;
        };
        timings = {
            {"cache_n", cached},
            {"prompt_n", pn},
            {"prompt_ms", round(pp, 1)},
            {"prompt_per_token_ms", pn ? J(round(pp / pn, 3)) : J(nullptr)},
            {"prompt_per_second", pn && pp > 0 ? J(round(1000. * pn / pp, 1)) : J(nullptr)},
            {"predicted_n", n},
            {"predicted_ms", round(ms, 1)},
            {"predicted_per_token_ms", decoded ? J(round(ms / decoded, 3)) : J(nullptr)},
            {"predicted_per_second", decoded && ms > 0 ? J(round(1000. * decoded / ms, 1)) : J(nullptr)}};
        if (done.contains("drafts_offered")) {
            timings["draft_n"] = done["drafts_offered"];
            timings["draft_n_accepted"] = done["drafts_accepted"];
        }
    }
    if (request_.dialect == ApiDialect::chat) {
        const auto finish = all_calls && reason == "stop" ? "tool_calls"
                            : reason == "cancel"          ? "stop"
                                                          : reason;
        J chunk{{"id", id_},
                {"object", "chat.completion.chunk"},
                {"created", created_},
                {"model", model_},
                {"choices", J::array({J{{"index", 0}, {"delta", J::object()}, {"finish_reason", finish}}})},
                {"usage", usage}};
        if (!timings.is_null())
            chunk["timings"] = timings;
        send("", chunk);
        J msg{{"role", "assistant"}, {"content", content_.empty() ? J(nullptr) : J(content_)}};
        if (!reasoning_.empty())
            msg["reasoning_content"] = reasoning_;
        if (!clean.empty())
            msg["tool_calls"] = clean;
        J result{{"id", id_},
                 {"object", "chat.completion"},
                 {"created", created_},
                 {"model", model_},
                 {"choices", J::array({J{{"index", 0}, {"message", msg}, {"finish_reason", finish}}})},
                 {"usage", usage}};
        if (!timings.is_null())
            result["timings"] = timings;
        return result;
    }
    if (request_.dialect == ApiDialect::anthropic) {
        anthropic_close();
        const auto finish = stop                            ? "stop_sequence"
                            : all_calls && reason == "stop" ? "tool_use"
                            : reason == "length"            ? "max_tokens"
                                                            : "end_turn";
        J u{{"input_tokens", pt - cached}, {"cache_read_input_tokens", cached}, {"output_tokens", n}};
        send("message_delta",
             {{"delta", {{"stop_reason", finish}, {"stop_sequence", stop ? J(*stop) : J(nullptr)}}},
              {"usage", u}});
        send("message_stop", J::object());
        J blocks = J::array();
        for (auto b : blocks_) {
            if (b.contains("_json")) {
                auto input = J::parse(b["_json"].get<std::string>(), nullptr, false);
                if (input.is_discarded())
                    continue;
                b["input"] = input;
                b.erase("_json");
            } else if (b["type"] == "tool_use" && !completed_calls_.count(b["id"].get<std::string>()))
                continue;
            blocks.push_back(b);
        }
        response_["content"] = blocks;
        response_["stop_reason"] = finish;
        response_["stop_sequence"] = stop ? J(*stop) : J(nullptr);
        response_["usage"] = u;
        return response_;
    }
    const bool unfinished = output_ >= 0 && (response_["output"][output_]["type"] == "function_call" ||
                                             response_["output"][output_]["type"] == "custom_tool_call");
    response_close(reason == "length" || unfinished ? "incomplete" : "completed");
    response_["status"] = reason == "length" || unfinished ? "incomplete" : "completed";
    response_["completed_at"] = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    if (response_["status"] == "incomplete")
        response_["incomplete_details"] = {{"reason", "max_output_tokens"}};
    response_["usage"] = {
        {"input_tokens", pt},
        {"input_tokens_details", {{"cached_tokens", cached}}},
        {"output_tokens", n},
        {"output_tokens_details", {{"reasoning_tokens", done.value("reasoning_tokens", 0)}}},
        {"total_tokens", pt + n}};
    send(response_["status"] == "completed" ? "response.completed" : "response.incomplete",
         {{"response", response_}});
    return response_;
}
void ApiOutput::failed(const ApiError &error) {
    if (request_.dialect == ApiDialect::responses) {
        response_close("incomplete");
        response_["status"] = "failed";
        response_["error"] = {{"code", error.code.empty() ? "server_error" : error.code},
                              {"message", error.what()}};
        send("response.failed", {{"response", response_}});
    } else
        send(request_.dialect == ApiDialect::anthropic ? "error" : "",
             error.body(request_.dialect == ApiDialect::anthropic));
}
} // namespace strata::runtime
