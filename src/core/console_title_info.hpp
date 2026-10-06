#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ds {

// What a 3DS or a Wii U game says about itself, for Data Management: the title
// ID that names its save on every device, and the name, publisher and icon to
// show it by. The DS's own is DsRomInfo.
struct ConsoleTitleInfo {
    uint64_t title_id = 0;
    std::string title;
    std::string publisher;
    // An RGBA icon -- see encode_rgba_icon() -- or empty.
    std::vector<uint8_t> icon;
};

// A 3DS game's ROM: a cartridge image (.3ds, .cci), its executable (.cxi) or
// an installable package (.cia). The title ID is in the clear in all of them;
// the name and icon only where the ROM is decrypted, as Azahar needs it to be.
// False, with why, when the file is none of these.
bool read_3ds_title_info(const std::string& path, ConsoleTitleInfo& out, std::string* error);

// A Wii U game as Cemu runs it from a folder: `rom_path` is its code/*.rpx,
// and meta/meta.xml beside code/ says the rest. A disc image (.wud, .wux) or
// an archive (.wua) is opaque here, and false.
bool read_wii_u_title_info(const std::string& rom_path, ConsoleTitleInfo& out, std::string* error);
// The same from a meta folder itself: a game folder's, or the one Cemu keeps
// for an installed title in mlc01/usr/title/<high>/<low>/meta.
bool read_wii_u_meta(const std::string& meta_dir, ConsoleTitleInfo& out, std::string* error);
// The same from inside Cemu's single-file archive of a game (.wua): from its
// folder `sub_path` ("0005000010101c00_v0"), or, when that is empty or not
// there, the folder of `title_id` or of an update to the same game. Archives
// are compressed with zstd: a build without it (DUALSCREEN_HAVE_ZSTD) reads
// none.
bool read_wii_u_archive_title_info(const std::string& wua_path, const std::string& sub_path, uint64_t title_id,
                                   ConsoleTitleInfo& out, std::string* error);

// A game in Cemu's game list, as it remembers it in title_list_cache.xml --
// whatever form the game is kept in, installed or not.
struct CemuTitleListEntry {
    uint64_t title_id = 0;
    std::string name;
    int format = 0;        // 1 a folder (with code/, content/, meta/), 2 a disc image, 3 a .wua
    std::string path;      // the folder or the file
    std::string sub_path;  // for a .wua, the title's folder inside it
};
inline constexpr int kCemuTitleFolder = 1;
inline constexpr int kCemuTitleArchive = 3;
std::vector<CemuTitleListEntry> read_cemu_title_list(const std::string& path);

// The game IDs Data Management knows these games by: the console's own code
// for it and the title ID, "CTR-0004000000055D00", "WUP-0005000010101C00".
std::string three_ds_game_id(uint64_t title_id);
std::string wii_u_game_id(uint64_t title_id);
// The title ID back out of one, or 0 when it is not one.
uint64_t title_id_from_game_id(const std::string& game_id);

// A small RGBA picture as bytes: "RGBA", its width and height, then the
// pixels, four bytes each. Empty when the size is not a sane icon's.
std::vector<uint8_t> encode_rgba_icon(int width, int height, const std::vector<uint8_t>& rgba);
bool decode_rgba_icon(const std::vector<uint8_t>& bytes, int& width, int& height, std::vector<uint8_t>& rgba);

// Any save's icon as RGBA pixels, whatever system it is -- a DS banner icon
// or one of the above. False when there is none.
bool save_icon_rgba(const std::string& system, const std::vector<uint8_t>& icon, int& width, int& height,
                    std::vector<uint8_t>& rgba);

// For tests: the pieces.
std::string utf16le_to_utf8(const uint8_t* data, std::size_t max_units);
bool decode_smdh_icon(const uint8_t* smdh, std::size_t size, std::vector<uint8_t>& rgba);  // 48x48
std::string xml_text(const std::string& xml, const std::string& tag);

}  // namespace ds
