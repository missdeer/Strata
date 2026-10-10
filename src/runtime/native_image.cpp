#include "strata/runtime/native_image.hpp"
#include "strata/runtime/server_engine.hpp"
#include <webp/decode.h>
#include <webp/demux.h>
#include <avif/avif.h>
extern "C" {
#include <avifexif.h>
}
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#include <algorithm>
#include <memory>
namespace strata::runtime {
namespace {
void dimensions(uint32_t w, uint32_t h) {
    if (!w || !h || uint64_t(w) * h > 100000000)
        throw ApiError(400, "image dimensions exceed limit");
}
} // namespace
bool decode_extra_image(const std::string &bytes, uint32_t &width, uint32_t &height,
                        std::vector<unsigned char> &rgba, int &orientation) {
    const auto *data = reinterpret_cast<const unsigned char *>(bytes.data());
    if (bytes.size() >= 12 && bytes.starts_with("RIFF") && bytes.compare(8, 4, "WEBP") == 0) {
        int w = 0, h = 0;
        if (!WebPGetInfo(data, bytes.size(), &w, &h))
            throw ApiError(400, "invalid WebP image");
        dimensions(static_cast<uint32_t>(w), static_cast<uint32_t>(h));
        WebPData raw{data, bytes.size()};
        WebPAnimDecoderOptions options{};
        if (!WebPAnimDecoderOptionsInit(&options))
            throw ApiError(400, "WebP decoder initialization failed");
        options.color_mode = MODE_RGBA;
        std::unique_ptr<WebPAnimDecoder, decltype(&WebPAnimDecoderDelete)> decoder(
            WebPAnimDecoderNew(&raw, &options), WebPAnimDecoderDelete);
        if (!decoder)
            throw ApiError(400, "WebP decoding failed");
        WebPAnimInfo info{};
        uint8_t *pixels = nullptr;
        int timestamp = 0;
        if (!WebPAnimDecoderGetInfo(decoder.get(), &info) ||
            !WebPAnimDecoderGetNext(decoder.get(), &pixels, &timestamp))
            throw ApiError(400, "WebP decoding failed");
        width = info.canvas_width;
        height = info.canvas_height;
        dimensions(width, height);
        rgba.assign(pixels, pixels + size_t(width) * height * 4);
        return true;
    }
    avifROData raw{data, bytes.size()};
    if (avifPeekCompatibleFileType(&raw)) {
        std::unique_ptr<avifDecoder, decltype(&avifDecoderDestroy)> decoder(avifDecoderCreate(),
                                                                            avifDecoderDestroy);
        if (!decoder)
            throw ApiError(400, "AVIF decoder allocation failed");
        decoder->maxThreads = 1;
        decoder->imageSizeLimit = 100000000;
        if (avifDecoderSetIOMemory(decoder.get(), data, bytes.size()) != AVIF_RESULT_OK ||
            avifDecoderParse(decoder.get()) != AVIF_RESULT_OK ||
            avifDecoderNextImage(decoder.get()) != AVIF_RESULT_OK)
            throw ApiError(400, "AVIF decoding failed");
        width = decoder->image->width;
        height = decoder->image->height;
        dimensions(width, height);
        orientation = avifImageGetExifOrientationFromIrotImir(decoder->image);
        struct RGB {
            avifRGBImage value{};
            ~RGB() { avifRGBImageFreePixels(&value); }
        } converted;
        avifRGBImageSetDefaults(&converted.value, decoder->image);
        converted.value.depth = 8;
        converted.value.format = AVIF_RGB_FORMAT_RGBA;
        if (avifRGBImageAllocatePixels(&converted.value) != AVIF_RESULT_OK ||
            avifImageYUVToRGB(decoder->image, &converted.value) != AVIF_RESULT_OK)
            throw ApiError(400, "AVIF color conversion failed");
        avifCropRect crop{0, 0, width, height};
        if (decoder->image->transformFlags & AVIF_TRANSFORM_CLAP) {
            avifDiagnostics diagnostics{};
            if (!avifCropRectFromCleanApertureBox(&crop, &decoder->image->clap, width, height, &diagnostics))
                throw ApiError(400, "invalid AVIF clean aperture");
        }
        rgba.resize(size_t(crop.width) * crop.height * 4);
        for (uint32_t y = 0; y < crop.height; ++y)
            std::copy_n(converted.value.pixels + size_t(y + crop.y) * converted.value.rowBytes +
                            size_t(crop.x) * 4,
                        size_t(crop.width) * 4, rgba.data() + size_t(y) * crop.width * 4);
        width = crop.width;
        height = crop.height;
        return true;
    }
    // JPEG uses WIC on Windows (and stb in the Linux caller) so the existing
    // EXIF path keeps its decoder. stb supplies the remaining PNM/TGA/PSD types.
    if (bytes.size() >= 3 && bytes.compare(0, 3, std::string("\xff\xd8\xff", 3)) == 0)
        return false;
    int w = 0, h = 0, components = 0;
    if (bytes.size() > 2147483647u ||
        !stbi_info_from_memory(data, static_cast<int>(bytes.size()), &w, &h, &components))
        return false;
    dimensions(static_cast<uint32_t>(w), static_cast<uint32_t>(h));
    auto *decoded = stbi_load_from_memory(data, static_cast<int>(bytes.size()), &w, &h, &components, 4);
    if (!decoded)
        return false;
    rgba.assign(decoded, decoded + size_t(w) * h * 4);
    stbi_image_free(decoded);
    width = static_cast<uint32_t>(w);
    height = static_cast<uint32_t>(h);
    return true;
}
} // namespace strata::runtime
