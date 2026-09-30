#include "forwarder/forwarder_icon.hpp"

#include <algorithm>
#include <cstring>

#include "core/ds_rom_info.hpp"

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wtype-limits"
#pragma GCC diagnostic ignored "-Wdouble-promotion"
#endif
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include "stb/stb_image.h"
#define STBIW_NO_STDIO
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image_write.h"
#define STB_IMAGE_RESIZE_STATIC
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb/stb_image_resize2.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace ds {
namespace {

// Bigger than any icon or grid SteamGridDB serves, and small enough that a
// Switch decoding it does not run out of memory.
constexpr int kMaxDimension = 4096;
constexpr int kJpegQuality = 93;

// Anything see-through laid over `background`, so the JPEG, which has no
// transparency, shows what was meant to be seen.
void flatten(RgbaImage& image, uint8_t background) {
    for (std::size_t index = 0; index + 3 < image.pixels.size(); index += 4) {
        const unsigned alpha = image.pixels[index + 3];
        for (int channel = 0; channel < 3; ++channel) {
            const unsigned value = image.pixels[index + channel];
            image.pixels[index + channel] =
                static_cast<uint8_t>((value * alpha + background * (255u - alpha) + 127u) / 255u);
        }
        image.pixels[index + 3] = 255;
    }
}

}  // namespace

std::vector<uint8_t> encode_jpeg(const RgbaImage& image) {
    std::vector<uint8_t> out;
    const auto write = [](void* context, void* data, int size) {
        auto* buffer = static_cast<std::vector<uint8_t>*>(context);
        const auto* bytes = static_cast<const uint8_t*>(data);
        buffer->insert(buffer->end(), bytes, bytes + size);
    };
    if (image.empty() ||
        !stbi_write_jpg_to_func(write, &out, image.width, image.height, 4, image.pixels.data(),
                                kJpegQuality)) {
        return {};
    }
    return out;
}

bool decode_image(const std::vector<uint8_t>& encoded, RgbaImage& out) {
    out = RgbaImage();
    if (encoded.empty() || encoded.size() > (64u << 20)) return false;
    int width = 0;
    int height = 0;
    int channels = 0;
    const int size = static_cast<int>(encoded.size());
    if (!stbi_info_from_memory(encoded.data(), size, &width, &height, &channels) || width <= 0 ||
        height <= 0 || width > kMaxDimension || height > kMaxDimension) {
        return false;
    }
    stbi_uc* pixels = stbi_load_from_memory(encoded.data(), size, &width, &height, &channels, 4);
    if (pixels == nullptr) return false;
    out.width = width;
    out.height = height;
    out.pixels.assign(pixels, pixels + static_cast<std::size_t>(width) * height * 4);
    stbi_image_free(pixels);
    return true;
}

std::vector<uint8_t> forwarder_icon_from_image(const std::vector<uint8_t>& encoded) {
    RgbaImage image;
    if (!decode_image(encoded, image)) return {};

    // The middle square, which is all a square icon can show.
    const int side = std::min(image.width, image.height);
    const int left = (image.width - side) / 2;
    const int top = (image.height - side) / 2;
    RgbaImage square;
    square.width = side;
    square.height = side;
    square.pixels.resize(static_cast<std::size_t>(side) * side * 4);
    for (int row = 0; row < side; ++row) {
        std::memcpy(square.pixels.data() + static_cast<std::size_t>(row) * side * 4,
                    image.pixels.data() +
                        (static_cast<std::size_t>(top + row) * image.width + left) * 4,
                    static_cast<std::size_t>(side) * 4);
    }

    RgbaImage icon;
    icon.width = kForwarderIconSize;
    icon.height = kForwarderIconSize;
    icon.pixels.resize(static_cast<std::size_t>(kForwarderIconSize) * kForwarderIconSize * 4);
    if (!stbir_resize_uint8_linear(square.pixels.data(), side, side, side * 4, icon.pixels.data(),
                                   kForwarderIconSize, kForwarderIconSize, kForwarderIconSize * 4,
                                   STBIR_RGBA)) {
        return {};
    }
    flatten(icon, 0);
    return encode_jpeg(icon);
}

std::vector<uint8_t> forwarder_icon_from_ds_banner(const std::vector<uint8_t>& ds_icon) {
    const std::vector<uint32_t> pixels = decode_ds_icon(ds_icon);
    if (pixels.size() != static_cast<std::size_t>(kDsIconSize) * kDsIconSize) return {};
    constexpr int kScale = kForwarderIconSize / kDsIconSize;
    RgbaImage icon;
    icon.width = kForwarderIconSize;
    icon.height = kForwarderIconSize;
    icon.pixels.resize(static_cast<std::size_t>(kForwarderIconSize) * kForwarderIconSize * 4);
    for (int y = 0; y < kForwarderIconSize; ++y) {
        for (int x = 0; x < kForwarderIconSize; ++x) {
            const uint32_t argb = pixels[static_cast<std::size_t>(y / kScale) * kDsIconSize + x / kScale];
            uint8_t* out = icon.pixels.data() + (static_cast<std::size_t>(y) * kForwarderIconSize + x) * 4;
            out[0] = static_cast<uint8_t>(argb >> 16);
            out[1] = static_cast<uint8_t>(argb >> 8);
            out[2] = static_cast<uint8_t>(argb);
            out[3] = static_cast<uint8_t>(argb >> 24);
        }
    }
    flatten(icon, 255);
    return encode_jpeg(icon);
}

std::vector<uint8_t> forwarder_icon_blank() {
    RgbaImage icon;
    icon.width = kForwarderIconSize;
    icon.height = kForwarderIconSize;
    icon.pixels.assign(static_cast<std::size_t>(kForwarderIconSize) * kForwarderIconSize * 4, 255);
    for (std::size_t index = 0; index < icon.pixels.size(); index += 4) {
        icon.pixels[index] = 0x2d;
        icon.pixels[index + 1] = 0x31;
        icon.pixels[index + 2] = 0x3a;
    }
    return encode_jpeg(icon);
}

}  // namespace ds
