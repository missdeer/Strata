#include "strata/runtime/server_metrics.hpp"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <set>
#include <charconv>
#include <cstdlib>
namespace strata::runtime {
namespace {
using J = TextJson;
const std::array<double, 22> buckets{.001, .005, .01, .02, .04, .06, .08, .1, .25, .5,  .75,
                                     1,    2.5,  5,   7.5, 10,  20,  40,  80, 160, 640, 2560};
bool number(const J &x) { return x.is_number() && std::isfinite(x.get<double>()); }
double num(const J &x, double fallback = 0) { return number(x) ? x.get<double>() : fallback; }
double round(double x, int places) {
    char buffer[512];
    const auto result =
        std::to_chars(buffer, buffer + sizeof(buffer) - 1, x, std::chars_format::fixed, places);
    if (result.ec != std::errc())
        return x;
    *result.ptr = '\0';
    return std::strtod(buffer, nullptr);
}
std::string value(const J &x) {
    if (x.is_number_integer())
        return x.dump();
    std::ostringstream out;
    out << std::setprecision(15) << num(x);
    return out.str();
}
std::string escape(const std::string &s) {
    std::string out;
    for (char c : s) {
        if (c == '\\' || c == '"')
            out += '\\';
        if (c == '\n')
            out += "\\n";
        else
            out += c;
    }
    return out;
}
} // namespace
void NativeLatencies::observe(const J &first, int output, double decode, double wall) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto add = [&](size_t index, double v, uint64_t times) {
        if (!std::isfinite(v) || v < 0 || !times)
            return;
        auto &h = values_[index];
        for (size_t i = 0; i < buckets.size(); ++i)
            if (v <= buckets[i])
                h.counts[i] += times;
        h.sum += v * times;
        h.n += times;
    };
    if (number(first))
        add(0, first.get<double>(), 1);
    if (output > 1 && decode > 0)
        add(1, decode / (output - 1), static_cast<uint64_t>(output - 1));
    add(2, wall, 1);
}
J NativeLatencies::snapshot() {
    std::lock_guard<std::mutex> lock(mutex_);
    J out = J::object();
    for (size_t i = 0; i < 3; ++i)
        out[std::array<const char *, 3>{"ttft", "itl", "e2e"}[i]] =
            J::array({values_[i].counts, values_[i].sum, values_[i].n});
    return out;
}
std::string prometheus_metrics(const J &m, const J &lat) {
    const auto engine = m.value("engine", J::object()), live = m.value("live", J::object()),
               totals = m.value("totals", J::object()), hardware = m.value("hardware", J::object());
    const auto label = "model_name=\"" + escape(engine.value("model", "strata")) + "\"";
    std::ostringstream out;
    auto metric = [&](const std::string &name, const std::string &kind, const std::string &help, const J &v) {
        out << "# HELP " << name << ' ' << help << "\n# TYPE " << name << ' ' << kind << '\n'
            << name << '{' << label << "} " << value(v) << '\n';
    };
    const auto state = live.value("state", "");
    const bool busy = state == "reading" || state == "generating";
    const auto ctx = num(engine.value("max_context", J(nullptr))),
               used = busy ? num(live.value("prompt_tokens", J(nullptr))) +
                                 num(live.value("generated", J(nullptr)))
                           : 0;
    metric("vllm:num_requests_running", "gauge", "Requests reading their prompt or generating.",
           live.value("running", J(busy ? 1 : 0)));
    metric("vllm:num_requests_waiting", "gauge", "Requests waiting for their turn.",
           num(live.value("queued", J(nullptr))) + num(live.value("waiting", J(nullptr))));
    metric("vllm:kv_cache_usage_perc", "gauge", "The running request's share of the context (1 = full).",
           ctx ? round(std::min(1., used / ctx), 4) : 0);
    for (const auto &[name, key, help] : std::vector<std::tuple<std::string, std::string, std::string>>{
             {"vllm:prompt_tokens_total", "prompt_tokens", "Prompt tokens of the finished requests."},
             {"vllm:generation_tokens_total", "output_tokens", "Generated tokens."},
             {"vllm:request_success_total", "requests", "Finished requests."},
             {"vllm:prefix_cache_queries_total", "prompt_tokens",
              "Prompt tokens looked up in the prompt cache."},
             {"vllm:prefix_cache_hits_total", "reused", "Prompt tokens the prompt cache already held."},
             {"vllm:spec_decode_num_draft_tokens_total", "drafts_offered", "MTP draft tokens offered."},
             {"vllm:spec_decode_num_accepted_tokens_total", "drafts_accepted", "MTP draft tokens accepted."}})
        metric(name, "counter", help, totals.value(key, J(0)));
    metric("vllm:num_preemptions_total", "counter", "Preempted requests (Strata does not preempt).", 0);
    for (const auto &[name, key, help] : std::vector<std::tuple<std::string, std::string, std::string>>{
             {"vllm:time_to_first_token_seconds", "ttft", "Time to the first token."},
             {"vllm:inter_token_latency_seconds", "itl", "Time between two tokens."},
             {"vllm:e2e_request_latency_seconds", "e2e", "A request from start to end."}}) {
        const auto h = lat.at(key);
        out << "# HELP " << name << ' ' << help << "\n# TYPE " << name << " histogram\n";
        for (size_t i = 0; i < buckets.size(); ++i) {
            std::string bucket = value(buckets[i]);
            if (buckets[i] >= 1 && std::floor(buckets[i]) == buckets[i])
                bucket += ".0";
            out << name << "_bucket{" << label << ",le=\"" << bucket << "\"} " << h[0][i].dump() << '\n';
        }
        out << name << "_bucket{" << label << ",le=\"+Inf\"} " << h[2].dump() << '\n'
            << name << "_sum{" << label << "} " << value(round(num(h[1]), 6)) << '\n'
            << name << "_count{" << label << "} " << h[2].dump() << '\n';
    }
    std::set<std::string> typed;
    auto strata = [&](const std::string &name, const std::string &kind, const std::string &help, const J &v,
                      const std::string &labels = std::string()) {
        if (!number(v))
            return;
        if (typed.insert(name).second)
            out << "# HELP strata:" << name << ' ' << help << "\n# TYPE strata:" << name << ' ' << kind
                << '\n';
        out << "strata:" << name << '{' << label << labels << "} " << value(v) << '\n';
    };
    for (auto s : {"unloaded", "idle", "reading", "generating"})
        strata("live_state", "gauge", "1 for what the engine is doing (live.state).", state == s ? 1 : 0,
               ",state=\"" + std::string(s) + "\"");
    strata("live_tok_s", "gauge", "Decode rate over the last seconds (live.tok_s).",
           num(live.value("tok_s", J(nullptr))));
    strata("live_prefill_tok_s_mean", "gauge",
           "Prompt reading rate of the running request (live.prefill_tok_s_mean).",
           num(live.value("prefill_tok_s_mean", J(nullptr))));
    strata("live_prompt_read", "gauge",
           "Prompt tokens read so far by the running request (live.prompt_read).",
           num(live.value("prompt_read", J(nullptr))));
    const auto slots = live.value("slots", J::array());
    if (slots.is_array() && !slots.empty())
        for (auto s : {"idle", "reading", "generating"}) {
            int n = 0;
            for (const auto &slot : slots)
                if (slot.is_object() && slot.value("state", "") == s)
                    ++n;
            strata("live_slots", "gauge", "The batch slots in each state (live.slots).", n,
                   ",state=\"" + std::string(s) + "\"");
        }
    strata("engine_max_context", "gauge", "The engine's context (engine.max_context).",
           engine.value("max_context", J(nullptr)));
    strata("totals_prompt_seconds_total", "counter", "Time spent reading prompts (totals.prompt_ms).",
           round(num(totals.value("prompt_ms", J(nullptr))) / 1000., 3));
    strata("totals_decode_seconds_total", "counter", "Time spent generating (totals.decode_ms).",
           round(num(totals.value("decode_ms", J(nullptr))) / 1000., 3));
    const auto reqs = m.value("requests", J::array());
    const auto last = reqs.empty() ? J::object() : reqs[0];
    strata("last_hit_rate", "gauge", "The last request's expert cache hit rate (requests[0].hit_rate).",
           last.value("hit_rate", J(nullptr)));
    strata("last_decode_tok_s", "gauge", "The last request's decode rate (requests[0].decode_tok_s).",
           last.value("decode_tok_s", J(nullptr)));
    auto gpus = hardware.value("gpus", J::array());
    if (gpus.empty() && number(hardware.value("gpu_util", J(nullptr))))
        gpus = J::array({J{{"index", 0},
                           {"util", hardware.value("gpu_util", J(nullptr))},
                           {"mem_used", hardware.value("gpu_mem_used", J(nullptr))},
                           {"mem_total", hardware.value("gpu_mem_total", J(nullptr))},
                           {"temp", hardware.value("gpu_temp", J(nullptr))},
                           {"power", hardware.value("gpu_power", J(nullptr))}}});
    for (const auto &[name, key, help] : std::vector<std::tuple<std::string, std::string, std::string>>{
             {"gpu_util", "util", "GPU busy, percent (hardware.gpu_util)."},
             {"gpu_mem_used_bytes", "mem_used", "GPU memory in use (hardware.gpu_mem_used)."},
             {"gpu_mem_total_bytes", "mem_total", "GPU memory (hardware.gpu_mem_total)."},
             {"gpu_temp_celsius", "temp", "GPU temperature (hardware.gpu_temp)."},
             {"gpu_power_watts", "power", "GPU power draw (hardware.gpu_power)."}})
        for (const auto &g : gpus)
            strata(name, "gauge", help, g.value(key, J(nullptr)),
                   ",gpu=\"" + g.value("index", J(0)).dump() + "\"");
    strata("cpu", "gauge", "CPU busy, percent (hardware.cpu).", hardware.value("cpu", J(nullptr)));
    strata("ram_used_bytes", "gauge", "RAM in use (hardware.ram_used).",
           hardware.value("ram_used", J(nullptr)));
    strata("ram_total_bytes", "gauge", "RAM (hardware.ram_total).", hardware.value("ram_total", J(nullptr)));
    return out.str();
}
J conversation_cache(const J &engine, const J &requests, const J &totals, const J &parked) {
    const auto last = requests.empty() ? J::object() : requests[0];
    int reused = 0;
    for (const auto &r : requests)
        if (r.value("reused", 0) > 0)
            ++reused;
    J result{{"enabled", num(engine.value("conversation_cache_mib", J(nullptr))) > 0},
             {"slots", engine.value("conversation_cache_slots", J(nullptr))},
             {"budget_mib", engine.value("conversation_cache_mib", J(nullptr))},
             {"requests", requests.size()},
             {"requests_reused", reused},
             {"prompt_tokens", totals.value("prompt_tokens", 0)},
             {"reused_tokens", totals.value("reused", 0)},
             {"last_prompt", last.value("prompt_tokens", J(nullptr))},
             {"last_reused", last.value("reused", J(nullptr))}};
    result.update(parked);
    return result;
}
} // namespace strata::runtime
