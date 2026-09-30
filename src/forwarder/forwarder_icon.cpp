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
#define STBI_ONLY_BMP
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

uint32_t read_u32(const uint8_t* at) {
    return uint32_t(at[0]) | (uint32_t(at[1]) << 8) | (uint32_t(at[2]) << 16) | (uint32_t(at[3]) << 24);
}
uint16_t read_u16(const uint8_t* at) { return static_cast<uint16_t>(at[0] | (at[1] << 8)); }
void write_u32(uint8_t* at, uint32_t value) {
    for (int i = 0; i < 4; ++i) at[i] = static_cast<uint8_t>(value >> (i * 8));
}

bool decode_with_stb(const uint8_t* data, std::size_t size, RgbaImage& out) {
    if (size == 0 || size > (64u << 20)) return false;
    int width = 0;
    int height = 0;
    int channels = 0;
    const int length = static_cast<int>(size);
    if (!stbi_info_from_memory(data, length, &width, &height, &channels) || width <= 0 || height <= 0 ||
        width > kMaxDimension || height > kMaxDimension) {
        return false;
    }
    stbi_uc* pixels = stbi_load_from_memory(data, length, &width, &height, &channels, 4);
    if (pixels == nullptr) return false;
    out.width = width;
    out.height = height;
    out.pixels.assign(pixels, pixels + static_cast<std::size_t>(width) * height * 4);
    stbi_image_free(pixels);
    return true;
}

// A Windows icon, which is how many of SteamGridDB's icons come: several
// pictures of one icon at different sizes, each a PNG or a headerless BMP
// twice as tall as it is (its colours, then a mask). The biggest is read.
bool decode_ico(const std::vector<uint8_t>& bytes, RgbaImage& out) {
    if (bytes.size() < 6 || read_u16(&bytes[0]) != 0 || read_u16(&bytes[2]) != 1) return false;
    const std::size_t count = read_u16(&bytes[4]);
    std::size_t best = count;
    uint64_t best_score = 0;
    for (std::size_t index = 0; index < count; ++index) {
        const std::size_t entry = 6 + index * 16;
        if (entry + 16 > bytes.size()) return false;
        const uint64_t width = bytes[entry] == 0 ? 256 : bytes[entry];
        const uint64_t height = bytes[entry + 1] == 0 ? 256 : bytes[entry + 1];
        const uint64_t score = (width * height) << 8 | read_u16(&bytes[entry + 6]);
        if (best == count || score > best_score) {
            best = index;
            best_score = score;
        }
    }
    if (best == count) return false;
    const std::size_t entry = 6 + best * 16;
    const std::size_t size = read_u32(&bytes[entry + 8]);
    const std::size_t offset = read_u32(&bytes[entry + 12]);
    if (offset > bytes.size() || bytes.size() - offset < size || size < 40) return false;
    const uint8_t* payload = bytes.data() + offset;
    static const uint8_t kPng[4] = {0x89, 'P', 'N', 'G'};
    if (std::memcmp(payload, kPng, 4) == 0) return decode_with_stb(payload, size, out);

    // A BMP without its file header: given one back, and its own height.
    const uint32_t header_size = read_u32(payload);
    if (header_size < 40 || header_size > size) return false;
    const uint16_t bits = read_u16(payload + 14);
    const uint32_t compression = read_u32(payload + 16);
    const uint32_t colours_used = read_u32(payload + 32);
    const uint32_t palette = (colours_used != 0 ? colours_used : (bits <= 8 ? (1u << bits) : 0)) * 4;
    const uint32_t masks = (compression == 3 && header_size == 40) ? 12 : 0;
    std::vector<uint8_t> bmp(14 + size);
    bmp[0] = 'B';
    bmp[1] = 'M';
    write_u32(&bmp[2], static_cast<uint32_t>(bmp.size()));
    write_u32(&bmp[10], 14 + header_size + masks + palette);
    std::memcpy(bmp.data() + 14, payload, size);
    const int32_t doubled = static_cast<int32_t>(read_u32(payload + 8));
    write_u32(&bmp[14 + 8], static_cast<uint32_t>(doubled / 2));
    return decode_with_stb(bmp.data(), bmp.size(), out);
}

RgbaImage resized(const RgbaImage& image, int width, int height) {
    RgbaImage out;
    out.width = width;
    out.height = height;
    out.pixels.resize(static_cast<std::size_t>(width) * height * 4);
    if (!stbir_resize_uint8_linear(image.pixels.data(), image.width, image.height, image.width * 4,
                                   out.pixels.data(), width, height, width * 4, STBIR_RGBA)) {
        return RgbaImage();
    }
    return out;
}

RgbaImage centre_square(const RgbaImage& image) {
    const int side = std::min(image.width, image.height);
    const int left = (image.width - side) / 2;
    const int top = (image.height - side) / 2;
    RgbaImage square;
    square.width = side;
    square.height = side;
    square.pixels.resize(static_cast<std::size_t>(side) * side * 4);
    for (int row = 0; row < side; ++row) {
        std::memcpy(square.pixels.data() + static_cast<std::size_t>(row) * side * 4,
                    image.pixels.data() + (static_cast<std::size_t>(top + row) * image.width + left) * 4,
                    static_cast<std::size_t>(side) * 4);
    }
    return square;
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
    if (decode_ico(encoded, out)) return true;
    out = RgbaImage();
    return decode_with_stb(encoded.data(), encoded.size(), out);
}

std::vector<uint8_t> forwarder_icon_from_image(const std::vector<uint8_t>& encoded) {
    RgbaImage image;
    if (!decode_image(encoded, image)) return {};
    const int side = kForwarderIconSize;

    // Near enough square: its middle, which is nearly all of it.
    const int longer = std::max(image.width, image.height);
    const int shorter = std::min(image.width, image.height);
    if (longer * 4 <= shorter * 5) {
        RgbaImage icon = resized(centre_square(image), side, side);
        if (icon.empty()) return {};
        flatten(icon, 0);
        return encode_jpeg(icon);
    }

    // A tall or wide grid: all of it, over a dimmed blur of its own middle,
    // rather than a slice of it.
    RgbaImage backdrop = resized(resized(centre_square(image), 8, 8), side, side);
    if (backdrop.empty()) return {};
    flatten(backdrop, 0);
    for (std::size_t index = 0; index + 3 < backdrop.pixels.size(); index += 4) {
        for (int channel = 0; channel < 3; ++channel) {
            backdrop.pixels[index + channel] = static_cast<uint8_t>(backdrop.pixels[index + channel] * 55 / 100);
        }
    }
    const int width = image.width >= image.height ? side : std::max(1, image.width * side / image.height);
    const int height = image.height >= image.width ? side : std::max(1, image.height * side / image.width);
    const RgbaImage picture = resized(image, width, height);
    if (picture.empty()) return {};
    const int left = (side - width) / 2;
    const int top = (side - height) / 2;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const uint8_t* from = picture.pixels.data() + (static_cast<std::size_t>(y) * width + x) * 4;
            uint8_t* to = backdrop.pixels.data() + (static_cast<std::size_t>(top + y) * side + left + x) * 4;
            const unsigned alpha = from[3];
            for (int channel = 0; channel < 3; ++channel) {
                to[channel] = static_cast<uint8_t>((from[channel] * alpha + to[channel] * (255u - alpha) + 127u) / 255u);
            }
        }
    }
    return encode_jpeg(backdrop);
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
