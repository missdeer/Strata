#pragma once

#include <nlohmann/json.hpp>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace strata::runtime {
// Tool/property order is part of the prompt, unlike canonical cluster JSON.
using TextJson = nlohmann::ordered_json;
using PlainSpans = std::vector<std::pair<size_t, size_t>>; // UTF-8 byte offsets
TextJson parse_text_json(const std::string& text);
TextJson read_text_json(const std::filesystem::path& path);
std::string read_text(const std::filesystem::path& path);
std::string python_json(const TextJson& value, bool ascii = false, bool compact = false);
std::string trim_text(std::string_view text);

class Utf8Decoder {
    std::string held_;
public:
    std::string push(std::string_view bytes, bool final = false);
    bool pending() const { return !held_.empty(); }
};

class Tokenizer {
    std::vector<std::string> tokens_;
    std::vector<int> types_;
    std::unordered_map<std::string, int> ids_;
    std::unordered_map<std::string, int> ranks_;
    std::vector<std::pair<std::string, int>> specials_;
    std::vector<int> bpe(const std::string& mapped) const;
public:
    TextJson special_ids;
    Tokenizer(std::vector<std::string> tokens, const std::vector<std::string>& merges,
              std::vector<int> types = {}, TextJson specials = TextJson::object());
    static Tokenizer load(const std::filesystem::path& directory);
    std::vector<int> encode(const std::string& text, bool parse_special = false,
                            const PlainSpans& plain = {}) const;
    std::string token_bytes(int id) const;
    std::string decode(const std::vector<int>& ids) const;
    std::vector<std::string> controls() const;
    size_t size() const { return tokens_.size(); }
};

std::vector<std::string> stop_strings(const TextJson& request);
class StopMatcher {
    std::vector<std::string> stops_;
    std::string held_;
public:
    std::optional<std::string> hit;
    explicit StopMatcher(std::vector<std::string> stops);
    std::string push(const std::string& text);
    std::string flush();
};

struct TemplateRequest {
    TextJson messages = TextJson::array(), tools = nullptr, kwargs = TextJson::object();
};
TemplateRequest openai_to_messages(const TextJson& request);
TemplateRequest anthropic_to_messages(const TextJson& request, bool think_unasked = true);
TextJson effort_kwargs(const TextJson& value);
std::string forced_call(const TextJson& choice, const TextJson& tools);
std::vector<std::string> images_of(const TextJson& messages);

struct RenderedPrompt { std::string text; PlainSpans plain; };
class ChatTemplate {
    struct Impl;
    std::shared_ptr<Impl> impl_;
public:
    explicit ChatTemplate(const std::filesystem::path& path);
    std::string render(const TemplateRequest& request, bool generation_prompt = true) const;
    RenderedPrompt render_prompt(const TemplateRequest& request,
                                const std::vector<std::string>& controls = {},
                                bool generation_prompt = true) const;
    TextJson capabilities() const;
};

struct ToolCall { std::string name; TextJson arguments = TextJson::object(); std::string id; };
struct OutputEvent { std::string kind, text; std::optional<ToolCall> call; };
ToolCall parse_tool_call(std::string body, const TextJson& schema = nullptr);
class OutputParser {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    OutputParser(bool thinking = true, const TextJson& tools = nullptr,
                 bool stream_tools = false, bool recover = false);
    ~OutputParser();
    OutputParser(OutputParser&&) noexcept;
    OutputParser& operator=(OutputParser&&) noexcept;
    std::vector<OutputEvent> feed(const std::string& delta);
    std::vector<OutputEvent> finish(const std::string& reason = "stop");
    size_t rescued() const;
    size_t refused() const;
};

// Schemas are compiled and checked against their meta-schema before execution;
// reference resolution never reads files or accesses the network.
class StructuredFormat {
    struct Impl;
    std::shared_ptr<Impl> impl_;
    TextJson schema_ = nullptr;
public:
    static StructuredFormat prepare(const TextJson& format, TextJson& messages, bool tools = false);
    bool enabled() const { return !schema_.is_null(); }
    std::string validate(const std::string& text, const std::string& finish) const;
};
} // namespace strata::runtime
