#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ds {

// A decoded image, one pixel per uint32 in the 0xAARRGGBB the texture uploader
// takes. Kept in that layout rather than RGBA so it can be handed straight to
// Texture::upload_xrgb8888 with no second pass.
struct DecodedImage {
    int width = 0;
    int height = 0;
    std::vector<uint32_t> pixels;

    bool valid() const {
        return width > 0 && height > 0 &&
               pixels.size() == static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    }
};

// Decodes a PNG. Supports what the app's own assets are: 8 bits per channel,
// non-interlaced, greyscale/RGB/RGBA with or without an alpha channel. Anything
// else is refused with a reason rather than half-decoded -- this reads one file
// that ships inside the binary, so the useful behaviour on anything unexpected
// is to say so loudly during development.
bool decode_png(const uint8_t* data, std::size_t size, DecodedImage& out, std::string* error);

}  // namespace ds
