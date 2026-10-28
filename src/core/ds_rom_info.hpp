#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ds {

// What a DS ROM says about itself, from its header and banner: enough to tell
// one game's save from another's whatever the files are called, and to show
// the game the way the console's own menu does.
//
// Read from the first few kilobytes of the file and nothing else, so a folder
// of ROMs can be gone through without reading a gigabyte of them.
struct DsRomInfo {
    // The game code ("AMCE" for Mario Kart DS, USA), the maker code, the ROM
    // version and the header's checksum, written "AMCE-01-0-1a2b". Stays the
    // same for a renamed file and for a trimmed dump; differs between regions,
    // whose saves are not always interchangeable, and for most ROM hacks,
    // which change the checksum.
    std::string game_id;
    std::string game_code;
    // From the banner: the title, and the publisher when the banner names one
    // (by convention, its last line). English, or Japanese when there is no
    // English. Empty for a ROM with no banner -- homebrew, mostly.
    std::string title;
    std::string publisher;
    // The banner's 32x32, 16-colour icon exactly as the ROM stores it -- 512
    // bytes of 4-bit tiles, then 16 BGR555 colours -- so it can be kept and
    // passed on at its own size. Empty when there is no banner.
    std::vector<uint8_t> icon;

    bool valid() const { return !game_id.empty(); }
};

constexpr std::size_t kDsIconBytes = 512 + 32;
constexpr int kDsIconSize = 32;

// From the ROM at `path`. False, with the reason, when it cannot be read or is
// not a DS ROM.
bool read_ds_rom_info(const std::string& path, DsRomInfo& out, std::string* error);

// From a header (at least 0x160 bytes) and, when there is one, the banner.
bool parse_ds_rom_info(const uint8_t* header, std::size_t header_size, const uint8_t* banner,
                       std::size_t banner_size, DsRomInfo& out);

// The stored icon as 32x32 pixels, 0xAARRGGBB, row by row -- colour 0 is
// transparent. Empty unless `icon` is kDsIconBytes long.
std::vector<uint32_t> decode_ds_icon(const std::vector<uint8_t>& icon);

// The name the icon is kept under for Discord to show (see
// util/discord_presence.hpp), worked out from the picture alone, since the
// collection it comes from knows releases rather than game codes: every
// pixel, row by row -- colour 0 as its colour, not see-through -- as BGR555
// in a little-endian 16-bit number, and the SHA-1 of those 2048 bytes.
// tools/build_ds_icons.py on the icons branch names them the same way.
// Empty unless `icon` is kDsIconBytes long.
std::string ds_icon_key(const std::vector<uint8_t>& icon);

}  // namespace ds
