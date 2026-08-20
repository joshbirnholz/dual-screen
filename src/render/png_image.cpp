#include "render/png_image.hpp"

#include <zlib.h>

#include <cstring>

namespace ds {
namespace {

uint32_t read_be32(const uint8_t* data) {
    return (static_cast<uint32_t>(data[0]) << 24) | (static_cast<uint32_t>(data[1]) << 16) |
           (static_cast<uint32_t>(data[2]) << 8) | static_cast<uint32_t>(data[3]);
}

bool fail(std::string* error, const char* reason) {
    if (error != nullptr) *error = reason;
    return false;
}

// Paeth, from the PNG specification. The one filter that is not obvious.
uint8_t paeth(uint8_t a, uint8_t b, uint8_t c) {
    const int p = static_cast<int>(a) + static_cast<int>(b) - static_cast<int>(c);
    const int pa = std::abs(p - static_cast<int>(a));
    const int pb = std::abs(p - static_cast<int>(b));
    const int pc = std::abs(p - static_cast<int>(c));
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

// Undoes one scanline's filter in place, given the reconstructed line above it.
bool unfilter_row(uint8_t filter,
                  uint8_t* row,
                  const uint8_t* previous,
                  std::size_t row_bytes,
                  std::size_t bytes_per_pixel) {
    switch (filter) {
        case 0:
            break;
        case 1:  // Sub
            for (std::size_t i = bytes_per_pixel; i < row_bytes; ++i) {
                row[i] = static_cast<uint8_t>(row[i] + row[i - bytes_per_pixel]);
            }
            break;
        case 2:  // Up
            for (std::size_t i = 0; i < row_bytes; ++i) {
                row[i] = static_cast<uint8_t>(row[i] + previous[i]);
            }
            break;
        case 3:  // Average
            for (std::size_t i = 0; i < row_bytes; ++i) {
                const int left = (i >= bytes_per_pixel) ? row[i - bytes_per_pixel] : 0;
                row[i] = static_cast<uint8_t>(row[i] + ((left + previous[i]) >> 1));
            }
            break;
        case 4:  // Paeth
            for (std::size_t i = 0; i < row_bytes; ++i) {
                const uint8_t left = (i >= bytes_per_pixel) ? row[i - bytes_per_pixel] : 0;
                const uint8_t up_left = (i >= bytes_per_pixel) ? previous[i - bytes_per_pixel] : 0;
                row[i] = static_cast<uint8_t>(row[i] + paeth(left, previous[i], up_left));
            }
            break;
        default:
            return false;
    }
    return true;
}

}  // namespace

bool decode_png(const uint8_t* data, std::size_t size, DecodedImage& out, std::string* error) {
    static const uint8_t kSignature[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (data == nullptr || size < 8 || std::memcmp(data, kSignature, sizeof(kSignature)) != 0) {
        return fail(error, "not a PNG");
    }

    int width = 0;
    int height = 0;
    int channels = 0;
    bool seen_header = false;
    std::vector<uint8_t> compressed;

    std::size_t offset = 8;
    while (offset + 8 <= size) {
        const uint32_t length = read_be32(data + offset);
        const char* type = reinterpret_cast<const char*>(data + offset + 4);
        const std::size_t body = offset + 8;
        if (length > size || body + length + 4 > size) {
            return fail(error, "truncated PNG chunk");
        }

        if (std::memcmp(type, "IHDR", 4) == 0) {
            if (length < 13) return fail(error, "short PNG header");
            width = static_cast<int>(read_be32(data + body));
            height = static_cast<int>(read_be32(data + body + 4));
            const uint8_t bit_depth = data[body + 8];
            const uint8_t colour_type = data[body + 9];
            const uint8_t interlace = data[body + 12];
            if (width <= 0 || height <= 0) return fail(error, "empty PNG");
            if (bit_depth != 8) return fail(error, "only 8-bit PNGs are supported");
            if (interlace != 0) return fail(error, "interlaced PNGs are not supported");
            switch (colour_type) {
                case 0: channels = 1; break;  // greyscale
                case 2: channels = 3; break;  // RGB
                case 4: channels = 2; break;  // greyscale + alpha
                case 6: channels = 4; break;  // RGBA
                default: return fail(error, "palettes are not supported");
            }
            seen_header = true;
        } else if (std::memcmp(type, "IDAT", 4) == 0) {
            compressed.insert(compressed.end(), data + body, data + body + length);
        } else if (std::memcmp(type, "IEND", 4) == 0) {
            break;
        }
        offset = body + length + 4;  // skip the chunk's CRC as well
    }

    if (!seen_header) return fail(error, "PNG has no header");
    if (compressed.empty()) return fail(error, "PNG has no image data");

    const std::size_t bytes_per_pixel = static_cast<std::size_t>(channels);
    const std::size_t row_bytes = static_cast<std::size_t>(width) * bytes_per_pixel;
    // One filter byte per row, ahead of the row itself.
    std::vector<uint8_t> raw((row_bytes + 1) * static_cast<std::size_t>(height));
    uLongf raw_size = static_cast<uLongf>(raw.size());
    if (uncompress(raw.data(), &raw_size, compressed.data(),
                   static_cast<uLong>(compressed.size())) != Z_OK ||
        raw_size != raw.size()) {
        return fail(error, "PNG image data did not decompress");
    }

    out.width = width;
    out.height = height;
    out.pixels.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height), 0);

    std::vector<uint8_t> previous(row_bytes, 0);
    std::vector<uint8_t> current(row_bytes, 0);
    for (int y = 0; y < height; ++y) {
        const std::size_t line = static_cast<std::size_t>(y) * (row_bytes + 1);
        const uint8_t filter = raw[line];
        std::memcpy(current.data(), raw.data() + line + 1, row_bytes);
        if (!unfilter_row(filter, current.data(), previous.data(), row_bytes, bytes_per_pixel)) {
            return fail(error, "unknown PNG row filter");
        }

        uint32_t* row_out = out.pixels.data() + static_cast<std::size_t>(y) * width;
        for (int x = 0; x < width; ++x) {
            const uint8_t* pixel = current.data() + static_cast<std::size_t>(x) * bytes_per_pixel;
            uint8_t r = 0;
            uint8_t g = 0;
            uint8_t b = 0;
            uint8_t a = 255;
            switch (channels) {
                case 1: r = g = b = pixel[0]; break;
                case 2: r = g = b = pixel[0]; a = pixel[1]; break;
                case 3: r = pixel[0]; g = pixel[1]; b = pixel[2]; break;
                default: r = pixel[0]; g = pixel[1]; b = pixel[2]; a = pixel[3]; break;
            }
            row_out[x] = (static_cast<uint32_t>(a) << 24) | (static_cast<uint32_t>(r) << 16) |
                         (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
        }
        previous.swap(current);
    }
    return true;
}

}  // namespace ds
