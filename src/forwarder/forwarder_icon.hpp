#pragma once

#include <cstdint>
#include <vector>

namespace ds {

// A forwarder's icon is what the Switch's HOME menu shows for it: a 256x256
// baseline JPEG, the only thing the menu will draw. Everything here turns a
// picture from somewhere -- the ROM's own banner icon, or one downloaded from
// SteamGridDB -- into that.

constexpr int kForwarderIconSize = 256;

// RGBA, 8 bits a channel, row by row.
struct RgbaImage {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> pixels;

    bool empty() const { return width <= 0 || height <= 0 || pixels.empty(); }
};

// A PNG, a JPEG, a BMP or a Windows icon (the biggest picture in it). False
// for anything else, or anything unreasonably large.
bool decode_image(const std::vector<uint8_t>& encoded, RgbaImage& out);
// As a baseline JPEG, the only kind the HOME menu reads. Empty on failure.
std::vector<uint8_t> encode_jpeg(const RgbaImage& image);

// The icon a picture makes, 256x256: a square or nearly square one scaled
// smoothly, its middle if it is not quite square; a tall or wide one -- a
// portrait grid -- whole, over a dimmed blur of itself. Anything see-through
// laid over black, and written as a JPEG. Empty when the picture cannot be read.
std::vector<uint8_t> forwarder_icon_from_image(const std::vector<uint8_t>& encoded);

// The icon a DS ROM's own 32x32 banner icon makes -- the bytes DsRomInfo::icon
// holds -- each of its pixels an 8x8 block, so the pixel art stays sharp, on
// white, as the DS's own menu shows it. Empty without a banner icon.
std::vector<uint8_t> forwarder_icon_from_ds_banner(const std::vector<uint8_t>& ds_icon);

// A plain icon, for a ROM with no banner of its own until another is chosen.
std::vector<uint8_t> forwarder_icon_blank();

}  // namespace ds
