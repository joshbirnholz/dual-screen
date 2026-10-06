#include "core/console_title_info.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "config/paths.hpp"
#include "core/ds_rom_info.hpp"
#include "core/save_store.hpp"

#ifdef DUALSCREEN_HAVE_ZSTD
#include <zstd.h>
#endif

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

namespace {

// meta.xml and iconTex.tga, wherever they were read from (`where`, for the
// error): the title ID, the English name and publisher, the icon.
bool parse_wii_u_meta(const std::vector<uint8_t>& xml_bytes, const std::vector<uint8_t>& tga,
                      const std::string& where, ConsoleTitleInfo& out, std::string* error) {
    out = ConsoleTitleInfo();
    const std::string xml(xml_bytes.begin(), xml_bytes.end());
    const std::string id = xml_text(xml, "title_id");
    if (id.size() != 16) {
        if (error != nullptr) *error = "no title ID in " + where;
        return false;
    }
    out.title_id = std::strtoull(id.c_str(), nullptr, 16);
    out.title = xml_text(xml, "longname_en");
    if (out.title.empty()) out.title = xml_text(xml, "shortname_en");
    out.publisher = xml_text(xml, "publisher_en");
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;
    if (!tga.empty() && read_tga(tga, width, height, rgba)) {
        shrink(width, height, rgba, 64);
        out.icon = encode_rgba_icon(width, height, rgba);
    }
    return out.title_id != 0;
}

// Cemu's single-file archive of a Wii U title, .wua: Exzap's ZArchive. At its
// end a footer, big-endian like every number in it, gives the sections: the
// data, cut into 64 KiB blocks each compressed with zstd on its own (one that
// would not shrink is kept as it is); a record per 16 blocks of where they
// start; the names; and the tree of files and folders, the root first.
class WuaArchive {
public:
    ~WuaArchive() {
        if (file_ != nullptr) std::fclose(file_);
    }

    bool open(const std::string& path, std::string* error) {
        const auto fail = [&](const std::string& why) {
            if (error != nullptr) *error = why;
            return false;
        };
        file_ = std::fopen(path.c_str(), "rb");
        if (file_ == nullptr) return fail("cannot open " + path);
        if (std::fseek(file_, 0, SEEK_END) != 0) return fail("cannot read " + path);
        const long end = std::ftell(file_);
        constexpr std::size_t kFooterSize = 16 * 6 + 32 + 8 + 4 + 4;
        uint8_t footer[kFooterSize];
        if (end < static_cast<long>(kFooterSize) ||
            !read_at(file_, static_cast<uint64_t>(end) - kFooterSize, footer, kFooterSize)) {
            return fail(path + " is too short to be a Wii U archive");
        }
        const uint64_t size = static_cast<uint64_t>(end);
        if (be32(footer + kFooterSize - 4) != 0x169f52d6u || be32(footer + kFooterSize - 8) != 0x61bf3a01u ||
            be64(footer + kFooterSize - 16) != size) {
            return fail(path + " is not a Wii U archive (.wua)");
        }
        struct Section {
            uint64_t offset;
            uint64_t size;
        } sections[6];
        for (int i = 0; i < 6; ++i) {
            sections[i] = {be64(footer + i * 16), be64(footer + i * 16 + 8)};
            if (sections[i].offset > size || sections[i].size > size - sections[i].offset) {
                return fail(path + " is damaged");
            }
        }
        const Section& data = sections[0];
        const Section& records = sections[1];
        const Section& names = sections[2];
        const Section& tree = sections[3];
        // Only the tables: the data stays on the disk.
        if (records.size % 40 != 0 || tree.size % 16 != 0 || tree.size == 0 || records.size > (64u << 20) ||
            names.size > (64u << 20) || tree.size > (64u << 20)) {
            return fail(path + " is damaged");
        }
        data_offset_ = data.offset;
        data_size_ = data.size;
        records_.resize(records.size);
        names_.resize(names.size);
        tree_.resize(tree.size);
        if (!read_at(file_, records.offset, records_.data(), records_.size()) ||
            (!names_.empty() && !read_at(file_, names.offset, names_.data(), names_.size())) ||
            !read_at(file_, tree.offset, tree_.data(), tree_.size()) || is_file(0)) {
            return fail("cannot read " + path);
        }
        return true;
    }

    std::size_t node_count() const { return tree_.size() / 16; }
    bool is_file(uint32_t node) const { return node < node_count() && (field(node, 0) & 0x80000000u) != 0; }

    std::string name(uint32_t node) const {
        uint32_t at = field(node, 0) & 0x7FFFFFFFu;
        if (at >= names_.size()) return std::string();
        std::size_t length = names_[at] & 0x7F;
        if ((names_[at] & 0x80) != 0) {
            if (at + 1 >= names_.size()) return std::string();
            length |= std::size_t(names_[at + 1]) << 7;
            at += 2;
        } else {
            at += 1;
        }
        if (at + length > names_.size()) return std::string();
        return std::string(reinterpret_cast<const char*>(names_.data()) + at, length);
    }

    std::vector<uint32_t> children(uint32_t folder) const {
        std::vector<uint32_t> out;
        if (folder >= node_count() || is_file(folder)) return out;
        const uint64_t first = field(folder, 1);
        const uint64_t count = field(folder, 2);
        for (uint64_t node = first; node < first + count && node < node_count(); ++node) {
            out.push_back(static_cast<uint32_t>(node));
        }
        return out;
    }

    // A folder's child by name, compared as ZArchive does, ignoring case.
    bool child(uint32_t folder, const std::string& wanted, uint32_t& out) const {
        for (const uint32_t node : children(folder)) {
            const std::string have = name(node);
            if (have.size() != wanted.size()) continue;
            bool same = true;
            for (std::size_t i = 0; i < have.size() && same; ++i) {
                same = std::tolower(static_cast<unsigned char>(have[i])) ==
                       std::tolower(static_cast<unsigned char>(wanted[i]));
            }
            if (same) {
                out = node;
                return true;
            }
        }
        return false;
    }

    // A file's contents, when it is no bigger than `most`.
    bool read(uint32_t node, std::vector<uint8_t>& out, uint64_t most) {
        if (!is_file(node)) return false;
        const uint32_t high = field(node, 3);
        const uint64_t offset = field(node, 1) | uint64_t(high & 0xFFFF) << 32;
        const uint64_t size = field(node, 2) | uint64_t(high & 0xFFFF0000u) << 16;
        if (size > most) return false;
        out.resize(static_cast<std::size_t>(size));
        uint64_t done = 0;
        while (done < size) {
            const uint64_t at = offset + done;
            if (!load_block(at / kBlockSize)) return false;
            const std::size_t inside = static_cast<std::size_t>(at % kBlockSize);
            const std::size_t step = static_cast<std::size_t>(std::min<uint64_t>(size - done, kBlockSize - inside));
            std::memcpy(out.data() + done, block_.data() + inside, step);
            done += step;
        }
        return true;
    }

private:
    static constexpr std::size_t kBlockSize = 64 * 1024;
    static constexpr std::size_t kBlocksPerRecord = 16;

    uint32_t field(uint32_t node, int index) const { return be32(tree_.data() + node * 16 + index * 4); }

    bool load_block(uint64_t index) {
        if (index == block_index_) return true;
        const uint64_t record = index / kBlocksPerRecord;
        if ((record + 1) * 40 > records_.size()) return false;
        const uint8_t* entry = records_.data() + record * 40;
        uint64_t offset = be64(entry);
        const std::size_t within = static_cast<std::size_t>(index % kBlocksPerRecord);
        for (std::size_t i = 0; i < within; ++i) {
            offset += uint64_t(uint32_t(entry[8 + i * 2]) << 8 | entry[9 + i * 2]) + 1;
        }
        const std::size_t stored = (std::size_t(entry[8 + within * 2]) << 8 | entry[9 + within * 2]) + 1;
        if (offset + stored > data_size_) return false;
        block_.resize(kBlockSize);
        block_index_ = UINT64_MAX;
        if (stored == kBlockSize) {
            if (!read_at(file_, data_offset_ + offset, block_.data(), kBlockSize)) return false;
        } else {
#ifdef DUALSCREEN_HAVE_ZSTD
            std::vector<uint8_t> packed(stored);
            if (!read_at(file_, data_offset_ + offset, packed.data(), stored)) return false;
            if (ZSTD_decompress(block_.data(), kBlockSize, packed.data(), stored) != kBlockSize) return false;
#else
            // Built without zstd: only blocks kept as they are can be read.
            return false;
#endif
        }
        block_index_ = index;
        return true;
    }

    std::FILE* file_ = nullptr;
    uint64_t data_offset_ = 0;
    uint64_t data_size_ = 0;
    std::vector<uint8_t> records_;
    std::vector<uint8_t> names_;
    std::vector<uint8_t> tree_;
    std::vector<uint8_t> block_;
    uint64_t block_index_ = UINT64_MAX;
};

}  // namespace

bool read_wii_u_meta(const std::string& meta_dir, ConsoleTitleInfo& out, std::string* error) {
    out = ConsoleTitleInfo();
    std::vector<uint8_t> bytes;
    if (!read_file(path_join(meta_dir, "meta.xml"), bytes, nullptr) || bytes.empty()) {
        if (error != nullptr) *error = "no meta.xml in " + meta_dir;
        return false;
    }
    std::vector<uint8_t> tga;
    if (!read_file(path_join(meta_dir, "iconTex.tga"), tga, nullptr)) tga.clear();
    return parse_wii_u_meta(bytes, tga, meta_dir + "/meta.xml", out, error);
}

bool read_wii_u_archive_title_info(const std::string& wua_path, const std::string& sub_path, uint64_t title_id,
                                   ConsoleTitleInfo& out, std::string* error) {
    out = ConsoleTitleInfo();
    WuaArchive archive;
    if (!archive.open(wua_path, error)) return false;
    // The title's own folder: the one Cemu named, else the first whose name
    // starts with its title ID ("0005000010101c00_v0"), else the first with
    // the same game's (an update's, "0005000e10101c00_v208", says the same).
    uint32_t folder = 0;
    bool found = !sub_path.empty() && archive.child(0, sub_path, folder);
    char id[17];
    std::snprintf(id, sizeof(id), "%016llx", static_cast<unsigned long long>(title_id));
    for (int pass = 0; pass < 2 && !found && title_id != 0; ++pass) {
        for (const uint32_t node : archive.children(0)) {
            std::string name = archive.name(node);
            for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (archive.is_file(node) || name.size() < 16) continue;
            const bool same = pass == 0 ? name.compare(0, 16, id) == 0 : name.compare(8, 8, id + 8) == 0;
            if (same) {
                folder = node;
                found = true;
                break;
            }
        }
    }
    uint32_t meta = 0;
    uint32_t xml = 0;
    if (!found || !archive.child(folder, "meta", meta) || !archive.child(meta, "meta.xml", xml)) {
        if (error != nullptr) *error = "no meta/meta.xml for " + std::string(id) + " in " + wua_path;
        return false;
    }
    std::vector<uint8_t> xml_bytes;
    if (!archive.read(xml, xml_bytes, 1 << 20)) {
        if (error != nullptr) *error = "cannot read meta.xml in " + wua_path;
        return false;
    }
    std::vector<uint8_t> tga;
    uint32_t icon = 0;
    if (!archive.child(meta, "iconTex.tga", icon) || !archive.read(icon, tga, 4 << 20)) tga.clear();
    return parse_wii_u_meta(xml_bytes, tga, wua_path, out, error);
}

std::vector<CemuTitleListEntry> read_cemu_title_list(const std::string& path) {
    std::vector<CemuTitleListEntry> out;
    std::vector<uint8_t> bytes;
    if (!read_file(path, bytes, nullptr)) return out;
    const std::string xml(bytes.begin(), bytes.end());
    // <title_list><title titleId="0005000010101c00" ...><name>..</name>
    // <format>1</format><path>..</path><sub_path>..</sub_path></title>...
    for (std::size_t at = xml.find("<title"); at != std::string::npos; at = xml.find("<title", at + 1)) {
        const char after = at + 6 < xml.size() ? xml[at + 6] : '\0';
        if (after != ' ' && after != '>' && after != '\t' && after != '\n' && after != '\r') continue;
        const std::size_t close = xml.find("</title>", at);
        if (close == std::string::npos) break;
        const std::string element = xml.substr(at, close + 8 - at);
        const std::size_t id_at = element.find("titleId=\"");
        const std::size_t tag_end = element.find('>');
        if (id_at == std::string::npos || id_at > tag_end) continue;
        const std::size_t id_end = element.find('"', id_at + 9);
        if (id_end == std::string::npos || id_end - (id_at + 9) != 16) continue;
        CemuTitleListEntry entry;
        entry.title_id = std::strtoull(element.substr(id_at + 9, 16).c_str(), nullptr, 16);
        if (entry.title_id == 0) continue;
        entry.name = xml_text(element, "name");
        entry.format = std::atoi(xml_text(element, "format").c_str());
        entry.path = xml_text(element, "path");
        entry.sub_path = xml_text(element, "sub_path");
        out.push_back(entry);
    }
    return out;
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
