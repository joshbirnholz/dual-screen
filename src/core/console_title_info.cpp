#include "core/console_title_info.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "config/paths.hpp"
#include "core/ds_rom_info.hpp"
#include "core/save_store.hpp"

namespace ds {
namespace {

constexpr uint32_t kMediaUnit = 0x200;
constexpr std::size_t kSmdhSize = 0x36C0;
constexpr int kSmdhIconSize = 48;

uint32_t le32(const uint8_t* at) {
    return uint32_t(at[0]) | uint32_t(at[1]) << 8 | uint32_t(at[2]) << 16 | uint32_t(at[3]) << 24;
}
uint64_t le64(const uint8_t* at) { return uint64_t(le32(at)) | uint64_t(le32(at + 4)) << 32; }
uint32_t be32(const uint8_t* at) {
    return uint32_t(at[0]) << 24 | uint32_t(at[1]) << 16 | uint32_t(at[2]) << 8 | uint32_t(at[3]);
}
uint64_t be64(const uint8_t* at) { return uint64_t(be32(at)) << 32 | be32(at + 4); }

uint64_t align64(uint64_t value) { return (value + 63) & ~uint64_t(63); }

// Reads `size` bytes at `offset`, false when the file is shorter.
bool read_at(std::FILE* file, uint64_t offset, uint8_t* out, std::size_t size) {
    return std::fseek(file, static_cast<long>(offset), SEEK_SET) == 0 && std::fread(out, 1, size, file) == size;
}

std::string trim(std::string text) {
    for (char& c : text) {
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    }
    std::string out;
    for (const char c : text) {
        if (c == ' ' && (out.empty() || out.back() == ' ')) continue;
        out += c;
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

std::string hex16(uint64_t value) {
    char text[17];
    std::snprintf(text, sizeof(text), "%016llX", static_cast<unsigned long long>(value));
    return text;
}

// The name, publisher and icon in an NCCH's ExeFS, when it is not encrypted.
void read_ncch_smdh(std::FILE* file, uint64_t ncch, ConsoleTitleInfo& out) {
    uint8_t header[0x200];
    if (!read_at(file, ncch, header, sizeof(header)) || std::memcmp(header + 0x100, "NCCH", 4) != 0) return;
    const bool no_crypto = (header[0x18F] & 0x04) != 0;
    if (!no_crypto) return;  // the ExeFS is encrypted: only the title ID is to be had
    const uint64_t exefs = ncch + uint64_t(le32(header + 0x1A0)) * kMediaUnit;
    uint8_t exefs_header[0x200];
    if (le32(header + 0x1A4) == 0 || !read_at(file, exefs, exefs_header, sizeof(exefs_header))) return;
    for (int index = 0; index < 10; ++index) {
        const uint8_t* entry = exefs_header + index * 16;
        if (std::memcmp(entry, "icon\0\0\0\0", 8) != 0) continue;
        const uint32_t offset = le32(entry + 8);
        const uint32_t size = le32(entry + 12);
        if (size < kSmdhSize) return;
        std::vector<uint8_t> smdh(kSmdhSize);
        if (!read_at(file, exefs + 0x200 + offset, smdh.data(), smdh.size()) ||
            std::memcmp(smdh.data(), "SMDH", 4) != 0) {
            return;
        }
        // The English names, falling back to Japanese.
        for (const int language : {1, 0}) {
            const uint8_t* names = smdh.data() + 0x8 + language * 0x200;
            const std::string title = trim(utf16le_to_utf8(names, 0x40));
            if (title.empty()) continue;
            out.title = title;
            out.publisher = trim(utf16le_to_utf8(names + 0x180, 0x40));
            break;
        }
        std::vector<uint8_t> rgba;
        if (decode_smdh_icon(smdh.data(), smdh.size(), rgba)) {
            out.icon = encode_rgba_icon(kSmdhIconSize, kSmdhIconSize, rgba);
        }
        return;
    }
}

// A TGA as Cemu's meta folders hold one: 32 or 24 bits, uncompressed or RLE.
bool read_tga(const std::vector<uint8_t>& bytes, int& width, int& height, std::vector<uint8_t>& rgba) {
    if (bytes.size() < 18) return false;
    const uint8_t id_length = bytes[0];
    const uint8_t type = bytes[2];
    width = bytes[12] | bytes[13] << 8;
    height = bytes[14] | bytes[15] << 8;
    const int depth = bytes[16] / 8;
    const bool top_down = (bytes[17] & 0x20) != 0;
    if ((type != 2 && type != 10) || (depth != 3 && depth != 4) || width <= 0 || height <= 0 || width > 1024 ||
        height > 1024 || bytes[1] != 0) {
        return false;
    }
    const std::size_t pixels = static_cast<std::size_t>(width) * height;
    std::vector<uint8_t> raw(pixels * 4);
    std::size_t at = 18 + id_length;
    const auto take = [&](std::size_t index) {
        if (at + depth > bytes.size()) return false;
        raw[index * 4] = bytes[at + 2];
        raw[index * 4 + 1] = bytes[at + 1];
        raw[index * 4 + 2] = bytes[at];
        raw[index * 4 + 3] = depth == 4 ? bytes[at + 3] : 255;
        at += depth;
        return true;
    };
    std::size_t done = 0;
    while (done < pixels) {
        if (type == 2) {
            if (!take(done++)) return false;
            continue;
        }
        if (at >= bytes.size()) return false;
        const uint8_t packet = bytes[at++];
        const std::size_t count = std::min<std::size_t>((packet & 0x7F) + 1, pixels - done);
        if (packet & 0x80) {
            if (!take(done)) return false;
            for (std::size_t i = 1; i < count; ++i) std::memcpy(&raw[(done + i) * 4], &raw[done * 4], 4);
            done += count;
        } else {
            for (std::size_t i = 0; i < count; ++i) {
                if (!take(done++)) return false;
            }
        }
    }
    rgba.resize(pixels * 4);
    for (int y = 0; y < height; ++y) {
        const int from = top_down ? y : height - 1 - y;
        std::memcpy(&rgba[static_cast<std::size_t>(y) * width * 4], &raw[static_cast<std::size_t>(from) * width * 4],
                    static_cast<std::size_t>(width) * 4);
    }
    return true;
}

// Halved until it is no more than `most` across, each pixel the average of four.
void shrink(int& width, int& height, std::vector<uint8_t>& rgba, int most) {
    while (width > most && width % 2 == 0 && height % 2 == 0) {
        const int w = width / 2;
        const int h = height / 2;
        std::vector<uint8_t> out(static_cast<std::size_t>(w) * h * 4);
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                for (int c = 0; c < 4; ++c) {
                    const auto at = [&](int dx, int dy) {
                        return rgba[(static_cast<std::size_t>(y * 2 + dy) * width + x * 2 + dx) * 4 + c];
                    };
                    out[(static_cast<std::size_t>(y) * w + x) * 4 + c] =
                        static_cast<uint8_t>((at(0, 0) + at(1, 0) + at(0, 1) + at(1, 1) + 2) / 4);
                }
            }
        }
        rgba = std::move(out);
        width = w;
        height = h;
    }
}

}  // namespace

std::string utf16le_to_utf8(const uint8_t* data, std::size_t max_units) {
    std::string out;
    for (std::size_t index = 0; index < max_units; ++index) {
        uint32_t unit = data[index * 2] | data[index * 2 + 1] << 8;
        if (unit == 0) break;
        if (unit >= 0xD800 && unit < 0xDC00 && index + 1 < max_units) {
            const uint32_t low = data[index * 2 + 2] | data[index * 2 + 3] << 8;
            if (low >= 0xDC00 && low < 0xE000) {
                unit = 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00);
                ++index;
            }
        }
        if (unit < 0x80) {
            out += static_cast<char>(unit);
        } else if (unit < 0x800) {
            out += static_cast<char>(0xC0 | unit >> 6);
            out += static_cast<char>(0x80 | (unit & 0x3F));
        } else if (unit < 0x10000) {
            out += static_cast<char>(0xE0 | unit >> 12);
            out += static_cast<char>(0x80 | ((unit >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (unit & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | unit >> 18);
            out += static_cast<char>(0x80 | ((unit >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((unit >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (unit & 0x3F));
        }
    }
    return out;
}

bool decode_smdh_icon(const uint8_t* smdh, std::size_t size, std::vector<uint8_t>& rgba) {
    constexpr std::size_t kLargeIcon = 0x24C0;
    if (size < kLargeIcon + kSmdhIconSize * kSmdhIconSize * 2) return false;
    const uint8_t* pixels = smdh + kLargeIcon;
    rgba.assign(static_cast<std::size_t>(kSmdhIconSize) * kSmdhIconSize * 4, 255);
    // 8x8 tiles left to right, top to bottom, each in Z order: a pixel's x
    // from the even bits of its place in the tile, its y from the odd.
    std::size_t index = 0;
    for (int tile_y = 0; tile_y < kSmdhIconSize / 8; ++tile_y) {
        for (int tile_x = 0; tile_x < kSmdhIconSize / 8; ++tile_x) {
            for (int k = 0; k < 64; ++k, ++index) {
                const int x = tile_x * 8 + ((k & 1) | ((k >> 1) & 2) | ((k >> 2) & 4));
                const int y = tile_y * 8 + (((k >> 1) & 1) | ((k >> 2) & 2) | ((k >> 3) & 4));
                const uint16_t value = static_cast<uint16_t>(pixels[index * 2] | pixels[index * 2 + 1] << 8);
                uint8_t* out = &rgba[(static_cast<std::size_t>(y) * kSmdhIconSize + x) * 4];
                out[0] = static_cast<uint8_t>(((value >> 11) & 31) * 255 / 31);
                out[1] = static_cast<uint8_t>(((value >> 5) & 63) * 255 / 63);
                out[2] = static_cast<uint8_t>((value & 31) * 255 / 31);
            }
        }
    }
    return true;
}

std::string xml_text(const std::string& xml, const std::string& tag) {
    // The tag itself, not one whose name only starts with it.
    std::size_t open = xml.find("<" + tag);
    while (open != std::string::npos) {
        const std::size_t after = open + 1 + tag.size();
        if (after < xml.size() && (xml[after] == '>' || xml[after] == '/' ||
                                   std::isspace(static_cast<unsigned char>(xml[after])) != 0)) {
            break;
        }
        open = xml.find("<" + tag, after);
    }
    if (open == std::string::npos) return std::string();
    const std::size_t start = xml.find('>', open);
    if (start == std::string::npos || xml[start - 1] == '/') return std::string();
    const std::size_t end = xml.find("</" + tag + ">", start);
    if (end == std::string::npos) return std::string();
    std::string text = xml.substr(start + 1, end - start - 1);
    // The five entities XML writes for itself.
    const std::pair<const char*, const char*> entities[] = {
        {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&apos;", "'"}, {"&amp;", "&"}};
    for (const auto& [from, to] : entities) {
        for (std::size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + 1)) {
            text.replace(at, std::strlen(from), to);
        }
    }
    return trim(text);
}

bool read_3ds_title_info(const std::string& path, ConsoleTitleInfo& out, std::string* error) {
    out = ConsoleTitleInfo();
    const auto fail = [&](const std::string& why) {
        if (error != nullptr) *error = why;
        return false;
    };
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) return fail("cannot open " + path);
    uint8_t header[0x200];
    if (!read_at(file, 0, header, sizeof(header))) {
        std::fclose(file);
        return fail(path + " is too short to be a 3DS game");
    }
    uint64_t ncch = UINT64_MAX;
    if (std::memcmp(header + 0x100, "NCSD", 4) == 0) {
        // A cartridge: its first partition is the game.
        ncch = uint64_t(le32(header + 0x120)) * kMediaUnit;
        uint8_t ncch_header[0x200];
        if (read_at(file, ncch, ncch_header, sizeof(ncch_header)) &&
            std::memcmp(ncch_header + 0x100, "NCCH", 4) == 0) {
            out.title_id = le64(ncch_header + 0x118);
        } else {
            out.title_id = le64(header + 0x108);
        }
    } else if (std::memcmp(header + 0x100, "NCCH", 4) == 0) {
        ncch = 0;
        out.title_id = le64(header + 0x118);
    } else if (le32(header) == 0x2020) {
        // An installable package: the title ID is in its TMD, big-endian, and
        // the game itself is its first content.
        const uint64_t cert = align64(le32(header));
        const uint64_t ticket = align64(cert + le32(header + 0x08));
        const uint64_t tmd = align64(ticket + le32(header + 0x0C));
        const uint64_t content = align64(tmd + le32(header + 0x10));
        uint8_t signature[4];
        if (read_at(file, tmd, signature, sizeof(signature))) {
            uint64_t skip = 0;
            switch (be32(signature)) {
                case 0x010003: skip = 4 + 0x200 + 0x3C; break;  // RSA-4096
                case 0x010004: skip = 4 + 0x100 + 0x3C; break;  // RSA-2048
                case 0x010005: skip = 4 + 0x3C + 0x40; break;   // ECDSA
                default: break;
            }
            uint8_t id[8];
            if (skip != 0 && read_at(file, tmd + skip + 0x4C, id, sizeof(id))) out.title_id = be64(id);
        }
        ncch = content;
    } else {
        std::fclose(file);
        return fail(path + " is not a 3DS cartridge, executable or package");
    }
    if (ncch != UINT64_MAX) read_ncch_smdh(file, ncch, out);
    std::fclose(file);
    if (out.title_id == 0) return fail("no title ID in " + path);
    return true;
}

bool read_wii_u_meta(const std::string& meta_dir, ConsoleTitleInfo& out, std::string* error) {
    out = ConsoleTitleInfo();
    std::vector<uint8_t> bytes;
    if (!read_file(path_join(meta_dir, "meta.xml"), bytes, nullptr) || bytes.empty()) {
        if (error != nullptr) *error = "no meta.xml in " + meta_dir;
        return false;
    }
    const std::string xml(bytes.begin(), bytes.end());
    const std::string id = xml_text(xml, "title_id");
    if (id.size() != 16) {
        if (error != nullptr) *error = "no title ID in " + meta_dir + "/meta.xml";
        return false;
    }
    out.title_id = std::strtoull(id.c_str(), nullptr, 16);
    out.title = xml_text(xml, "longname_en");
    if (out.title.empty()) out.title = xml_text(xml, "shortname_en");
    out.publisher = xml_text(xml, "publisher_en");
    std::vector<uint8_t> tga;
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;
    if (read_file(path_join(meta_dir, "iconTex.tga"), tga, nullptr) && read_tga(tga, width, height, rgba)) {
        shrink(width, height, rgba, 64);
        out.icon = encode_rgba_icon(width, height, rgba);
    }
    return out.title_id != 0;
}

bool read_wii_u_title_info(const std::string& rom_path, ConsoleTitleInfo& out, std::string* error) {
    if (path_extension_lower(rom_path) != "rpx") {
        if (error != nullptr) *error = "only a Wii U game kept as a folder says what it is";
        return false;
    }
    // <game>/code/<name>.rpx, and <game>/meta beside code.
    const std::string game = path_dirname(path_dirname(rom_path));
    return read_wii_u_meta(path_join(game, "meta"), out, error);
}

std::string three_ds_game_id(uint64_t title_id) { return "CTR-" + hex16(title_id); }
std::string wii_u_game_id(uint64_t title_id) { return "WUP-" + hex16(title_id); }

uint64_t title_id_from_game_id(const std::string& game_id) {
    if (game_id.size() != 20 || (game_id.compare(0, 4, "CTR-") != 0 && game_id.compare(0, 4, "WUP-") != 0)) {
        return 0;
    }
    for (std::size_t index = 4; index < game_id.size(); ++index) {
        if (!std::isxdigit(static_cast<unsigned char>(game_id[index]))) return 0;
    }
    return std::strtoull(game_id.c_str() + 4, nullptr, 16);
}

std::vector<uint8_t> encode_rgba_icon(int width, int height, const std::vector<uint8_t>& rgba) {
    if (width <= 0 || height <= 0 || width > 256 || height > 256 ||
        rgba.size() != static_cast<std::size_t>(width) * height * 4) {
        return {};
    }
    std::vector<uint8_t> out = {'R', 'G', 'B', 'A', static_cast<uint8_t>(width), static_cast<uint8_t>(width >> 8),
                                static_cast<uint8_t>(height), static_cast<uint8_t>(height >> 8)};
    out.insert(out.end(), rgba.begin(), rgba.end());
    return out;
}

bool decode_rgba_icon(const std::vector<uint8_t>& bytes, int& width, int& height, std::vector<uint8_t>& rgba) {
    if (bytes.size() < 8 || std::memcmp(bytes.data(), "RGBA", 4) != 0) return false;
    width = bytes[4] | bytes[5] << 8;
    height = bytes[6] | bytes[7] << 8;
    if (width <= 0 || height <= 0 || width > 256 || height > 256 ||
        bytes.size() != 8 + static_cast<std::size_t>(width) * height * 4) {
        return false;
    }
    rgba.assign(bytes.begin() + 8, bytes.end());
    return true;
}

bool save_icon_rgba(const std::string& system, const std::vector<uint8_t>& icon, int& width, int& height,
                    std::vector<uint8_t>& rgba) {
    if (system != kSaveSystemDs) return decode_rgba_icon(icon, width, height, rgba);
    const std::vector<uint32_t> pixels = decode_ds_icon(icon);
    if (pixels.empty()) return false;
    width = kDsIconSize;
    height = kDsIconSize;
    rgba.resize(pixels.size() * 4);
    for (std::size_t index = 0; index < pixels.size(); ++index) {
        const uint32_t argb = pixels[index];
        rgba[index * 4] = static_cast<uint8_t>(argb >> 16);
        rgba[index * 4 + 1] = static_cast<uint8_t>(argb >> 8);
        rgba[index * 4 + 2] = static_cast<uint8_t>(argb);
        rgba[index * 4 + 3] = static_cast<uint8_t>(argb >> 24);
    }
    return true;
}

}  // namespace ds
