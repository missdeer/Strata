#include "build.hpp"
#include <fstream>
#include <deque>
#include <stdexcept>
#include <cstdint>

namespace strata::build {
namespace {
// Inspect imports without loading untrusted images or executing ldd/objdump.
class Image {
public:
    std::vector<unsigned char> data;
    explicit Image(const fs::path& path) {
        std::ifstream input(path, std::ios::binary);
        if (!input) throw std::runtime_error("cannot read binary: " + path_utf8(path));
        const auto size = fs::file_size(path);
        if (size > (1ull << 30)) throw std::runtime_error("binary exceeds 1 GiB import inspection limit");
        data.resize(static_cast<size_t>(size));
        if (!input.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size)))
            throw std::runtime_error("truncated binary: " + path_utf8(path));
    }
    void range(uint64_t offset, uint64_t size) const {
        if (offset > data.size() || size > data.size() - offset) throw std::runtime_error("invalid binary import table bounds");
    }
    uint64_t number(uint64_t offset, unsigned size) const {
        range(offset, size);
        uint64_t value = 0;
        for (unsigned i = 0; i < size; ++i) value |= uint64_t(data[static_cast<size_t>(offset + i)]) << (8 * i);
        return value;
    }
    std::string string(uint64_t offset, uint64_t limit) const {
        range(offset, limit);
        std::string result;
        for (uint64_t i = 0; i < limit && i < 4096; ++i) {
            auto c = data[static_cast<size_t>(offset + i)];
            if (!c) return result;
            result += static_cast<char>(c);
        }
        throw std::runtime_error("unterminated import name");
    }
};
bool valid_name(const std::string& name) {
    return !name.empty() && name != "." && name != ".." && name.find_first_of("/\\:\r\n") == std::string::npos;
}
std::vector<std::string> pe_imports(const Image& image) {
    const auto pe = image.number(0x3c, 4);
    if (image.number(pe, 4) != 0x00004550 || image.number(pe + 4, 2) != 0x8664)
        throw std::runtime_error("package requires an x64 PE image");
    const auto count = image.number(pe + 6, 2), optional_size = image.number(pe + 20, 2), opt = pe + 24;
    if (image.number(opt, 2) != 0x20b || optional_size < 112) throw std::runtime_error("invalid PE64 optional header");
    image.range(opt, optional_size);
    const auto directories = image.number(opt + 108, 4);
    if (directories > (optional_size - 112) / 8) throw std::runtime_error("invalid PE directory count");
    const auto sections = opt + optional_size;
    image.range(sections, count * 40);
    auto offset = [&](uint64_t rva, uint64_t bytes) -> uint64_t {
        for (uint64_t i = 0; i < count; ++i) {
            const auto s = sections + i * 40, va = image.number(s + 12, 4), raw_size = image.number(s + 16, 4);
            if (rva >= va && rva - va < raw_size && bytes <= raw_size - (rva - va)) {
                const auto result = image.number(s + 20, 4) + rva - va;
                image.range(result, bytes); return result;
            }
        }
        throw std::runtime_error("PE import RVA is outside file-backed sections");
    };
    std::vector<std::string> names;
    for (unsigned directory : {1u, 13u}) {
        if (directory >= directories) continue;
        auto rva = image.number(opt + 112 + directory * 8, 4);
        const auto size = image.number(opt + 116 + directory * 8, 4);
        if (!rva) continue;
        const unsigned stride = directory == 1 ? 20 : 32;
        bool terminated = false;
        for (uint64_t consumed = 0; consumed + stride <= size; consumed += stride) {
            auto entry = offset(rva + consumed, stride);
            auto name = image.number(entry + (directory == 1 ? 12 : 4), 4);
            if (!name) { terminated = true; break; }
            if (directory == 13 && !(image.number(entry, 4) & 1)) {
                const auto base = image.number(opt + 24, 8);
                if (name < base) throw std::runtime_error("invalid PE delay import VA");
                name -= base;
            }
            const auto start = offset(name, 1);
            // Resolve each byte's RVA too: a name must not spill into another section.
            std::string text;
            for (uint64_t n = 0; n < 4096; ++n) {
                auto c = image.number(offset(name + n, 1), 1);
                if (!c) break;
                text += static_cast<char>(c);
                if (n == 4095) throw std::runtime_error("unterminated PE import name");
            }
            (void) start;
            if (!valid_name(text)) throw std::runtime_error("invalid PE import name");
            names.push_back(text);
        }
        if (!terminated) throw std::runtime_error("unterminated PE import table");
    }
    return names;
}
std::vector<std::string> elf_imports(const Image& image) {
    if (image.number(4, 1) != 2 || image.number(5, 1) != 1 || image.number(18, 2) != 62)
        throw std::runtime_error("package requires a little-endian x86_64 ELF image");
    const auto ph = image.number(32, 8), stride = image.number(54, 2), count = image.number(56, 2);
    if (stride < 56 || count == 0xffff) throw std::runtime_error("unsupported ELF program headers");
    image.range(ph, stride * count);
    auto offset = [&](uint64_t address, uint64_t bytes) -> uint64_t {
        for (uint64_t i = 0; i < count; ++i) {
            auto p = ph + stride * i;
            const auto va = image.number(p + 16, 8), size = image.number(p + 32, 8);
            if (image.number(p, 4) == 1 && address >= va && address - va < size && bytes <= size - (address - va)) {
                auto result = image.number(p + 8, 8) + address - va;
                image.range(result, bytes); return result;
            }
        }
        throw std::runtime_error("ELF dynamic address outside file-backed segments");
    };
    uint64_t strings = 0, string_size = 0;
    std::vector<uint64_t> needed;
    for (uint64_t i = 0; i < count; ++i) {
        auto p = ph + stride * i;
        if (image.number(p, 4) != 2) continue;
        const auto start = image.number(p + 8, 8), size = image.number(p + 32, 8);
        image.range(start, size);
        bool terminated = false;
        for (uint64_t n = 0; n + 16 <= size; n += 16) {
            auto tag = image.number(start + n, 8), value = image.number(start + n + 8, 8);
            if (!tag) { terminated = true; break; }
            if (tag == 1) needed.push_back(value);
            if (tag == 5) strings = value;
            if (tag == 10) string_size = value;
        }
        if (!terminated) throw std::runtime_error("unterminated ELF dynamic table");
    }
    std::vector<std::string> names;
    if (needed.empty()) return names;
    if (!strings || !string_size) throw std::runtime_error("missing ELF dynamic string table");
    const auto start = offset(strings, string_size);
    for (auto n : needed) {
        if (n >= string_size) throw std::runtime_error("ELF import name outside string table");
        auto name = image.string(start + n, string_size - n);
        if (!valid_name(name)) throw std::runtime_error("invalid ELF import name");
        names.push_back(name);
    }
    return names;
}
bool system_library(const std::string& name) {
#ifdef _WIN32
    static const std::set<std::string> names{
        "kernel32.dll", "kernelbase.dll", "ntdll.dll", "advapi32.dll", "ws2_32.dll", "user32.dll", "gdi32.dll",
        "rpcrt4.dll", "ole32.dll", "oleaut32.dll", "shell32.dll", "shlwapi.dll", "secur32.dll", "crypt32.dll",
        "bcrypt.dll", "bcryptprimitives.dll", "version.dll", "winmm.dll", "psapi.dll", "setupapi.dll", "cfgmgr32.dll",
        "shcore.dll", "imm32.dll", "combase.dll", "dbghelp.dll", "dbgcore.dll", "nvcuda.dll", "ucrtbase.dll",
        "winhttp.dll", "windowscodecs.dll", "pdh.dll"};
    return names.count(name) || name.starts_with("api-ms-win-") || name.starts_with("ext-ms-win-");
#else
    static const std::set<std::string> names{"libc.so.6", "libm.so.6", "libpthread.so.0", "libdl.so.2", "librt.so.1",
        "libresolv.so.2", "libutil.so.1", "ld-linux-x86-64.so.2", "libcuda.so.1", "libnvidia-ml.so.1"};
    return names.count(name);
#endif
}
std::string key(const std::string& name) {
#ifdef _WIN32
    return lower(name);
#else
    return name;
#endif
}
} // namespace
std::vector<std::string> imports(const fs::path& path) {
    Image image(path);
    if (image.data.size() >= 4 && image.number(0, 2) == 0x5a4d) return pe_imports(image);
    if (image.data.size() >= 4 && image.number(0, 4) == 0x464c457f) return elf_imports(image);
    throw std::runtime_error("not a PE/ELF executable: " + path_utf8(path));
}
void copy_runtime(const fs::path& destination, const std::vector<fs::path>& roots, const std::vector<fs::path>& directories) {
    std::map<std::string, fs::path> candidates;
    for (const auto& dir : directories) {
        std::error_code error;
        fs::directory_iterator it(dir, error);
        if (error) continue;
        for (const auto& entry : it) {
            auto name = path_utf8(entry.path().filename());
            if ((lower(name).ends_with(".dll") || name.find(".so") != std::string::npos) && entry.is_regular_file())
                candidates.try_emplace(key(name), entry.path());
        }
    }
    std::deque<fs::path> todo(roots.begin(), roots.end());
    std::set<fs::path> seen;
    while (!todo.empty()) {
        auto image = todo.front(); todo.pop_front();
        if (!seen.insert(resolved(image)).second) continue;
        for (const auto& name : imports(image)) {
            if (system_library(key(name))) continue;
            auto out = destination / utf8_path(name);
            if (!fs::is_regular_file(out)) {
                const auto found = candidates.find(key(name));
                if (found == candidates.end()) throw std::runtime_error("missing runtime library " + name + ", imported by " + path_utf8(image.filename()) + "; add --runtime-dir DIR");
                fs::copy_file(found->second, out);
            }
            todo.push_back(out);
        }
    }
}
} // namespace strata::build
