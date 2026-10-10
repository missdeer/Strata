#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <wincodec.h>
#include <objidl.h>
#include <sddl.h>
#else
#include "stb_image.h"
#include <png.h>
#include <unistd.h>
#endif
#include "strata/runtime/server_vision.hpp"
#include "strata/runtime/native_image.hpp"
#include "strata/runtime/server_api.hpp"
#include "strata/runtime/native_http.hpp"
#include "strata/runtime/path.hpp"
#include "strata/runtime/cluster_launch.hpp"
extern "C" {
#include "sha256.h"
}
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <list>
#include <mutex>
#include <sstream>
#include <cmath>
namespace strata::runtime {
namespace {
using J = TextJson;
using Clock = std::chrono::steady_clock;
std::string digest(const std::string &bytes) {
    unsigned char value[32];
    sha256_hash(value, reinterpret_cast<const unsigned char *>(bytes.data()), bytes.size());
    std::ostringstream out;
    for (int i = 0; i < 16; ++i)
        out << std::hex << std::setfill('0') << std::setw(2) << static_cast<int>(value[i]);
    return out.str();
}
std::string b64(const std::string &input) {
    const std::string chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    uint32_t bits = 0;
    int held = 0;
    std::string output;
    for (unsigned char c : input) {
        if (c == '=')
            break;
        const auto at = chars.find(c);
        if (at == std::string::npos) {
            if (std::isspace(c))
                continue;
            throw ApiError(400, "invalid base64 image");
        }
        bits = (bits << 6) | static_cast<uint32_t>(at);
        held += 6;
        if (held >= 8) {
            held -= 8;
            output += static_cast<char>((bits >> held) & 255);
            if (output.size() > (256u << 20))
                throw ApiError(400, "image exceeds 256 MiB");
        }
    }
    return output;
}
int orientation(const std::string &bytes) {
    if (bytes.size() < 4 || static_cast<unsigned char>(bytes[0]) != 255 ||
        static_cast<unsigned char>(bytes[1]) != 216)
        return 1;
    for (size_t at = 2; at + 4 <= bytes.size();) {
        if (static_cast<unsigned char>(bytes[at]) != 255)
            break;
        const auto marker = static_cast<unsigned char>(bytes[at + 1]);
        if (marker == 218 || marker == 217)
            break;
        const auto n =
            (static_cast<unsigned char>(bytes[at + 2]) << 8) | static_cast<unsigned char>(bytes[at + 3]);
        if (n < 2 || at + 2 + n > bytes.size())
            break;
        if (marker == 225 && n >= 16 && bytes.compare(at + 4, 6, std::string("Exif\0\0", 6)) == 0) {
            const size_t base = at + 10, end = at + 2 + n;
            const bool little = bytes.compare(base, 2, "II") == 0;
            auto get = [&](size_t p, int width) -> uint32_t {
                if (p + width > end)
                    return 0;
                uint32_t v = 0;
                for (int i = 0; i < width; ++i)
                    v |= uint32_t(static_cast<unsigned char>(bytes[p + i]))
                         << (8 * (little ? i : width - 1 - i));
                return v;
            };
            const auto dir = base + get(base + 4, 4);
            if (dir + 2 > end)
                return 1;
            const auto count = get(dir, 2);
            for (size_t i = 0; i < count && dir + 2 + (i + 1) * 12 <= end; ++i) {
                const auto p = dir + 2 + i * 12;
                if (get(p, 2) == 274 && get(p + 2, 2) == 3 && get(p + 4, 4) == 1)
                    return static_cast<int>(get(p + 8, 2));
            }
            return 1;
        }
        at += n + 2;
    }
    return 1;
}
#ifdef _WIN32
template <class T> struct Com {
    T *p = nullptr;
    ~Com() {
        if (p)
            p->Release();
    }
    T **put() { return &p; }
    T *operator->() { return p; }
};
void check(HRESULT result) {
    if (FAILED(result))
        throw ApiError(400, "the native image codec could not read this format");
}
#endif
} // namespace
std::string NativeVision::normalize(const std::string &bytes) {
    auto orient = orientation(bytes);
    const bool jpeg = bytes.size() >= 3 && bytes.compare(0, 3, std::string("\xff\xd8\xff", 3)) == 0;
    if ((jpeg && orient <= 1) || bytes.starts_with(std::string("\x89PNG\r\n\x1a\n", 8)) ||
        bytes.starts_with("BM") || bytes.starts_with("GIF87a") || bytes.starts_with("GIF89a"))
        return bytes;
    uint32_t width = 0, height = 0;
    std::vector<unsigned char> rgba;
    const bool extra = decode_extra_image(bytes, width, height, rgba, orient);
#ifdef _WIN32
    const auto initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    struct Apartment {
        bool own;
        ~Apartment() {
            if (own)
                CoUninitialize();
        }
    } apartment{SUCCEEDED(initialized)};
    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE)
        check(initialized);
    Com<IWICImagingFactory> factory;
    check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                           IID_PPV_ARGS(factory.put())));
    if (!extra) {
        Com<IWICStream> stream;
        check(factory->CreateStream(stream.put()));
        if (bytes.size() > DWORD(-1))
            throw ApiError(400, "image too large");
        check(stream->InitializeFromMemory(reinterpret_cast<BYTE *>(const_cast<char *>(bytes.data())),
                                           static_cast<DWORD>(bytes.size())));
        Com<IWICBitmapDecoder> decoder;
        check(
            factory->CreateDecoderFromStream(stream.p, nullptr, WICDecodeMetadataCacheOnLoad, decoder.put()));
        Com<IWICBitmapFrameDecode> frame;
        check(decoder->GetFrame(0, frame.put()));
        check(frame->GetSize(&width, &height));
        if (!jpeg) {
            Com<IWICMetadataQueryReader> metadata;
            if (SUCCEEDED(frame->GetMetadataQueryReader(metadata.put()))) {
                PROPVARIANT tag{};
                if (SUCCEEDED(metadata->GetMetadataByName(L"/ifd/{ushort=274}", &tag)) && tag.vt == VT_UI2 &&
                    tag.uiVal >= 1 && tag.uiVal <= 8)
                    orient = tag.uiVal;
                PropVariantClear(&tag);
            }
        }
        if (!width || !height || uint64_t(width) * height > 100000000)
            throw ApiError(400, "image dimensions exceed limit");
        Com<IWICFormatConverter> converted;
        check(factory->CreateFormatConverter(converted.put()));
        check(converted->Initialize(frame.p, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr,
                                    0, WICBitmapPaletteTypeCustom));
        rgba.resize(size_t(width) * height * 4);
        check(converted->CopyPixels(nullptr, width * 4, static_cast<UINT>(rgba.size()), rgba.data()));
    }
#else
    if (!extra) {
        int w = 0, h = 0, channels = 0;
        auto *decoded = stbi_load_from_memory(reinterpret_cast<const unsigned char *>(bytes.data()),
                                              static_cast<int>(bytes.size()), &w, &h, &channels, 4);
        if (!decoded)
            throw ApiError(400, "the native image codec could not read this format");
        if (w <= 0 || h <= 0 || uint64_t(w) * h > 100000000) {
            stbi_image_free(decoded);
            throw ApiError(400, "image dimensions exceed limit");
        }
        width = static_cast<uint32_t>(w);
        height = static_cast<uint32_t>(h);
        rgba.assign(decoded, decoded + size_t(w) * h * 4);
        stbi_image_free(decoded);
    }
#endif
    const auto out_w = orient >= 5 && orient <= 8 ? height : width,
               out_h = orient >= 5 && orient <= 8 ? width : height;
    std::vector<unsigned char> rgb(size_t(out_w) * out_h * 3);
    for (uint32_t y = 0; y < height; ++y)
        for (uint32_t x = 0; x < width; ++x) {
            uint32_t xx = x, yy = y;
            switch (orient) {
            case 2:
                xx = width - 1 - x;
                break;
            case 3:
                xx = width - 1 - x;
                yy = height - 1 - y;
                break;
            case 4:
                yy = height - 1 - y;
                break;
            case 5:
                xx = y;
                yy = x;
                break;
            case 6:
                xx = height - 1 - y;
                yy = x;
                break;
            case 7:
                xx = height - 1 - y;
                yy = width - 1 - x;
                break;
            case 8:
                xx = y;
                yy = width - 1 - x;
                break;
            }
            const auto from = (size_t(y) * width + x) * 4, to = (size_t(yy) * out_w + xx) * 3;
            const auto alpha = rgba[from + 3];
            for (int c = 0; c < 3; ++c)
                rgb[to + c] =
                    static_cast<unsigned char>((rgba[from + c] * alpha + 255 * (255 - alpha) + 127) / 255);
        }
#ifdef _WIN32
    // WIC's PNG encoder negotiates BGR. Supply that explicit format instead of
    // assuming SetPixelFormat preserves RGB, which swapped red and blue.
    for (size_t at = 0; at < rgb.size(); at += 3)
        std::swap(rgb[at], rgb[at + 2]);
    Com<IStream> destination;
    check(CreateStreamOnHGlobal(nullptr, TRUE, destination.put()));
    Com<IWICBitmapEncoder> encoder;
    check(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.put()));
    check(encoder->Initialize(destination.p, WICBitmapEncoderNoCache));
    Com<IWICBitmapFrameEncode> output;
    Com<IPropertyBag2> properties;
    check(encoder->CreateNewFrame(output.put(), properties.put()));
    check(output->Initialize(properties.p));
    check(output->SetSize(out_w, out_h));
    auto format = GUID_WICPixelFormat24bppBGR;
    check(output->SetPixelFormat(&format));
    if (format != GUID_WICPixelFormat24bppBGR)
        throw ApiError(400, "unexpected PNG codec pixel format");
    check(output->WritePixels(out_h, out_w * 3, static_cast<UINT>(rgb.size()), rgb.data()));
    check(output->Commit());
    check(encoder->Commit());
    STATSTG stat{};
    check(destination->Stat(&stat, STATFLAG_NONAME));
    if (stat.cbSize.QuadPart > (256u << 20))
        throw ApiError(400, "normalized image too large");
    std::string result(static_cast<size_t>(stat.cbSize.QuadPart), '\0');
    LARGE_INTEGER zero{};
    check(destination->Seek(zero, STREAM_SEEK_SET, nullptr));
    ULONG read = 0;
    check(destination->Read(result.data(), static_cast<ULONG>(result.size()), &read));
    if (read != result.size())
        throw ApiError(400, "short normalized image");
    return result;
#else
    png_image image{};
    image.version = PNG_IMAGE_VERSION;
    image.width = out_w;
    image.height = out_h;
    image.format = PNG_FORMAT_RGB;
    png_alloc_size_t n = 0;
    if (!png_image_write_to_memory(&image, nullptr, &n, 0, rgb.data(), 0, nullptr))
        throw ApiError(400, "could not encode normalized image");
    std::string result(n, '\0');
    if (!png_image_write_to_memory(&image, result.data(), &n, 0, rgb.data(), 0, nullptr))
        throw ApiError(400, "could not encode normalized image");
    result.resize(n);
    return result;
#endif
}
std::string NativeVision::load(const std::string &source, const std::function<bool()> &cancelled) {
    if (source.starts_with("data:")) {
        const auto at = source.find(',');
        if (at == std::string::npos)
            throw ApiError(400, "invalid image data URL");
        return b64(source.substr(at + 1));
    }
    if (source.starts_with("http://") || source.starts_with("https://")) {
        try {
            auto result =
                native_http("GET", source, {{"User-Agent", "strata"}}, "", 60, 32u << 20, cancelled);
            if (result.status < 200 || result.status >= 300)
                throw ApiError(400, "image URL returned HTTP " + std::to_string(result.status));
            return result.body;
        } catch (const ApiError &) {
            throw;
        } catch (...) {
            throw ApiError(400, "the image URL could not be read");
        }
    }
    auto name = source.starts_with("file://") ? source.substr(7) : source;
    auto network = name;
    std::replace(network.begin(), network.end(), '/', '\\');
    if (network.starts_with("\\\\") || network.starts_with("\\??\\") || name.find('\0') != std::string::npos)
        throw ApiError(400, "image files must be on this computer; network paths are not read");
    if (source.starts_with("file://") && !name.empty() && name[0] != '/' &&
        !(name.size() > 1 && name[1] == ':'))
        throw ApiError(400, "network file URLs are not read");
#ifdef _WIN32
    if (name.size() > 3 && name[0] == '/' && name[2] == ':')
        name.erase(name.begin());
#endif
    const auto path = utf8_path(name);
    if (!std::filesystem::is_regular_file(path))
        throw ApiError(400, "image must be a data URL, HTTP URL or local file");
    if (std::filesystem::file_size(path) > (256u << 20))
        throw ApiError(400, "image file exceeds limit");
    return read_text(path);
}
struct NativeVision::Impl {
    struct Entry {
        std::filesystem::path path;
        int tokens;
        ~Entry() {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
        }
    };
    J cfg;
    std::filesystem::path root;
    std::unique_ptr<EngineProcess> process;
    std::recursive_mutex mutex;
    std::map<std::string, std::shared_ptr<Entry>> cache;
    std::list<std::string> lru;
    explicit Impl(J config) : cfg(std::move(config)) {
        const auto engine_cwd = std::filesystem::absolute(utf8_path(cfg.value("cwd", ".")));
#ifdef _WIN32
        const auto prefix = "strata-vision-" + std::to_string(GetCurrentProcessId());
#else
        const auto prefix = "strata-vision-" + std::to_string(getpid());
#endif
        for (const auto &parent : std::vector<std::filesystem::path>{
                 std::filesystem::temp_directory_path(), engine_cwd, std::filesystem::current_path()}) {
            const auto candidate = parent / utf8_path(api_id(prefix));
            if (parent != engine_cwd && path_utf8(candidate).find(' ') != std::string::npos)
                continue;
#ifdef _WIN32
            PSECURITY_DESCRIPTOR sd = nullptr;
            if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;OICI;FA;;;OW)", SDDL_REVISION_1,
                                                                      &sd, nullptr))
                throw std::runtime_error("cannot protect vision directory");
            SECURITY_ATTRIBUTES sa{sizeof(sa), sd, FALSE};
            const bool made = CreateDirectoryW(candidate.c_str(), &sa) != FALSE;
            LocalFree(sd);
            if (made) {
                root = candidate;
                break;
            }
#else
            std::error_code error;
            if (std::filesystem::create_directory(candidate, error)) {
                std::filesystem::permissions(candidate, std::filesystem::perms::owner_all);
                root = candidate;
                break;
            }
#endif
        }
        if (root.empty())
            throw std::runtime_error("cannot create private vision directory without spaces");
    }
    ~Impl() {
        process.reset();
        cache.clear();
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
    void ready(const std::function<bool()> &cancelled) {
        std::lock_guard<std::recursive_mutex> lock(mutex);
        if (process && process->alive())
            return;
        process.reset();
        const auto &vision = cfg["vision"];
        std::vector<std::string> args;
        const auto cwd = std::filesystem::absolute(utf8_path(cfg.value("cwd", ".")));
        auto absolute = [&](const J &value) {
            auto path = utf8_path(value.get<std::string>());
            return path_utf8(path.is_relative() ? cwd / path : path);
        };
        const auto host = executable_path();
        if (!vision.contains("exe")) {
            args = {path_utf8(host), "vision"};
        } else {
            args = {absolute(vision.at("exe"))};
            if (std::filesystem::equivalent(utf8_path(args.front()), host)) args.push_back("vision");
        }
        args.insert(args.end(), {"--mmproj", absolute(vision.at("mmproj")), "--model",
                                 absolute(vision.at("model"))});
        if (vision.value("gpu", false))
            args.push_back("--gpu");
        for (auto key : {"threads", "max_tokens", "min_tokens"})
            if (vision.contains(key)) {
                std::string option = key;
                std::replace(option.begin(), option.end(), '_', '-');
                args.push_back("--" + option);
                args.push_back(vision[key].dump());
            }
        auto environment = child_environment(cfg);
        if (vision.contains("cuda_device"))
            environment[cfg.value("backend", "") == "hip" ? "HIP_VISIBLE_DEVICES" : "CUDA_VISIBLE_DEVICES"] =
                vision["cuda_device"].is_string() ? vision["cuda_device"].get<std::string>()
                                                  : vision["cuda_device"].dump();
        process = std::make_unique<EngineProcess>(args, root, utf8_path(cfg.value("log", "")), environment,
                                                  1u << 20, child_environment_removed(cfg));
        double limit = 300;
        const auto setting = server_environment("STRATA_VISION_READY_S");
        if (!setting.empty())
            limit = std::stod(setting);
        const auto born = Clock::now();
        try {
            for (;;) {
                if (cancelled && cancelled())
                    throw ApiError(499, "vision startup cancelled", "server_error");
                if (limit > 0 && std::chrono::duration<double>(Clock::now() - born).count() > limit)
                    throw ApiError(503, "vision encoder startup timed out", "server_error");
                std::string line;
                if (!process->receive(line, std::chrono::milliseconds(100)))
                    continue;
                if (!line.starts_with("READY"))
                    throw ApiError(503, "the vision encoder did not start", "server_error");
                break;
            }
        } catch (...) {
            process.reset();
            throw;
        }
    }
};
NativeVision::NativeVision(const J &cfg) : impl_(std::make_unique<Impl>(cfg)) {
    if (!cfg.value("lazy_load", false))
        ensure_loaded();
}
NativeVision::~NativeVision() = default;
void NativeVision::ensure_loaded(const std::function<bool()> &cancelled) { impl_->ready(cancelled); }
void NativeVision::unload() {
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
    if (impl_->process)
        impl_->process->finish();
    impl_->process.reset();
}
std::filesystem::path NativeVision::directory() const { return impl_->root; }
std::string NativeVision::command_path(const std::filesystem::path &path) const {
    const auto cwd = std::filesystem::absolute(utf8_path(impl_->cfg.value("cwd", ".")));
    return path_utf8(impl_->root.parent_path() == cwd ? std::filesystem::relative(path, cwd) : path);
}
std::pair<std::filesystem::path, int> NativeVision::encode(const std::string &bytes,
                                                           const std::function<bool()> &cancelled,
                                                           std::vector<std::shared_ptr<void>> &retained) {
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
    const auto key = digest(bytes);
    impl_->lru.remove(key);
    impl_->lru.push_back(key);
    if (!impl_->cache.count(key)) {
        ensure_loaded(cancelled);
        const auto image = impl_->root / (key + ".img"), embedding = impl_->root / (key + ".sve");
        {
            std::ofstream file(image, std::ios::binary);
            file.write(bytes.data(), bytes.size());
            if (!file)
                throw ApiError(400, "could not stage image");
        }
        struct Remove {
            std::filesystem::path p;
            ~Remove() {
                std::error_code e;
                std::filesystem::remove(p, e);
            }
        } remove{image};
        try {
            impl_->process->send("ENC " + image.filename().string() + " " + embedding.filename().string());
            const auto born = Clock::now();
            double limit = 300;
            const auto setting = server_environment("STRATA_VISION_ENCODE_S");
            if (!setting.empty())
                limit = std::stod(setting);
            for (;;) {
                if (cancelled && cancelled())
                    throw ApiError(499, "image encoding cancelled", "server_error");
                if (limit > 0 && std::chrono::duration<double>(Clock::now() - born).count() > limit)
                    throw ApiError(400, "image encoding timed out");
                std::string line;
                if (!impl_->process->receive(line, std::chrono::milliseconds(100)))
                    continue;
                std::istringstream in(line);
                std::string kind;
                int n = 0, x = 0, y = 0;
                double ms = 0;
                if (!(in >> kind >> n >> x >> y >> ms) || kind != "OK" || n < 1 || int64_t(x) * y != n ||
                    !std::isfinite(ms) || ms < 0 || !std::filesystem::is_regular_file(embedding))
                    throw ApiError(400, "the image could not be encoded");
                int32_t header[5]{};
                std::ifstream saved(embedding, std::ios::binary);
                saved.read(reinterpret_cast<char *>(header), sizeof(header));
                if (!saved || header[0] != 0x31455653 || header[1] != n || header[2] != x || header[3] != y ||
                    header[4] < 1 ||
                    std::filesystem::file_size(embedding) != 20 + uint64_t(n) * header[4] * 4)
                    throw ApiError(400, "invalid image embeddings record");
                auto entry = std::make_shared<Impl::Entry>();
                entry->path = embedding;
                entry->tokens = n;
                impl_->cache[key] = entry;
                break;
            }
        } catch (...) {
            impl_->process.reset();
            std::error_code e;
            std::filesystem::remove(embedding, e);
            throw;
        }
        while (impl_->cache.size() > 64) {
            const auto oldest = impl_->lru.front();
            impl_->lru.pop_front();
            impl_->cache.erase(oldest);
        }
    }
    const auto entry = impl_->cache.at(key);
    retained.push_back(entry);
    return {entry->path, entry->tokens};
}
VisionJob::~VisionJob() {
    if (!path.empty()) {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    }
}
void prepare_vision(ApiRequest &request, NativeVision *vision, ServerEngine &engine,
                    const Tokenizer &tokenizer, const ChatTemplate &chat, int context, bool fit,
                    const std::function<bool()> &cancelled) {
    if (images_of(request.prompt.messages).empty())
        return;
    struct Input {
        J *part;
        bool tool;
        std::string bytes;
    };
    std::vector<Input> incoming;
    for (auto &message : request.prompt.messages)
        if (message.value("content", J(nullptr)).is_array())
            for (auto &part : message["content"])
                if (part.is_object() && part.value("type", "") == "image") {
                    const bool tool = message.value("role", "") == "tool";
                    try {
                        if (!vision)
                            throw ApiError(400, "this server has no image encoder");
                        incoming.push_back({&part, tool,
                                            NativeVision::normalize(
                                                NativeVision::load(part.value("source", ""), cancelled))});
                    } catch (const std::exception &e) {
                        if (!tool)
                            throw;
                        part = {{"type", "text"}, {"text", std::string("[image omitted: ") + e.what() + "]"}};
                    }
                }
    auto gate = engine.exclusive(cancelled);
    auto job = std::make_shared<VisionJob>();
    std::vector<std::pair<std::filesystem::path, int>> encoded;
    for (auto &image : incoming) {
        try {
            encoded.push_back(vision->encode(image.bytes, cancelled, job->retained));
        } catch (const std::exception &e) {
            if (!image.tool)
                throw;
            *image.part = {{"type", "text"}, {"text", std::string("[image omitted: ") + e.what() + "]"}};
        }
    }
    auto rendered = chat.render_prompt(request.prompt, tokenizer.controls());
    request.ids = tokenizer.encode(rendered.text, true, rendered.plain);
    if (!request.force.empty() && !request.thinking) {
        const auto force = tokenizer.encode(request.force, true);
        request.ids.insert(request.ids.end(), force.begin(), force.end());
    }
    if (!encoded.empty()) {
        const auto pad = tokenizer.encode("<|image_pad|>", true),
                   start = tokenizer.encode("<|vision_start|>", true);
        if (pad.size() != 1 || start.size() != 1)
            throw ApiError(400, "tokenizer has no vision control tokens");
        size_t index = 0;
        std::vector<int> expanded;
        for (size_t i = 0; i < request.ids.size(); ++i) {
            if (request.ids[i] == pad[0] && i > 0 && request.ids[i - 1] == start[0]) {
                if (index >= encoded.size())
                    throw ApiError(400, "prompt and images do not match");
                expanded.insert(expanded.end(), static_cast<size_t>(encoded[index++].second), pad[0]);
            } else
                expanded.push_back(request.ids[i]);
        }
        if (index != encoded.size())
            throw ApiError(400, "prompt and images do not match");
        request.ids = std::move(expanded);
        job->path = vision->directory() / (api_id("req") + ".sve");
        job->engine_path = vision->command_path(job->path);
        std::ofstream file(job->path, std::ios::binary);
        for (const auto &[path, n] : encoded) {
            (void)n;
            std::ifstream part(path, std::ios::binary);
            file << part.rdbuf();
            if (!part || !file)
                throw ApiError(400, "could not combine image embeddings");
        }
        file.close();
        request.vision = job;
    }
    const auto room = int64_t(context) - static_cast<int64_t>(request.ids.size()) - 8;
    if (room < 1)
        throw ApiError(400, "image prompt leaves no room in context");
    const auto original =
        request.wire.value("max_output_tokens",
                           request.wire.value("max_completion_tokens", request.wire.value("max_tokens", 0)));
    request.max_new = original <= 0 ? static_cast<int>(room) : original;
    if (request.max_new > room) {
        if (!fit)
            throw ApiError(400, "image prompt and max_tokens exceed context");
        request.max_new = static_cast<int>(room);
    }
}
} // namespace strata::runtime
