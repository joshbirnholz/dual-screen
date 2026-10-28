#include "core/ds_rom_info.hpp"

#include <cstdio>
#include <cstring>

#include "config/sha1.hpp"
#include "core/game_title.hpp"

namespace ds {
namespace {

// Where things are in the header -- GBATEK's "DS Cartridge Header".
constexpr std::size_t kGameCodeAt = 0x00C;
constexpr std::size_t kMakerCodeAt = 0x010;
constexpr std::size_t kRomVersionAt = 0x01E;
constexpr std::size_t kBannerOffsetAt = 0x068;
constexpr std::size_t kHeaderCrcAt = 0x15E;
constexpr std::size_t kHeaderBytes = 0x160;
// And in the banner: the icon's tiles, then its palette, then the titles,
// which run to 0x840 in the first version of the format, the one every
// banner has.
constexpr std::size_t kIconAt = 0x020;
constexpr std::size_t kBannerBytes = 0x840;

bool printable(const uint8_t* text, std::size_t length) {
    for (std::size_t index = 0; index < length; ++index) {
        if (text[index] < 0x20 || text[index] > 0x7E) return false;
    }
    return true;
}

uint32_t u16_at(const uint8_t* data, std::size_t at) {
    return static_cast<uint32_t>(data[at]) | (static_cast<uint32_t>(data[at + 1]) << 8);
}

uint32_t u32_at(const uint8_t* data, std::size_t at) {
    return u16_at(data, at) | (u16_at(data, at + 2) << 16);
}

}  // namespace

bool parse_ds_rom_info(const uint8_t* header, std::size_t header_size, const uint8_t* banner,
                       std::size_t banner_size, DsRomInfo& out) {
    out = DsRomInfo();
    if (header == nullptr || header_size < kHeaderBytes) return false;
    // A DS ROM's game code is four printable characters and its maker code
    // two; anything else is not a DS ROM, whatever its extension says.
    if (!printable(header + kGameCodeAt, 4) || !printable(header + kMakerCodeAt, 2)) return false;
    out.game_code.assign(reinterpret_cast<const char*>(header + kGameCodeAt), 4);
    const std::string maker(reinterpret_cast<const char*>(header + kMakerCodeAt), 2);
    char id[32];
    std::snprintf(id, sizeof(id), "%s-%s-%u-%04x", out.game_code.c_str(), maker.c_str(),
                  static_cast<unsigned>(header[kRomVersionAt]),
                  static_cast<unsigned>(u16_at(header, kHeaderCrcAt)));
    out.game_id = id;

    if (banner != nullptr && banner_size >= kIconAt + kDsIconBytes) {
        out.icon.assign(banner + kIconAt, banner + kIconAt + kDsIconBytes);
        std::vector<std::string> lines = nds_banner_title_lines(banner, banner_size);
        if (lines.size() >= 2) {
            out.publisher = lines.back();
            lines.pop_back();
        }
        for (const std::string& line : lines) {
            if (!out.title.empty()) out.title.push_back(' ');
            out.title += line;
        }
    }
    return true;
}

bool read_ds_rom_info(const std::string& path, DsRomInfo& out, std::string* error) {
    out = DsRomInfo();
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        if (error != nullptr) *error = "cannot open " + path;
        return false;
    }
    uint8_t header[kHeaderBytes];
    const bool have_header = std::fread(header, 1, sizeof(header), file) == sizeof(header);
    std::vector<uint8_t> banner;
    if (have_header) {
        const uint32_t banner_at = u32_at(header, kBannerOffsetAt);
        if (banner_at != 0 && std::fseek(file, static_cast<long>(banner_at), SEEK_SET) == 0) {
            banner.resize(kBannerBytes);
            banner.resize(std::fread(banner.data(), 1, banner.size(), file));
        }
    }
    std::fclose(file);
    if (!have_header || !parse_ds_rom_info(header, sizeof(header), banner.data(), banner.size(), out)) {
        if (error != nullptr) *error = path + " is not a DS ROM";
        return false;
    }
    return true;
}

std::vector<uint32_t> decode_ds_icon(const std::vector<uint8_t>& icon) {
    if (icon.size() != kDsIconBytes) return {};
    // Sixteen colours, BGR555; the first is see-through.
    uint32_t palette[16];
    for (int index = 0; index < 16; ++index) {
        const uint32_t colour = u16_at(icon.data(), 512 + index * 2);
        const auto channel = [](uint32_t five) { return (five << 3) | (five >> 2); };
        const uint32_t red = channel(colour & 0x1F);
        const uint32_t green = channel((colour >> 5) & 0x1F);
        const uint32_t blue = channel((colour >> 10) & 0x1F);
        palette[index] = (index == 0 ? 0u : 0xFF000000u) | (red << 16) | (green << 8) | blue;
    }
    // Four by four tiles of eight by eight pixels, two pixels a byte, the
    // left one in the low half.
    std::vector<uint32_t> pixels(static_cast<std::size_t>(kDsIconSize) * kDsIconSize);
    for (int tile = 0; tile < 16; ++tile) {
        const int tile_x = (tile % 4) * 8;
        const int tile_y = (tile / 4) * 8;
        for (int y = 0; y < 8; ++y) {
            for (int x = 0; x < 8; ++x) {
                const uint8_t pair = icon[static_cast<std::size_t>(tile * 32 + y * 4 + x / 2)];
                const int index = (x % 2 == 0) ? (pair & 0x0F) : (pair >> 4);
                pixels[static_cast<std::size_t>((tile_y + y) * kDsIconSize + tile_x + x)] =
                    palette[index];
            }
        }
    }
    return pixels;
}

std::string ds_icon_key(const std::vector<uint8_t>& icon) {
    const std::vector<uint32_t> pixels = decode_ds_icon(icon);
    if (pixels.empty()) return std::string();
    std::vector<uint8_t> packed;
    packed.reserve(pixels.size() * 2);
    for (const uint32_t pixel : pixels) {
        const uint32_t red = ((pixel >> 16) & 0xFF) >> 3;
        const uint32_t green = ((pixel >> 8) & 0xFF) >> 3;
        const uint32_t blue = (pixel & 0xFF) >> 3;
        const uint32_t value = red | (green << 5) | (blue << 10);
        packed.push_back(static_cast<uint8_t>(value & 0xFF));
        packed.push_back(static_cast<uint8_t>(value >> 8));
    }
    return Sha1::hash_bytes(packed.data(), packed.size());
}

}  // namespace ds
