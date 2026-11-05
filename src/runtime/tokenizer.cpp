#include "strata/runtime/text.hpp"
#include "unicode.h"

#include <algorithm>
#include <fstream>
#include <limits>
#include <queue>
#include <set>
#include <stdexcept>
#include <tuple>

namespace strata::runtime {
std::string read_text(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read " + path.string());
    const auto length = std::filesystem::file_size(path);
    if (length > 256 * 1024 * 1024) throw std::runtime_error("text file exceeds 256 MiB");
    std::string out(static_cast<size_t>(length), '\0');
    if (!out.empty() && !in.read(out.data(), static_cast<std::streamsize>(out.size())))
        throw std::runtime_error("short text read");
    return out;
}
TextJson parse_text_json(const std::string& text) {
    std::vector<std::set<std::string>> keys;
    return TextJson::parse(text, [&keys](int, TextJson::parse_event_t event, TextJson& v) {
        if (event == TextJson::parse_event_t::object_start) keys.emplace_back();
        if (event == TextJson::parse_event_t::key && !keys.back().insert(v.get<std::string>()).second)
            throw std::invalid_argument("duplicate JSON key: " + v.get<std::string>());
        if (event == TextJson::parse_event_t::object_end) keys.pop_back();
        return true;
    });
}
TextJson read_text_json(const std::filesystem::path& path) { return parse_text_json(read_text(path)); }
std::string python_json(const TextJson& value, bool ascii, bool compact) {
    const auto raw = value.dump(-1, ' ', ascii);
    if (compact) return raw;
    std::string out;
    bool quoted = false, escaped = false;
    for (char c : raw) {
        out += c;
        if (quoted) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') quoted = false;
        } else if (c == '"') quoted = true;
        else if (c == ',' || c == ':') out += ' ';
    }
    return out;
}
std::string trim_text(std::string_view s) {
    auto cps = unicode_cpts_from_utf8(std::string(s));
    size_t a = 0, b = cps.size();
    auto space = [](uint32_t c) { return unicode_cpt_flags_from_cpt(c).is_whitespace || (c >= 0x1c && c <= 0x1f); };
    while (a < b && space(cps[a])) ++a;
    while (b > a && space(cps[b-1])) --b;
    std::string out;
    for (; a < b; ++a) out += unicode_cpt_to_utf8(cps[a]);
    return out;
}

std::string Utf8Decoder::push(std::string_view bytes, bool final) {
    std::string s = std::move(held_); held_.clear(); s.append(bytes);
    std::string out;
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) { out += s[i++]; continue; }
        size_t n = c >= 0xc2 && c <= 0xdf ? 2 : c >= 0xe0 && c <= 0xef ? 3 : c >= 0xf0 && c <= 0xf4 ? 4 : 0;
        if (!n) { out += "\xef\xbf\xbd"; ++i; continue; }
        // CPython defers the surrogate-range check until a third byte arrives.
        // Match that streaming timing as well as the final replacement text.
        if (!final && c == 0xed && i+2 == s.size() &&
            static_cast<unsigned char>(s[i+1]) >= 0xa0 && static_cast<unsigned char>(s[i+1]) <= 0xbf) {
            held_ = s.substr(i); break;
        }
        size_t k = 1;
        for (; k < n && i+k < s.size(); ++k) {
            const auto b = static_cast<unsigned char>(s[i+k]);
            if (b < 0x80 || b > 0xbf || (k == 1 &&
                ((c == 0xe0 && b < 0xa0) || (c == 0xed && b >= 0xa0) ||
                 (c == 0xf0 && b < 0x90) || (c == 0xf4 && b >= 0x90)))) break;
        }
        if (k == n) { out.append(s, i, n); i += n; }
        else if (i+k == s.size() && !final) { held_ = s.substr(i); break; }
        else { out += "\xef\xbf\xbd"; i += k; }
    }
    return out;
}

static constexpr const char* qwen35 =
    R"((?:'[sS]|'[tT]|'[rR][eE]|'[vV][eE]|'[mM]|'[lL][lL]|'[dD])|[^\r\n\p{L}\p{N}]?[\p{L}\p{M}]+|\p{N}| ?[^\s\p{L}\p{M}\p{N}]+[\r\n]*|\s*[\r\n]+|\s+(?!\S)|\s+)";
static std::string pair_key(const std::string& a, const std::string& b) { return a + " " + b; }
Tokenizer::Tokenizer(std::vector<std::string> tokens, const std::vector<std::string>& merges,
                     std::vector<int> types, TextJson specials)
    : tokens_(std::move(tokens)), types_(std::move(types)), special_ids(std::move(specials)) {
    if (tokens_.size() > static_cast<size_t>(std::numeric_limits<int>::max()) || tokens_.empty())
        throw std::invalid_argument("invalid vocabulary size");
    if (!types_.empty() && types_.size() != tokens_.size()) throw std::invalid_argument("token_type size mismatch");
    for (size_t i = 0; i < tokens_.size(); ++i) {
        if (tokens_[i].empty() || !ids_.emplace(tokens_[i], static_cast<int>(i)).second)
            throw std::invalid_argument("empty or duplicate vocabulary token");
        if (!types_.empty() && (types_[i] == 3 || types_[i] == 4)) specials_.emplace_back(tokens_[i], static_cast<int>(i));
    }
    for (size_t i = 0; i < merges.size(); ++i) {
        const auto p = merges[i].find(' ');
        if (p == std::string::npos || merges[i].find(' ', p+1) != std::string::npos ||
            !ids_.contains(merges[i].substr(0,p)) || !ids_.contains(merges[i].substr(p+1)) ||
            !ids_.contains(merges[i].substr(0,p) + merges[i].substr(p+1)))
            throw std::invalid_argument("invalid merge rule " + std::to_string(i));
        // Python's dict keeps the last rank for repeated pairs.
        ranks_[merges[i]] = static_cast<int>(i);
    }
    std::stable_sort(specials_.begin(), specials_.end(), [](const auto& a, const auto& b) { return a.first.size() > b.first.size(); });
    for (int b = 0; b < 256; ++b) if (!ids_.contains(unicode_byte_to_utf8(static_cast<uint8_t>(b))))
        throw std::invalid_argument("vocabulary lacks byte alphabet");
}
Tokenizer Tokenizer::load(const std::filesystem::path& directory) {
    const auto cfg = read_text_json(directory / "tokenizer.json");
    if (cfg.value("model", "") != "gpt2" || cfg.value("pre", "qwen35") != "qwen35" ||
        (cfg.contains("pre_pattern") && cfg["pre_pattern"] != qwen35))
        throw std::invalid_argument("unsupported tokenizer model, pre or pattern");
    const auto vocab = read_text_json(directory / "vocab.json");
    if (!vocab.is_object()) throw std::invalid_argument("vocab must be an object");
    std::vector<std::string> tokens(vocab.size());
    for (const auto& item : vocab.items()) {
        if (!item.value().is_number_integer()) throw std::invalid_argument("token id must be integer");
        auto id = item.value().get<int64_t>();
        if (id < 0 || static_cast<uint64_t>(id) >= tokens.size() || !tokens[static_cast<size_t>(id)].empty())
            throw std::invalid_argument("vocab ids must be contiguous and unique");
        tokens[static_cast<size_t>(id)] = item.key();
    }
    std::vector<std::string> merges;
    const auto raw = read_text(directory / "merges.txt");
    for (size_t p = 0; p < raw.size();) {
        auto e = raw.find('\n', p); if (e == std::string::npos) e = raw.size();
        auto line = raw.substr(p, e-p); if (line.ends_with('\r')) line.pop_back();
        if (line.empty()) throw std::invalid_argument("empty merge rule");
        merges.push_back(std::move(line)); p = e+1;
    }
    auto types = read_text_json(directory / "token_type.json");
    auto out = Tokenizer(std::move(tokens), merges, types.is_null() ? std::vector<int>{} : types.get<std::vector<int>>(),
                        cfg.value("special_ids", TextJson::object()));
    if (cfg.contains("vocab_size") && cfg["vocab_size"] != out.size()) throw std::invalid_argument("vocab_size mismatch");
    if (cfg.contains("n_merges") && cfg["n_merges"] != out.ranks_.size()) throw std::invalid_argument("n_merges mismatch");
    for (const auto& id : out.special_ids) if (!id.is_number_integer() || id.get<int64_t>() < 0 ||
        static_cast<uint64_t>(id.get<int64_t>()) >= out.size()) throw std::invalid_argument("special id out of range");
    return out;
}
std::vector<int> Tokenizer::bpe(const std::string& mapped) const {
    struct Node { std::string text; int next, prev; size_t version = 0; };
    std::vector<Node> nodes;
    for (auto c : unicode_cpts_from_utf8(mapped)) {
        const int i = static_cast<int>(nodes.size());
        nodes.push_back({unicode_cpt_to_utf8(c), i+1, i-1});
    }
    if (nodes.empty()) return {};
    nodes.back().next = -1;
    using Edge = std::tuple<int, int, size_t, size_t>;
    std::priority_queue<Edge, std::vector<Edge>, std::greater<Edge>> heap;
    auto add = [&](int i) {
        if (i < 0 || nodes[i].next < 0) return;
        const auto j = nodes[i].next;
        auto it = ranks_.find(pair_key(nodes[i].text, nodes[j].text));
        if (it != ranks_.end()) heap.emplace(it->second, i, nodes[i].version, nodes[j].version);
    };
    for (int i = 0; i+1 < static_cast<int>(nodes.size()); ++i) add(i);
    while (!heap.empty()) {
        auto [rank, i, a, b] = heap.top(); heap.pop(); (void)rank;
        const int j = nodes[i].next;
        if (j < 0 || nodes[i].version != a || nodes[j].version != b) continue;
        nodes[i].text += nodes[j].text; ++nodes[i].version;
        nodes[i].next = nodes[j].next; ++nodes[j].version;
        if (nodes[i].next >= 0) nodes[nodes[i].next].prev = i;
        add(i); add(nodes[i].prev);
    }
    std::vector<int> out;
    for (int i = 0; i >= 0; i = nodes[i].next) out.push_back(ids_.at(nodes[i].text));
    return out;
}
std::vector<int> Tokenizer::encode(const std::string& text, bool parse_special, const PlainSpans& plain) const {
    // Validate UTF-8 rather than letting the Unicode helper replace invalid input.
    Utf8Decoder check;
    if (check.push(text, true) != text) throw std::invalid_argument("prompt is not valid UTF-8");
    for (auto [a,b] : plain) if (a > b || b > text.size()) throw std::invalid_argument("plain span out of range");
    std::vector<int> out;
    auto encode_plain = [&](size_t a, size_t b) {
        if (a == b) return;
        for (const auto& piece : unicode_regex_split(text.substr(a,b-a), {qwen35})) {
            auto ids = this->bpe(piece); out.insert(out.end(), ids.begin(), ids.end());
        }
    };
    size_t start = 0, pos = 0;
    while (pos < text.size()) {
        const bool protected_span = std::any_of(plain.begin(), plain.end(), [pos](auto s) { return s.first <= pos && pos < s.second; });
        const std::pair<std::string, int>* match = nullptr;
        if (!protected_span) for (const auto& special : specials_) {
            if (!parse_special && types_[special.second] != 4) continue;
            if (text.compare(pos, special.first.size(), special.first) == 0) { match = &special; break; }
        }
        if (match) { encode_plain(start,pos); out.push_back(match->second); pos += match->first.size(); start = pos; }
        else ++pos;
    }
    encode_plain(start,text.size()); return out;
}
std::string Tokenizer::token_bytes(int id) const {
    if (id < 0 || static_cast<size_t>(id) >= tokens_.size()) throw std::out_of_range("token id outside vocabulary");
    std::string out;
    for (auto c : unicode_cpts_from_utf8(tokens_[id])) out += static_cast<char>(unicode_utf8_to_byte(unicode_cpt_to_utf8(c)));
    return out;
}
std::string Tokenizer::decode(const std::vector<int>& ids) const {
    Utf8Decoder decoder; std::string out;
    for (int id : ids) out += decoder.push(token_bytes(id));
    return out + decoder.push({}, true);
}
std::vector<std::string> Tokenizer::controls() const {
    std::vector<std::string> out;
    for (size_t i = 0; i < types_.size(); ++i) if (types_[i] == 3) out.push_back(tokens_[i]);
    return out;
}
std::vector<std::string> stop_strings(const TextJson& req) {
    TextJson stops = TextJson::array();
    if (req.contains("stop") && !req["stop"].is_null()) {
        stops = req["stop"].is_string() ? TextJson::array({req["stop"]}) : req["stop"];
        if (!stops.is_array() || stops.size() > 4) throw std::invalid_argument("stop takes at most 4 strings");
    } else if (req.contains("stop_sequences") && !req["stop_sequences"].is_null()) stops = req["stop_sequences"];
    if (!stops.is_array()) throw std::invalid_argument("stop_sequences must be a list of strings");
    std::vector<std::string> out;
    for (const auto& s : stops) {
        if (!s.is_string()) throw std::invalid_argument("stop must contain strings");
        if (!s.get_ref<const std::string&>().empty()) out.push_back(s.get<std::string>());
    }
    return out;
}
StopMatcher::StopMatcher(std::vector<std::string> stops) : stops_(std::move(stops)) {
    std::erase(stops_, "");
}
std::string StopMatcher::push(const std::string& text) {
    if (hit) return {};
    const auto buf = held_ + text;
    size_t first = std::string::npos;
    for (const auto& s : stops_) {
        auto i = buf.find(s);
        if (i < first) { first = i; hit = s; }
    }
    if (hit) { held_.clear(); return buf.substr(0, first); }
    size_t keep = 0;
    for (const auto& s : stops_) for (size_t k = std::min(s.size()-1, buf.size()); k > keep; --k)
        if (buf.ends_with(s.substr(0,k))) { keep = k; break; }
    held_ = buf.substr(buf.size()-keep); return buf.substr(0,buf.size()-keep);
}
std::string StopMatcher::flush() { auto out = std::move(held_); held_.clear(); return out; }
} // namespace strata::runtime
