#include "core/game_title.hpp"

#include <algorithm>
#include <cstring>
#include <vector>

#include "config/paths.hpp"

namespace ds {
namespace {

// Little-endian reads that refuse to run off the end rather than trusting a
// length field in a file somebody else wrote. Every offset in these formats
// comes out of the file itself, so each one is a chance to be pointed
// somewhere that is not there.
bool read_u16(const uint8_t* data, std::size_t size, std::size_t at, uint32_t& out) {
    if (at + 2 > size) return false;
    out = static_cast<uint32_t>(data[at]) | (static_cast<uint32_t>(data[at + 1]) << 8);
    return true;
}

bool read_u32(const uint8_t* data, std::size_t size, std::size_t at, uint32_t& out) {
    if (at + 4 > size) return false;
    out = static_cast<uint32_t>(data[at]) | (static_cast<uint32_t>(data[at + 1]) << 8) |
          (static_cast<uint32_t>(data[at + 2]) << 16) | (static_cast<uint32_t>(data[at + 3]) << 24);
    return true;
}

bool magic_at(const uint8_t* data, std::size_t size, std::size_t at, const char* magic) {
    const std::size_t length = std::strlen(magic);
    if (at + length > size) return false;
    return std::memcmp(data + at, magic, length) == 0;
}

void append_utf8(uint32_t code_point, std::string& out) {
    if (code_point < 0x80) {
        out.push_back(static_cast<char>(code_point));
    } else if (code_point < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (code_point >> 6)));
        out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    } else if (code_point < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (code_point >> 12)));
        out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (code_point >> 18)));
        out.push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    }
}

// Both consoles store their titles as UTF-16LE in a fixed-size field, padded
// with zeroes, so this stops at the first one and at the end of the field.
// Everything downstream of here -- the log, the wire, the text renderer -- is
// UTF-8.
std::string utf16le_to_utf8(const uint8_t* data, std::size_t size, std::size_t at,
                            std::size_t units) {
    std::string out;
    for (std::size_t index = 0; index < units; ++index) {
        uint32_t unit = 0;
        if (!read_u16(data, size, at + index * 2, unit)) break;
        if (unit == 0) break;
        // A character outside the basic plane arrives as a pair. Anything else
        // starting with a high surrogate is broken; drop it rather than emit a
        // code point that is not one.
        if (unit >= 0xD800 && unit <= 0xDBFF) {
            uint32_t low = 0;
            if (index + 1 >= units || !read_u16(data, size, at + (index + 1) * 2, low) ||
                low < 0xDC00 || low > 0xDFFF) {
                break;
            }
            append_utf8(0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00), out);
            ++index;
            continue;
        }
        if (unit >= 0xDC00 && unit <= 0xDFFF) break;
        append_utf8(unit, out);
    }
    return out;
}

std::string trimmed(const std::string& text) {
    std::size_t first = 0;
    while (first < text.size() && (text[first] == ' ' || text[first] == '\t' ||
                                   text[first] == '\r' || text[first] == '\n')) {
        ++first;
    }
    std::size_t last = text.size();
    while (last > first && (text[last - 1] == ' ' || text[last - 1] == '\t' ||
                            text[last - 1] == '\r' || text[last - 1] == '\n')) {
        --last;
    }
    return text.substr(first, last - first);
}

std::vector<std::string> non_empty_lines(const std::string& text) {
    std::vector<std::string> lines;
    std::string current;
    for (const char character : text) {
        if (character == '\n' || character == '\r') {
            const std::string line = trimmed(current);
            if (!line.empty()) lines.push_back(line);
            current.clear();
            continue;
        }
        current.push_back(character);
    }
    const std::string line = trimmed(current);
    if (!line.empty()) lines.push_back(line);
    return lines;
}

std::string join_with_spaces(const std::vector<std::string>& lines, std::size_t count) {
    std::string out;
    for (std::size_t index = 0; index < count && index < lines.size(); ++index) {
        if (!out.empty()) out.push_back(' ');
        out += lines[index];
    }
    return out;
}

// English, in both formats: the DS banner and the 3DS SMDH happen to agree
// that it is the second entry in their language tables, after Japanese.
constexpr std::size_t kEnglish = 1;

}  // namespace

std::vector<std::string> nds_banner_title_lines(const uint8_t* banner, std::size_t size) {
    if (banner == nullptr) return {};
    // Titles are a table of sixteen-bit characters, 128 of them per language,
    // after 0x240 bytes of version, checksums and icon bitmap. English first;
    // Japanese, the table's first entry, for a game that has no English one.
    constexpr std::size_t kTitlesAt = 0x240;
    constexpr std::size_t kTitleChars = 128;
    for (const std::size_t language : {kEnglish, std::size_t{0}}) {
        const std::size_t at = kTitlesAt + language * kTitleChars * 2;
        std::vector<std::string> lines = non_empty_lines(utf16le_to_utf8(banner, size, at, kTitleChars));
        if (!lines.empty()) return lines;
    }
    return {};
}

std::string nds_title_from_rom(const uint8_t* data, std::size_t size) {
    if (data == nullptr) return std::string();
    // The header's pointer to the banner. Zero means the ROM has none, which
    // homebrew often does not.
    uint32_t banner = 0;
    if (!read_u32(data, size, 0x068, banner) || banner == 0 || banner >= size) {
        return std::string();
    }
    const std::vector<std::string> lines =
        nds_banner_title_lines(data + banner, size - static_cast<std::size_t>(banner));
    if (lines.empty()) return std::string();
    // The last line is the publisher, by a convention every commercial DS
    // banner follows. One line on its own is the title and nothing else.
    return lines.size() == 1 ? lines[0] : join_with_spaces(lines, lines.size() - 1);
}

std::string smdh_title(const uint8_t* data, std::size_t size) {
    if (data == nullptr || !magic_at(data, size, 0, "SMDH")) return std::string();
    // Sixteen language entries after an eight-byte header, each holding a
    // short name, a long one and a publisher.
    constexpr std::size_t kTitlesAt = 0x08;
    constexpr std::size_t kTitleStride = 0x200;
    constexpr std::size_t kShortChars = 0x40;
    const std::string title = utf16le_to_utf8(data, size, kTitlesAt + kEnglish * kTitleStride,
                                              kShortChars);
    // The short description is the one line the HOME menu shows, so it is
    // already a title rather than a sentence -- but some carry a second line.
    const std::vector<std::string> lines = non_empty_lines(title);
    return join_with_spaces(lines, lines.size());
}

namespace {

// Everything in a card image is counted in 0x200-byte media units.
constexpr std::size_t kMediaUnit = 0x200;

// The SMDH inside an NCCH -- a bare .cxi or .app, or the first partition of a
// card image. It is a file called "icon" in the ExeFS, whose header is a table
// of ten name-offset-size entries.
std::string smdh_from_ncch(const uint8_t* data, std::size_t size, std::size_t ncch_at) {
    if (!magic_at(data, size, ncch_at + 0x100, "NCCH")) return std::string();
    uint32_t exefs_units = 0;
    uint32_t exefs_size_units = 0;
    if (!read_u32(data, size, ncch_at + 0x1A0, exefs_units) ||
        !read_u32(data, size, ncch_at + 0x1A4, exefs_size_units) || exefs_units == 0) {
        return std::string();
    }
    const std::size_t exefs_at = ncch_at + static_cast<std::size_t>(exefs_units) * kMediaUnit;
    constexpr std::size_t kExeFsEntries = 10;
    constexpr std::size_t kExeFsEntryBytes = 0x10;
    constexpr std::size_t kExeFsHeaderBytes = 0x200;
    for (std::size_t entry = 0; entry < kExeFsEntries; ++entry) {
        const std::size_t at = exefs_at + entry * kExeFsEntryBytes;
        // An encrypted ExeFS header is indistinguishable from noise, so no
        // entry will be named "icon" and this simply finds nothing. That is
        // the intended outcome: without the console's keys there is no title
        // in here to read.
        if (!magic_at(data, size, at, "icon")) continue;
        uint32_t offset = 0;
        if (!read_u32(data, size, at + 8, offset)) return std::string();
        const std::size_t smdh_at = exefs_at + kExeFsHeaderBytes + offset;
        if (smdh_at >= size) return std::string();
        return smdh_title(data + smdh_at, size - smdh_at);
    }
    return std::string();
}

std::size_t align_up(std::size_t value, std::size_t to) {
    return (value + to - 1) / to * to;
}

// A .cia keeps its SMDH in a meta region at the very end, past the certificate
// chain, ticket, title metadata and the content itself -- each of which is
// padded to a 64-byte boundary before the next begins.
std::string smdh_from_cia(const uint8_t* data, std::size_t size) {
    uint32_t header_size = 0;
    if (!read_u32(data, size, 0x00, header_size) || header_size != 0x2020) return std::string();
    uint32_t cert_size = 0;
    uint32_t ticket_size = 0;
    uint32_t tmd_size = 0;
    uint32_t meta_size = 0;
    uint32_t content_low = 0;
    uint32_t content_high = 0;
    if (!read_u32(data, size, 0x08, cert_size) || !read_u32(data, size, 0x0C, ticket_size) ||
        !read_u32(data, size, 0x10, tmd_size) || !read_u32(data, size, 0x14, meta_size) ||
        !read_u32(data, size, 0x18, content_low) || !read_u32(data, size, 0x1C, content_high)) {
        return std::string();
    }
    if (meta_size == 0) return std::string();  // built without one; nothing to read
    constexpr std::size_t kAlign = 64;
    const std::size_t content_size =
        static_cast<std::size_t>(content_low) | (static_cast<std::size_t>(content_high) << 32);
    const std::size_t meta_at = align_up(header_size, kAlign) + align_up(cert_size, kAlign) +
                                align_up(ticket_size, kAlign) + align_up(tmd_size, kAlign) +
                                align_up(content_size, kAlign);
    // Past the dependency list and the core version at the front of the meta
    // region.
    constexpr std::size_t kSmdhInMeta = 0x400;
    const std::size_t smdh_at = meta_at + kSmdhInMeta;
    if (smdh_at >= size) return std::string();
    return smdh_title(data + smdh_at, size - smdh_at);
}

// A card image: eight partitions, of which the first is the game.
std::string smdh_from_cci(const uint8_t* data, std::size_t size) {
    if (!magic_at(data, size, 0x100, "NCSD")) return std::string();
    uint32_t partition_units = 0;
    if (!read_u32(data, size, 0x120, partition_units) || partition_units == 0) return std::string();
    return smdh_from_ncch(data, size, static_cast<std::size_t>(partition_units) * kMediaUnit);
}

// Enough of an XML reader for one element, which is all this needs and far
// less than a parser. meta.xml is machine-written by Nintendo's own tools, so
// it is regular in the ways a hand-written file would not be.
std::string xml_element_text(const std::string& xml, const std::string& name) {
    const std::string open = "<" + name;
    const std::size_t start = xml.find(open);
    if (start == std::string::npos) return std::string();
    // Past the rest of the opening tag, attributes and all.
    const std::size_t text_at = xml.find('>', start);
    if (text_at == std::string::npos) return std::string();
    const std::size_t end = xml.find("</" + name, text_at);
    if (end == std::string::npos) return std::string();
    return xml.substr(text_at + 1, end - text_at - 1);
}

std::string decode_xml_entities(const std::string& text) {
    static const struct {
        const char* entity;
        const char* character;
    } kEntities[] = {{"&amp;", "&"},  {"&lt;", "<"},   {"&gt;", ">"},
                     {"&quot;", "\""}, {"&apos;", "'"}};
    std::string out;
    for (std::size_t index = 0; index < text.size();) {
        bool matched = false;
        if (text[index] == '&') {
            for (const auto& entity : kEntities) {
                const std::size_t length = std::strlen(entity.entity);
                if (text.compare(index, length, entity.entity) == 0) {
                    out += entity.character;
                    index += length;
                    matched = true;
                    break;
                }
            }
        }
        if (!matched) {
            out.push_back(text[index]);
            ++index;
        }
    }
    return out;
}

}  // namespace

std::string three_ds_title_from_rom(const uint8_t* data, std::size_t size) {
    if (data == nullptr) return std::string();
    if (magic_at(data, size, 0, "SMDH")) return smdh_title(data, size);
    if (magic_at(data, size, 0x100, "NCSD")) return smdh_from_cci(data, size);
    if (magic_at(data, size, 0x100, "NCCH")) return smdh_from_ncch(data, size, 0);
    return smdh_from_cia(data, size);
}

std::string wii_u_title_from_meta_xml(const std::string& xml) {
    const std::string raw = xml_element_text(xml, "longname_en");
    if (raw.empty()) return std::string();
    // The Wii U menu breaks long names across two lines; a log and a host list
    // want them on one.
    const std::vector<std::string> lines = non_empty_lines(decode_xml_entities(raw));
    return join_with_spaces(lines, lines.size());
}

std::string game_title_for_file(const std::string& path) {
    const std::string extension = path_extension_lower(path);

    // A Wii U's title is not in the file the player picked -- an .rpx is code
    // and a .wud is an encrypted disc -- it is in meta/meta.xml beside it,
    // which is the same file Cemu's own game list reads. Two shapes, so two
    // places to look: the folder of a loose game, and the code/ directory
    // inside one, whose sibling is meta/.
    const bool is_folder = directory_exists(path);
    if (is_folder || extension == "rpx" || extension == "wud" || extension == "wux" ||
        extension == "wua") {
        const std::string directory = is_folder ? path : path_dirname(path);
        const std::string candidates[] = {
            path_join(directory, path_join("meta", "meta.xml")),
            path_join(path_dirname(directory), path_join("meta", "meta.xml")),
        };
        for (const std::string& candidate : candidates) {
            std::vector<uint8_t> bytes;
            if (!read_file(candidate, bytes, nullptr) || bytes.empty()) continue;
            const std::string title = wii_u_title_from_meta_xml(
                std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
            if (!title.empty()) return title;
        }
        // There is nothing inside these files themselves to fall back on.
        return std::string();
    }

    std::vector<uint8_t> bytes;
    if (!read_file(path, bytes, nullptr) || bytes.empty()) return std::string();
    if (extension == "nds") return nds_title_from_rom(bytes.data(), bytes.size());
    if (extension == "3ds" || extension == "cci" || extension == "cia" || extension == "cxi" ||
        extension == "app" || extension == "smdh") {
        return three_ds_title_from_rom(bytes.data(), bytes.size());
    }
    return std::string();
}

}  // namespace ds
