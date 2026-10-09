#include "strata/runtime/expert_store.hpp"
#include "strata/runtime/path.hpp"
#include "strata/kernels/cpu/native_expert.hpp"
#include "strata/kernels/cpu/expert.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace strata::runtime {
namespace cpu = strata::kernels::cpu;
class MappedFile {
public:
    explicit MappedFile(const std::filesystem::path& path) {
#ifdef _WIN32
        HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot open expert file: " + path_utf8(path));
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || uint64_t(size.QuadPart) > SIZE_MAX) {
            CloseHandle(file); throw std::runtime_error("invalid expert file size");
        }
        size_ = static_cast<size_t>(size.QuadPart);
        HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (mapping) data_ = static_cast<const uint8_t*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0));
        if (mapping) CloseHandle(mapping);
        CloseHandle(file);
        if (!data_) throw std::runtime_error("cannot map expert file");
#else
        int fd = open(path.c_str(), O_RDONLY);
        if (fd < 0) throw std::runtime_error("cannot open expert file: " + path.string());
        struct stat status{};
        if (fstat(fd, &status) || status.st_size <= 0 || uint64_t(status.st_size) > SIZE_MAX) {
            close(fd); throw std::runtime_error("invalid expert file size");
        }
        size_ = static_cast<size_t>(status.st_size);
        void* p = mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd, 0);
        close(fd);
        if (p == MAP_FAILED) throw std::runtime_error("cannot map expert file");
        data_ = static_cast<const uint8_t*>(p);
#endif
    }
    ~MappedFile() {
#ifdef _WIN32
        if (data_) UnmapViewOfFile(data_);
#else
        if (data_) munmap(const_cast<uint8_t*>(data_), size_);
#endif
    }
    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;
    const uint8_t* at(uint64_t offset, uint64_t bytes) const {
        if (offset > size_ || bytes > size_ - offset) throw std::runtime_error("expert tensor outside file");
        return data_ + static_cast<size_t>(offset);
    }
    size_t size() const { return size_; }
private:
    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
};

namespace {
// Only the GGUF directory is needed here. Bounds-check skips without allocating
// tokenizer arrays; workers never load dense tensors or model/session state.
class DirectoryReader {
public:
    explicit DirectoryReader(const MappedFile& file) : file_(file) {}
    uint64_t integer(size_t bytes) {
        const auto* p = file_.at(pos_, bytes);
        uint64_t value = 0;
        for (size_t i = 0; i < bytes; ++i) value |= uint64_t(p[i]) << (8 * i);
        pos_ += bytes;
        return value;
    }
    std::string string() {
        const auto bytes = integer(8);
        if (bytes > (1u << 20)) throw std::runtime_error("GGUF directory string exceeds 1 MiB");
        const auto* p = file_.at(pos_, bytes);
        pos_ += bytes;
        return std::string(reinterpret_cast<const char*>(p), static_cast<size_t>(bytes));
    }
    uint64_t skip_value(uint64_t type, int depth = 0) {
        static constexpr size_t sizes[] = {1, 1, 2, 2, 4, 4, 4, 1, 0, 0, 8, 8, 8};
        if (type >= 13 || depth > 2) throw std::runtime_error("invalid GGUF metadata type/nesting");
        if (type == 8) {
            const auto n = integer(8); skip(n); return 0;
        }
        if (type == 9) {
            const auto elem = integer(4), count = integer(8);
            if (count > (1u << 24)) throw std::runtime_error("implausible GGUF array length");
            for (uint64_t i = 0; i < count; ++i) skip_value(elem, depth + 1);
            return 0;
        }
        return integer(sizes[type]);
    }
    uint64_t position() const { return pos_; }
private:
    void skip(uint64_t bytes) { file_.at(pos_, bytes); pos_ += bytes; }
    const MappedFile& file_;
    uint64_t pos_ = 0;
};

struct Tensor { uint64_t offset = 0, type = 0; std::vector<uint64_t> shape; };
struct Directory { uint64_t start = 0; std::map<std::string, Tensor> tensors; };
Directory directory(const MappedFile& file) {
    DirectoryReader r(file);
    if (r.integer(4) != 0x46554747 || r.integer(4) != 3) throw std::runtime_error("worker requires GGUF v3");
    const auto tensors = r.integer(8), metadata = r.integer(8);
    if (tensors > (1u << 20) || metadata > (1u << 20)) throw std::runtime_error("implausible GGUF directory size");
    uint64_t alignment = 32;
    std::set<std::string> keys;
    for (uint64_t i = 0; i < metadata; ++i) {
        const auto name = r.string();
        if (!keys.insert(name).second) throw std::runtime_error("duplicate GGUF metadata key");
        const auto type = r.integer(4), value = r.skip_value(type);
        if (name == "general.alignment") {
            if (type != 4 || value == 0 || value > (1u << 20) || (value & (value - 1)))
                throw std::runtime_error("invalid GGUF alignment");
            alignment = value;
        }
    }
    Directory result;
    for (uint64_t i = 0; i < tensors; ++i) {
        auto name = r.string();
        const auto dims = r.integer(4);
        if (!dims || dims > 4) throw std::runtime_error("invalid GGUF tensor dimensions");
        Tensor tensor;
        for (uint64_t d = 0; d < dims; ++d) tensor.shape.push_back(r.integer(8));
        tensor.type = r.integer(4); tensor.offset = r.integer(8);
        if (!result.tensors.emplace(std::move(name), std::move(tensor)).second) throw std::runtime_error("duplicate GGUF tensor name");
    }
    result.start = (r.position() + alignment - 1) / alignment * alignment;
    file.at(result.start, 0);
    return result;
}
uint64_t number(const std::string& text) {
    if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos) throw std::runtime_error("invalid native expert integer");
    return std::stoull(text);
}
std::pair<uint64_t, uint64_t> available_memory() {
#ifdef _WIN32
    MEMORYSTATUSEX status{}; status.dwLength = sizeof(status);
    if (!GlobalMemoryStatusEx(&status)) throw std::runtime_error("cannot query available RAM for expert preload");
    return {status.ullAvailPhys, status.ullAvailPageFile};
#else
    std::ifstream in("/proc/meminfo");
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream row(line); std::string key; uint64_t kib = 0;
        if (row >> key >> kib && key == "MemAvailable:") return {kib * 1024, UINT64_MAX};
    }
    throw std::runtime_error("cannot query available RAM for expert preload");
#endif
}
} // namespace

ExpertStore::ExpertStore(const std::filesystem::path& pack, const std::filesystem::path& native, uint64_t budget) : budget_(budget) {
    std::ifstream in(pack / "native_experts.txt");
    if (!in) throw std::runtime_error("cannot read native_experts.txt");
    std::string line;
    uint64_t offset = 0;
    bool version = false;
    while (std::getline(in, line)) {
        const auto tag = line.find("strata native experts v");
        if (tag != std::string::npos) {
            const auto at = tag + std::strlen("strata native experts v");
            const auto end = line.find_first_not_of("0123456789", at);
            const auto v = number(line.substr(at, end - at));
            if (v < 1 || v > 4) throw std::runtime_error("unsupported native_experts.txt version");
            version = true;
        }
        const auto first = line.find_first_not_of(" \r\t");
        if (first == std::string::npos || line[first] == '#') continue;
        std::istringstream row(line); std::vector<std::string> fields;
        for (std::string field; row >> field;) fields.push_back(std::move(field));
        if (fields.size() != 5 && fields.size() != 8 && fields.size() != 9) throw std::runtime_error("invalid native expert row");
        if (layers_.size() >= 128 || number(fields[0]) != layers_.size() || number(fields[3]) != offset)
            throw std::runtime_error("expert table is not contiguous");
        const auto gu = number(fields[1]), down = number(fields[2]);
        if (gu > INT32_MAX || down > INT32_MAX) throw std::runtime_error("invalid native expert format");
        cpu::NativeFmt fmt; std::string error;
        if (!cpu::native_fmt(static_cast<int>(gu), static_cast<int>(down), cpu::H, cpu::FF, fmt, error)) throw std::runtime_error(error);
        ExpertLayer layer;
        layer.gate_up = static_cast<int>(gu); layer.down = static_cast<int>(down);
        layer.offset = offset; layer.bytes = number(fields[4]);
        layer.gate_bytes = fmt.up_off; layer.down_bytes = fmt.bytes - fmt.down_off;
        if (layer.bytes != fmt.bytes) throw std::runtime_error("expert blob size differs from format geometry");
        if (fields.size() >= 8) {
            std::vector<std::string> names(3);
            if (fields.size() == 9) {
                std::istringstream list(fields[8]); std::vector<std::string> split;
                for (std::string name; std::getline(list, name, ',');) split.push_back(std::move(name));
                if (split.size() == 1) names.assign(3, split.front());
                else if (split.size() == 3 && fields[8].back() != ',') names = split;
                else throw std::runtime_error("invalid per-role shard column");
            }
            for (size_t r = 0; r < 3; ++r) {
                layer.role_offset[r] = number(fields[5 + r]);
                const auto name = utf8_path(names[r]);
                if (!names[r].empty() && (name.filename() != name || names[r] == "." || names[r] == ".." ||
                    names[r].find_first_of("/\\:") != std::string::npos)) throw std::runtime_error("shard column must be a filename");
                layer.role_file[r] = names[r].empty() ? native : native.parent_path() / name;
            }
        }
        offset += layer.bytes * 512;
        layers_.push_back(std::move(layer));
    }
    if (!version || layers_.empty()) throw std::runtime_error("worker requires native expert layout v1..v4 with 1..128 layers");
    const auto bin = pack / "experts.bin";
    if (std::filesystem::exists(bin)) {
        auto map = std::make_unique<MappedFile>(bin);
        if (map->size() != offset) throw std::runtime_error("experts.bin has the wrong size");
        packed_ = bin; maps_.emplace(bin, std::move(map));
    } else {
        if (native.empty()) throw std::runtime_error("--native is required when experts.bin is absent");
        std::map<std::filesystem::path, Directory> headers;
        for (size_t l = 0; l < layers_.size(); ++l) for (size_t r = 0; r < 3; ++r) {
            const auto& layer = layers_[l]; const auto& path = layer.role_file[r];
            if (path.empty()) throw std::runtime_error("layout lacks GGUF offsets");
            if (!maps_.count(path)) {
                auto map = std::make_unique<MappedFile>(path);
                headers.emplace(path, directory(*map)); maps_.emplace(path, std::move(map));
            }
            const auto& header = headers.at(path);
            const auto name = "blk." + std::to_string(l) + ".ffn_" + std::array<const char*, 3>{"gate", "up", "down"}[r] + "_exps.weight";
            auto found = header.tensors.find(name);
            const auto type = r < 2 ? layer.gate_up : layer.down;
            const auto shape = r < 2 ? std::vector<uint64_t>{cpu::H, cpu::FF, 512} : std::vector<uint64_t>{cpu::FF, cpu::H, 512};
            if (found == header.tensors.end() || found->second.type != static_cast<uint64_t>(type) || found->second.shape != shape ||
                found->second.offset > maps_.at(path)->size() - header.start ||
                header.start + found->second.offset != layer.role_offset[r]) throw std::runtime_error(name + ": layout does not match GGUF");
            maps_.at(path)->at(layer.role_offset[r], (r < 2 ? layer.gate_bytes : layer.down_bytes) * 512);
        }
    }
}

ExpertStore::~ExpertStore() = default;
ExpertStore::Buffer ExpertStore::read(int layer, int expert) {
    const auto& f = layers_.at(static_cast<size_t>(layer));
    if (expert < 0 || expert >= 512) throw std::runtime_error("invalid expert id");
    auto result = std::make_shared<std::vector<uint8_t>>(static_cast<size_t>(f.bytes));
    if (!packed_.empty()) {
        std::memcpy(result->data(), maps_.at(packed_)->at(f.offset + f.bytes * expert, f.bytes), static_cast<size_t>(f.bytes));
    } else {
        size_t dst = 0;
        for (size_t r = 0; r < 3; ++r) {
            const auto bytes = r < 2 ? f.gate_bytes : f.down_bytes;
            std::memcpy(result->data() + dst, maps_.at(f.role_file[r])->at(f.role_offset[r] + expert * bytes, bytes), static_cast<size_t>(bytes));
            dst += static_cast<size_t>(bytes);
        }
    }
    return result;
}

ExpertStore::Buffer ExpertStore::get(int layer, int expert) {
    if (layer < 0 || static_cast<size_t>(layer) >= layers_.size() || expert < 0 || expert >= 512)
        throw std::runtime_error("invalid expert cache entry");
    const int key = layer * 512 + expert;
    auto it = cache_.find(key);
    if (it != cache_.end()) {
        lru_.splice(lru_.end(), lru_, it->second.position);
        return it->second.buffer;
    }
    const auto bytes = layers_.at(static_cast<size_t>(layer)).bytes;
    if (bytes > budget_) throw std::runtime_error("RAM cache is smaller than one expert");
    while (cached_ + bytes > budget_) {
        const auto oldest = lru_.front();
        cached_ -= cache_.at(oldest).buffer->size(); cache_.erase(oldest); lru_.pop_front();
    }
    auto buffer = read(layer, expert);
    lru_.push_back(key);
    try { cache_.emplace(key, Cached{buffer, std::prev(lru_.end())}); }
    catch (...) { lru_.pop_back(); throw; }
    cached_ += bytes;
    return buffer;
}

uint64_t ExpertStore::fingerprint() {
    uint64_t value = 14695981039346656037ull;
    auto add = [&](const uint8_t* p, size_t n) { for (size_t i = 0; i < n; ++i) { value ^= p[i]; value *= 1099511628211ull; } };
    auto integer = [&](uint64_t v, size_t n) { for (size_t i = 0; i < n; ++i) { const uint8_t b = static_cast<uint8_t>(v >> (8 * i)); add(&b, 1); } };
    integer(layers_.size(), 4); integer(512, 4); integer(cpu::H, 4); integer(cpu::FF, 4);
    for (size_t l = 0; l < layers_.size(); ++l) {
        const auto& f = layers_[l]; integer(f.gate_up, 4); integer(f.down, 4); integer(f.bytes, 8);
        for (int e : {0, 511}) { auto b = read(static_cast<int>(l), e); add(b->data(), 4096); add(b->data() + b->size() - 4096, 4096); }
    }
    return value;
}

void ExpertStore::preload(int index, int count, const std::string& assignment, const std::function<bool()>& cancelled) {
    if (count < 1 || count > 16 || index < 0 || index >= count || (!assignment.empty() && assignment.size() != 512))
        throw std::runtime_error("invalid preload worker assignment");
    std::vector<int> owned;
    for (int e = 0; e < 512; ++e) {
        const auto owner = assignment.empty() ? static_cast<size_t>(e % count) : std::string("0123456789abcdef").find(assignment[e]);
        if (owner >= static_cast<size_t>(count)) throw std::runtime_error("invalid preload expert owner table");
        if (owner == static_cast<size_t>(index)) owned.push_back(e);
    }
    if (owned.empty()) throw std::runtime_error("preload assignment contains no experts");
    uint64_t total = 0, resident = 0;
    for (size_t l = 0; l < layers_.size(); ++l) for (int e : owned) {
        total += layers_[l].bytes;
        if (cache_.count(static_cast<int>(l) * 512 + e)) resident += layers_[l].bytes;
    }
    if (total > budget_) throw std::runtime_error("expert preload exceeds RAM cache budget");
    const auto memory = available_memory();
    const auto needed = total - resident;
    if (needed && (memory.first < needed + (8ull << 30) || memory.second < needed + (8ull << 30)))
        throw std::runtime_error("expert preload needs more available RAM/commit plus 8 GiB headroom");
    const auto start = std::chrono::steady_clock::now();
    std::cout << "preload: reading " << owned.size() * layers_.size() << " experts (" << double(total) / (1ull << 30) << " GiB) before listening" << std::endl;
    try {
        for (size_t l = 0; l < layers_.size(); ++l) {
            for (int e : owned) {
                if (cancelled && cancelled()) throw std::runtime_error("worker stopping");
                get(static_cast<int>(l), e);
            }
            std::cout << "preload: layer " << l + 1 << '/' << layers_.size() << " cache=" << cached_ << std::endl;
        }
        for (size_t l = 0; l < layers_.size(); ++l) for (int e : owned)
            if (!cache_.count(static_cast<int>(l) * 512 + e)) throw std::runtime_error("preload failed to retain its complete assignment");
    } catch (...) { cache_.clear(); lru_.clear(); cached_ = 0; throw; }
    std::cout << "preload complete: bytes=" << total << " seconds=" << std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() << std::endl;
}
} // namespace strata::runtime
