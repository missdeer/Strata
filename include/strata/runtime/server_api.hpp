#pragma once
#include "strata/runtime/server_engine.hpp"
#include <functional>
#include <map>
#include <set>

namespace strata::runtime {
enum class ApiDialect { chat, anthropic, responses };
struct ApiRequest {
    ApiDialect dialect = ApiDialect::chat;
    TextJson original, wire, sampling, names = TextJson::object();
    TemplateRequest prompt;
    StructuredFormat format;
    std::vector<std::string> stops;
    std::vector<int> ids;
    int max_new = 0;
    int reasoning_budget = 0;
    bool thinking = true, stream = false, encrypted = false;
    std::string force;
};
ApiRequest prepare_api_request(ApiDialect dialect, TextJson request, const TextJson &defaults,
                               const Tokenizer &tokenizer, const ChatTemplate &chat, int context, bool fit,
                               bool anthropic_thinking);
std::string api_id(const std::string &prefix);

// A single assembler drives both JSON and SSE, so their data and usage agree.
class ApiOutput {
    ApiRequest request_;
    TextJson response_, calls_ = TextJson::array(), blocks_ = TextJson::array();
    std::string model_, id_, content_, reasoning_, open_kind_;
    int64_t created_;
    int sequence_ = 0, block_ = -1, output_ = -1;
    std::map<std::string, int> call_index_;
    std::set<std::string> completed_calls_;
    std::function<bool(const std::string &, const TextJson &)> emit_;
    bool send(const std::string &type, TextJson body);
    bool chat_chunk(const TextJson &delta, const TextJson &finish = nullptr, const TextJson &usage = nullptr);
    bool response_close(const std::string &status = "completed");
    bool response_open(TextJson item);
    bool anthropic_close();

  public:
    ApiOutput(ApiRequest request, std::string model,
              std::function<bool(const std::string &, const TextJson &)> emit);
    bool start();
    bool event(const OutputEvent &event);
    bool ping(const TextJson &progress = nullptr);
    TextJson finish(const TextJson &done, const std::optional<std::string> &stop);
    void failed(const ApiError &error);
};
} // namespace strata::runtime
