// 3DS and Wii U saves in Data Management: a save folder packed into one run
// of bytes and back, what a 3DS ROM and a Wii U game's meta folder say about
// themselves, and the save store keeping these folders the way it keeps a DS
// game's .srm -- found, versioned, set aside, put back and traded.
//
// ROMs are laid out by hand, as test_save_store's DS ones are.

#include "test_support.hpp"

#include <cstring>
#include <string>
#include <vector>

#include <stdlib.h>
#include <utime.h>

#ifdef DUALSCREEN_HAVE_ZSTD
#include <zstd.h>
#endif

#include "config/paths.hpp"
#include "core/console_title_info.hpp"
#include "core/save_folder.hpp"
#include "core/save_store.hpp"

using namespace ds;

namespace {

std::string make_sandbox() {
    char pattern[] = "/tmp/dualscreen-foldersaves-XXXXXX";
    const char* directory = mkdtemp(pattern);
    return directory != nullptr ? std::string(directory) : std::string();
}

bool make_directories(const std::string& path) {
    if (path.empty() || directory_exists(path)) return true;
    return make_directories(path_dirname(path)) && ensure_directory(path);
}

bool write_text(const std::string& path, const std::string& text) {
    return make_directories(path_dirname(path)) &&
           write_file_atomically(path, reinterpret_cast<const uint8_t*>(text.data()), text.size(), nullptr);
}

bool write_bytes(const std::string& path, const std::vector<uint8_t>& bytes) {
    return make_directories(path_dirname(path)) && write_file_atomically(path, bytes.data(), bytes.size(), nullptr);
}

std::string read_text(const std::string& path) {
    std::vector<uint8_t> bytes;
    if (!read_file(path, bytes, nullptr)) return "<missing>";
    return std::string(bytes.begin(), bytes.end());
}

// Moved a minute on, so a change is newer than the version noted before it
// within the same second.
void touch_later(const std::string& path, int64_t seconds) {
    struct utimbuf times {};
    times.actime = static_cast<time_t>(seconds);
    times.modtime = static_cast<time_t>(seconds);
    utime(path.c_str(), &times);
}

void put_le16(std::vector<uint8_t>& bytes, std::size_t at, uint32_t value) {
    if (bytes.size() < at + 2) bytes.resize(at + 2, 0);
    bytes[at] = static_cast<uint8_t>(value);
    bytes[at + 1] = static_cast<uint8_t>(value >> 8);
}
void put_le32(std::vector<uint8_t>& bytes, std::size_t at, uint32_t value) {
    put_le16(bytes, at, value & 0xFFFF);
    put_le16(bytes, at + 2, value >> 16);
}
void put_le64(std::vector<uint8_t>& bytes, std::size_t at, uint64_t value) {
    put_le32(bytes, at, static_cast<uint32_t>(value));
    put_le32(bytes, at + 4, static_cast<uint32_t>(value >> 32));
}
void put_be32(std::vector<uint8_t>& bytes, std::size_t at, uint32_t value) {
    if (bytes.size() < at + 4) bytes.resize(at + 4, 0);
    for (int i = 0; i < 4; ++i) bytes[at + i] = static_cast<uint8_t>(value >> (24 - 8 * i));
}
void put_be64(std::vector<uint8_t>& bytes, std::size_t at, uint64_t value) {
    put_be32(bytes, at, static_cast<uint32_t>(value >> 32));
    put_be32(bytes, at + 4, static_cast<uint32_t>(value));
}
void put_text(std::vector<uint8_t>& bytes, std::size_t at, const char* ascii) {
    const std::size_t length = std::strlen(ascii);
    if (bytes.size() < at + length) bytes.resize(at + length, 0);
    std::memcpy(bytes.data() + at, ascii, length);
}
void put_utf16(std::vector<uint8_t>& bytes, std::size_t at, const std::u16string& text) {
    for (std::size_t index = 0; index < text.size(); ++index) put_le16(bytes, at + index * 2, text[index]);
}

constexpr std::size_t kSmdhSize = 0x36C0;
constexpr std::size_t kLargeIcon = 0x24C0;

// An SMDH whose English names are `title` and `publisher`, and whose icon is
// black but for four pixels: (1,0) red, (0,1) green -- the second and third
// in the first tile's Z order -- (8,0) blue, the second tile's first, and
// (0,8) white, the first of the second row of tiles.
std::vector<uint8_t> smdh(const std::u16string& title, const std::u16string& publisher) {
    std::vector<uint8_t> bytes(kSmdhSize, 0);
    put_text(bytes, 0, "SMDH");
    const std::size_t english = 0x8 + 1 * 0x200;
    put_utf16(bytes, english, title);
    put_utf16(bytes, english + 0x80, title);
    put_utf16(bytes, english + 0x180, publisher);
    put_le16(bytes, kLargeIcon + 1 * 2, 0xF800);
    put_le16(bytes, kLargeIcon + 2 * 2, 0x07E0);
    put_le16(bytes, kLargeIcon + 64 * 2, 0x001F);
    put_le16(bytes, kLargeIcon + 6 * 64 * 2, 0xFFFF);
    return bytes;
}

// An NCCH for `title_id` whose ExeFS holds `icon`; encrypted, it says only
// its title ID.
std::vector<uint8_t> ncch(uint64_t title_id, const std::vector<uint8_t>& icon, bool encrypted = false) {
    std::vector<uint8_t> bytes(0x400, 0);
    put_text(bytes, 0x100, "NCCH");
    put_le64(bytes, 0x108, title_id);
    put_le64(bytes, 0x118, title_id);
    bytes[0x18F] = encrypted ? 0x00 : 0x04;
    put_le32(bytes, 0x1A0, 1);  // the ExeFS, one media unit in
    put_le32(bytes, 0x1A4, static_cast<uint32_t>((0x200 + icon.size() + 0x1FF) / 0x200));
    put_text(bytes, 0x200, "icon");
    put_le32(bytes, 0x200 + 8, 0);
    put_le32(bytes, 0x200 + 12, static_cast<uint32_t>(icon.size()));
    bytes.insert(bytes.end(), icon.begin(), icon.end());
    return bytes;
}

// A cartridge image whose first partition, one media unit in, is `game`.
std::vector<uint8_t> ncsd(const std::vector<uint8_t>& game, uint64_t media_id) {
    std::vector<uint8_t> bytes(0x200, 0);
    put_text(bytes, 0x100, "NCSD");
    put_le64(bytes, 0x108, media_id);
    put_le32(bytes, 0x120, 1);
    put_le32(bytes, 0x124, static_cast<uint32_t>(game.size() / 0x200));
    bytes.insert(bytes.end(), game.begin(), game.end());
    return bytes;
}

// An installable package for `title_id` whose content is `game`, its TMD
// signed with RSA-2048.
std::vector<uint8_t> cia(uint64_t title_id, const std::vector<uint8_t>& game) {
    const std::size_t cert_size = 0xA00;
    const std::size_t ticket_size = 0x350;
    const std::size_t tmd_size = 0x208 + 0x30;
    const auto align = [](std::size_t value) { return (value + 63) & ~std::size_t(63); };
    const std::size_t cert = align(0x2020);
    const std::size_t ticket = align(cert + cert_size);
    const std::size_t tmd = align(ticket + ticket_size);
    const std::size_t content = align(tmd + tmd_size);
    std::vector<uint8_t> bytes(content, 0);
    put_le32(bytes, 0x00, 0x2020);
    put_le32(bytes, 0x08, cert_size);
    put_le32(bytes, 0x0C, ticket_size);
    put_le32(bytes, 0x10, tmd_size);
    put_be32(bytes, tmd, 0x00010004);
    put_be64(bytes, tmd + 4 + 0x100 + 0x3C + 0x4C, title_id);
    bytes.insert(bytes.end(), game.begin(), game.end());
    return bytes;
}

// A 32-bit TGA, bottom-up as most are: `width` x `height`, all of it `rgba`
// but the top-left pixel, which is opaque black.
std::vector<uint8_t> tga(int width, int height, const uint8_t rgba[4]) {
    std::vector<uint8_t> bytes(18, 0);
    bytes[2] = 2;
    put_le16(bytes, 12, static_cast<uint32_t>(width));
    put_le16(bytes, 14, static_cast<uint32_t>(height));
    bytes[16] = 32;
    bytes[17] = 8;
    for (int y = height - 1; y >= 0; --y) {
        for (int x = 0; x < width; ++x) {
            const bool corner = x == 0 && y == 0;
            bytes.push_back(corner ? 0 : rgba[2]);
            bytes.push_back(corner ? 0 : rgba[1]);
            bytes.push_back(corner ? 0 : rgba[0]);
            bytes.push_back(corner ? 255 : rgba[3]);
        }
    }
    return bytes;
}

std::string meta_xml(const std::string& title_id, const std::string& name, const std::string& publisher) {
    return "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<menu type=\"complex\" access=\"777\">\n"
           "  <title_version type=\"unsignedInt\" length=\"4\">16</title_version>\n"
           "  <title_id type=\"hexBinary\" length=\"8\">" + title_id + "</title_id>\n"
           "  <longname_en type=\"string\" length=\"512\">" + name + "</longname_en>\n"
           "  <publisher_en type=\"string\" length=\"256\">" + publisher + "</publisher_en>\n"
           "</menu>\n";
}

const uint8_t* pixel(const std::vector<uint8_t>& rgba, int width, int x, int y) {
    return &rgba[(static_cast<std::size_t>(y) * width + x) * 4];
}

void test_pack_and_unpack(const std::string& sandbox) {
    const std::string from = path_join(sandbox, "pack-from");
    CHECK(write_text(path_join(from, "00000001/system"), "one"));
    CHECK(write_text(path_join(from, "00000001/user.dat"), "two"));
    CHECK(write_text(path_join(from, ".hidden"), "three"));
    CHECK(make_directories(path_join(from, "empty")));
    CHECK(save_folder_has_files(from));
    CHECK_EQ(save_folder_size(from), static_cast<uint64_t>(11));

    std::vector<uint8_t> archive;
    std::string error;
    CHECK(pack_save_folder(from, archive, &error));
    std::vector<uint8_t> again;
    CHECK(pack_save_folder(from, again, nullptr));
    CHECK(archive == again);

    // Into a folder holding something else: exactly what was packed after.
    const std::string to = path_join(sandbox, "pack-to");
    CHECK(write_text(path_join(to, "stale"), "old"));
    CHECK(unpack_save_folder(archive, to, &error));
    CHECK_STR_EQ(read_text(path_join(to, "00000001/user.dat")), "two");
    CHECK_STR_EQ(read_text(path_join(to, ".hidden")), "three");
    CHECK(directory_exists(path_join(to, "empty")));
    CHECK(!file_exists(path_join(to, "stale")));
    CHECK(!directory_exists(to + ".incoming"));
    CHECK(!directory_exists(to + ".outgoing"));
    std::vector<uint8_t> repacked;
    CHECK(pack_save_folder(to, repacked, nullptr));
    CHECK(archive == repacked);

    // Cut short, or naming a file outside the folder: refused, and the
    // folder is as it was.
    std::vector<uint8_t> short_archive(archive.begin(), archive.end() - 2);
    CHECK(!unpack_save_folder(short_archive, to, &error));
    CHECK_STR_EQ(read_text(path_join(to, ".hidden")), "three");
    std::vector<uint8_t> escaping;
    const std::string bad = path_join(sandbox, "pack-bad");
    CHECK(write_text(path_join(bad, "aaaaaa"), "x"));
    CHECK(pack_save_folder(bad, escaping, nullptr));
    const auto at = std::search(escaping.begin(), escaping.end(), std::begin("aaaaaa"), std::end("aaaaaa") - 1);
    CHECK(at != escaping.end());
    std::memcpy(&*at, "../esc", 6);
    CHECK(!unpack_save_folder(escaping, to, &error));
    CHECK(!file_exists(path_join(sandbox, "esc")));
    CHECK_STR_EQ(read_text(path_join(to, "00000001/system")), "one");

    // Nothing there packs to an archive of nothing, which unpacks to an
    // empty folder.
    std::vector<uint8_t> nothing;
    CHECK(pack_save_folder(path_join(sandbox, "pack-none"), nothing, nullptr));
    CHECK(unpack_save_folder(nothing, path_join(sandbox, "pack-none-to"), nullptr));
    CHECK(!save_folder_has_files(path_join(sandbox, "pack-none-to")));

    CHECK(remove_save_folder(to));
    CHECK(!directory_exists(to));
}

void test_pieces() {
    const uint8_t text[] = {'C', 0, 0xE9, 0, 0x3D, 0xD8, 0x00, 0xDE, 0, 0, 'X', 0};
    CHECK_STR_EQ(utf16le_to_utf8(text, 6), "C\xC3\xA9\xF0\x9F\x98\x80");
    CHECK_STR_EQ(xml_text("<a><title_id>1</title_id><title>Fish &amp; Chips &lt;3</title></a>", "title"),
                 "Fish & Chips <3");
    CHECK_STR_EQ(xml_text("<mlc_path/><mlc_path>x</mlc_path>", "mlc_path"), "");
    CHECK_STR_EQ(xml_text("<longname_en>Super Mario\n3D World</longname_en>", "longname_en"),
                 "Super Mario 3D World");

    CHECK_STR_EQ(three_ds_game_id(0x0004000000055D00ull), "CTR-0004000000055D00");
    CHECK_STR_EQ(wii_u_game_id(0x0005000010101C00ull), "WUP-0005000010101C00");
    CHECK_EQ(title_id_from_game_id("CTR-0004000000055D00"), 0x0004000000055D00ull);
    CHECK_EQ(title_id_from_game_id("AMCE-01-0-1a2b"), 0ull);
    CHECK_EQ(title_id_from_game_id("CTR-00040000000G5D00"), 0ull);

    const std::vector<uint8_t> four = {1, 2, 3, 4, 5, 6, 7, 8};
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;
    CHECK(decode_rgba_icon(encode_rgba_icon(2, 1, four), width, height, rgba));
    CHECK(width == 2 && height == 1 && rgba == four);
    CHECK(encode_rgba_icon(3, 1, four).empty());
    CHECK(save_icon_rgba(kSaveSystem3ds, encode_rgba_icon(2, 1, four), width, height, rgba));

    const std::vector<uint8_t> icon = smdh(u"Pilotwings Resort", u"Nintendo");
    CHECK(decode_smdh_icon(icon.data(), icon.size(), rgba));
    CHECK(pixel(rgba, 48, 1, 0)[0] == 255 && pixel(rgba, 48, 1, 0)[1] == 0);
    CHECK(pixel(rgba, 48, 0, 1)[1] == 255 && pixel(rgba, 48, 0, 1)[0] == 0);
    CHECK(pixel(rgba, 48, 8, 0)[2] == 255 && pixel(rgba, 48, 8, 0)[0] == 0);
    CHECK(pixel(rgba, 48, 0, 8)[0] == 255 && pixel(rgba, 48, 0, 8)[2] == 255);
    CHECK(pixel(rgba, 48, 0, 0)[0] == 0 && pixel(rgba, 48, 0, 0)[3] == 255);
}

constexpr uint64_t kPilotwings = 0x0004000000055D00ull;

void test_3ds_roms(const std::string& sandbox) {
    const std::vector<uint8_t> game = ncch(kPilotwings, smdh(u"Pilotwings Resort", u"Nintendo"));
    const std::string cartridge = path_join(sandbox, "roms3ds/Pilotwings Resort.3ds");
    CHECK(write_bytes(cartridge, ncsd(game, kPilotwings)));
    ConsoleTitleInfo info;
    std::string error;
    CHECK_MSG(read_3ds_title_info(cartridge, info, &error), error);
    CHECK_EQ(info.title_id, kPilotwings);
    CHECK_STR_EQ(info.title, "Pilotwings Resort");
    CHECK_STR_EQ(info.publisher, "Nintendo");
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;
    CHECK(decode_rgba_icon(info.icon, width, height, rgba));
    CHECK(width == 48 && height == 48 && pixel(rgba, 48, 1, 0)[0] == 255);

    const std::string executable = path_join(sandbox, "roms3ds/game.cxi");
    CHECK(write_bytes(executable, game));
    CHECK(read_3ds_title_info(executable, info, nullptr));
    CHECK_STR_EQ(info.title, "Pilotwings Resort");

    const std::string package = path_join(sandbox, "roms3ds/game.cia");
    CHECK(write_bytes(package, cia(0x0004000000030800ull, ncch(0x0004000000030800ull, smdh(u"Mario Kart 7", u"Nintendo")))));
    CHECK_MSG(read_3ds_title_info(package, info, &error), error);
    CHECK_EQ(info.title_id, 0x0004000000030800ull);
    CHECK_STR_EQ(info.title, "Mario Kart 7");

    // Encrypted, as a cartridge dumped straight is: its title ID, nothing more.
    const std::string encrypted = path_join(sandbox, "roms3ds/encrypted.3ds");
    CHECK(write_bytes(encrypted, ncsd(ncch(0x0004000000086300ull, smdh(u"Hidden", u"Nobody"), true), 0)));
    CHECK(read_3ds_title_info(encrypted, info, nullptr));
    CHECK_EQ(info.title_id, 0x0004000000086300ull);
    CHECK(info.title.empty() && info.icon.empty());

    const std::string junk = path_join(sandbox, "roms3ds/junk.3ds");
    CHECK(write_bytes(junk, std::vector<uint8_t>(0x400, 0x5A)));
    CHECK(!read_3ds_title_info(junk, info, &error));
    CHECK(!error.empty());
}

constexpr uint64_t kMario3dWorld = 0x0005000010145D00ull;
constexpr uint64_t kMarioKart8 = 0x0005000010101C00ull;
constexpr uint64_t kZelda = 0x0005000010143500ull;
constexpr uint64_t kPikmin = 0x000500001012BD00ull;

void put_be16(std::vector<uint8_t>& bytes, std::size_t at, uint32_t value) {
    bytes[at] = static_cast<uint8_t>(value >> 8);
    bytes[at + 1] = static_cast<uint8_t>(value);
}

// A Cemu .wua -- a ZArchive -- of these files ("folder/meta/meta.xml"), laid
// out as Exzap's writer does: the data in 64 KiB blocks, each compressed with
// zstd when this build has it (and it shrinks) and kept as it is otherwise;
// the block offsets, the names, the tree, the root first and every folder's
// entries together; and the footer, big-endian throughout.
std::vector<uint8_t> wua(const std::vector<std::pair<std::string, std::vector<uint8_t>>>& files) {
    struct Node {
        std::string name;
        bool file = false;
        std::vector<uint8_t> data;
        std::vector<Node> children;
    };
    Node root;
    for (const auto& [path, data] : files) {
        Node* at = &root;
        std::size_t start = 0;
        while (true) {
            const std::size_t slash = path.find('/', start);
            const std::string part = path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
            Node* next = nullptr;
            for (Node& child : at->children) {
                if (child.name == part) next = &child;
            }
            if (next == nullptr) {
                at->children.push_back(Node{part, false, {}, {}});
                next = &at->children.back();
            }
            at = next;
            if (slash == std::string::npos) break;
            start = slash + 1;
        }
        at->file = true;
        at->data = data;
    }
    // Breadth first: each folder's entries are a run of the table.
    std::vector<const Node*> order{&root};
    std::vector<std::pair<uint32_t, uint32_t>> runs;  // per node: first child, count
    for (std::size_t i = 0; i < order.size(); ++i) {
        runs.push_back({static_cast<uint32_t>(order.size()), static_cast<uint32_t>(order[i]->children.size())});
        for (const Node& child : order[i]->children) order.push_back(&child);
    }
    std::vector<uint8_t> names;
    std::vector<uint8_t> tree(order.size() * 16, 0);
    std::vector<uint8_t> data;
    for (std::size_t i = 0; i < order.size(); ++i) {
        const Node& node = *order[i];
        uint32_t name_at = 0x7FFFFFFF;
        if (i != 0) {
            name_at = static_cast<uint32_t>(names.size());
            names.push_back(static_cast<uint8_t>(node.name.size()));
            names.insert(names.end(), node.name.begin(), node.name.end());
        }
        put_be32(tree, i * 16, name_at | (node.file ? 0x80000000u : 0u));
        if (node.file) {
            put_be32(tree, i * 16 + 4, static_cast<uint32_t>(data.size()));
            put_be32(tree, i * 16 + 8, static_cast<uint32_t>(node.data.size()));
            data.insert(data.end(), node.data.begin(), node.data.end());
        } else {
            put_be32(tree, i * 16 + 4, node.children.empty() ? 0 : runs[i].first);
            put_be32(tree, i * 16 + 8, runs[i].second);
        }
    }
    constexpr std::size_t kBlock = 64 * 1024;
    data.resize((data.size() + kBlock - 1) / kBlock * kBlock, 0);
    std::vector<uint8_t> out;
    const std::size_t blocks = data.size() / kBlock;
    std::vector<uint8_t> records(((blocks + 15) / 16) * 40, 0);
    for (std::size_t block = 0; block < blocks; ++block) {
        const uint8_t* raw = data.data() + block * kBlock;
        std::vector<uint8_t> stored(raw, raw + kBlock);
#ifdef DUALSCREEN_HAVE_ZSTD
        std::vector<uint8_t> packed(ZSTD_compressBound(kBlock));
        const std::size_t size = ZSTD_compress(packed.data(), packed.size(), raw, kBlock, 3);
        if (!ZSTD_isError(size) && size < kBlock) stored.assign(packed.begin(), packed.begin() + size);
#endif
        if (block % 16 == 0) put_be64(records, block / 16 * 40, out.size());
        put_be16(records, block / 16 * 40 + 8 + (block % 16) * 2, static_cast<uint32_t>(stored.size() - 1));
        out.insert(out.end(), stored.begin(), stored.end());
    }
    const uint64_t data_size = out.size();
    const uint64_t records_at = out.size();
    out.insert(out.end(), records.begin(), records.end());
    const uint64_t names_at = out.size();
    out.insert(out.end(), names.begin(), names.end());
    const uint64_t tree_at = out.size();
    out.insert(out.end(), tree.begin(), tree.end());
    const uint64_t end = out.size();
    std::vector<uint8_t> footer(16 * 6 + 32 + 8 + 4 + 4, 0);
    const uint64_t sections[6][2] = {{0, data_size},          {records_at, records.size()},
                                     {names_at, names.size()}, {tree_at, tree.size()},
                                     {end, 0},                 {end, 0}};
    for (int i = 0; i < 6; ++i) {
        put_be64(footer, i * 16, sections[i][0]);
        put_be64(footer, i * 16 + 8, sections[i][1]);
    }
    put_be64(footer, 16 * 6 + 32, end + footer.size());
    put_be32(footer, 16 * 6 + 40, 0x61bf3a01u);
    put_be32(footer, 16 * 6 + 44, 0x169f52d6u);
    out.insert(out.end(), footer.begin(), footer.end());
    return out;
}

std::vector<uint8_t> bytes_of(const std::string& text) { return std::vector<uint8_t>(text.begin(), text.end()); }

std::string cemu_title(uint64_t title_id, const std::string& name, int format, const std::string& path,
                       const std::string& sub_path = std::string()) {
    char id[17];
    std::snprintf(id, sizeof(id), "%016llx", static_cast<unsigned long long>(title_id));
    return std::string("\t<title titleId=\"") + id +
           "\" version=\"0\" sdk_version=\"0\" group_id=\"0\" app_type=\"80000000\">\n"
           "\t\t<region>2</region>\n\t\t<name>" + name + "</name>\n\t\t<format>" + std::to_string(format) +
           "</format>\n\t\t<path>" + path + "</path>\n" +
           (sub_path.empty() ? std::string() : "\t\t<sub_path>" + sub_path + "</sub_path>\n") + "\t</title>\n";
}

void write_wii_u_game(const std::string& game) {
    CHECK(write_text(path_join(game, "code/RedPro.rpx"), "rpx"));
    CHECK(write_text(path_join(game, "meta/meta.xml"),
                     meta_xml("0005000010145D00", "Super Mario\n3D World", "Nintendo")));
    const uint8_t red[4] = {255, 0, 0, 255};
    CHECK(write_bytes(path_join(game, "meta/iconTex.tga"), tga(128, 128, red)));
}

void test_wii_u_meta(const std::string& sandbox) {
    const std::string game = path_join(sandbox, "wiiu/Super Mario 3D World");
    write_wii_u_game(game);
    ConsoleTitleInfo info;
    std::string error;
    CHECK_MSG(read_wii_u_title_info(path_join(game, "code/RedPro.rpx"), info, &error), error);
    CHECK_EQ(info.title_id, kMario3dWorld);
    CHECK_STR_EQ(info.title, "Super Mario 3D World");
    CHECK_STR_EQ(info.publisher, "Nintendo");
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;
    CHECK(decode_rgba_icon(info.icon, width, height, rgba));
    CHECK(width == 64 && height == 64);
    // Shrunk: the black corner pixel is a quarter of the top-left one.
    CHECK(pixel(rgba, 64, 0, 0)[0] == 191 && pixel(rgba, 64, 1, 0)[0] == 255 && pixel(rgba, 64, 63, 63)[0] == 255);
    CHECK(!read_wii_u_title_info(path_join(sandbox, "wiiu/game.wud"), info, nullptr));
}

std::string sdmc_save(const std::string& sdmc, uint64_t title_id) {
    char tail[64];
    std::snprintf(tail, sizeof(tail), "title/%08x/%08x/data", static_cast<unsigned>(title_id >> 32),
                  static_cast<unsigned>(title_id));
    return path_join(path_join(path_join(sdmc, "Nintendo 3DS/00000000000000000000000000000000"),
                               "00000000000000000000000000000000"),
                     tail);
}

void test_3ds_store(const std::string& sandbox) {
    const std::string saves = path_join(sandbox, "saves3ds");
    const std::string roms = path_join(sandbox, "roms3ds");
    CHECK(ensure_directory(saves));
    const std::string folder = sdmc_save(path_join(saves, "Azahar/sdmc"), kPilotwings);
    CHECK(write_text(path_join(folder, "00000001/save00.bin"), "first"));

    SaveStore store(saves, "Deck");
    store.set_rom_dirs({roms});
    CHECK_STR_EQ(store.save_folder(kSaveSystem3ds, kPilotwings), folder);
    std::vector<SaveInfo> listed = store.list();
    CHECK_EQ(listed.size(), static_cast<std::size_t>(1));
    if (listed.size() != 1) return;
    CHECK_STR_EQ(listed[0].system, kSaveSystem3ds);
    CHECK_STR_EQ(listed[0].game_id, "CTR-0004000000055D00");
    CHECK_STR_EQ(listed[0].title, "Pilotwings Resort");
    CHECK(listed[0].has_save);
    CHECK_EQ(listed[0].history.size(), static_cast<std::size_t>(1));
    CHECK_EQ(listed[0].size, static_cast<uint64_t>(5));

    // Played in Azahar on its own: a new version, seen the next time.
    CHECK(write_text(path_join(folder, "00000001/save00.bin"), "second"));
    touch_later(path_join(folder, "00000001/save00.bin"), listed[0].saved_at + 60);
    listed = store.list();
    CHECK_EQ(listed[0].history.size(), static_cast<std::size_t>(2));
    listed = store.list();
    CHECK_EQ(listed[0].history.size(), static_cast<std::size_t>(2));

    // Started from here: the ROM names it, and its save is what is noted.
    const std::string rom = path_join(roms, "Pilotwings Resort.3ds");
    SaveInfo found;
    CHECK(store.find_for_rom(rom, found));
    CHECK_STR_EQ(found.game_id, "CTR-0004000000055D00");
    store.note_game_started(rom);
    CHECK_STR_EQ(store.game_in_play(), "CTR-0004000000055D00");
    CHECK(write_text(path_join(folder, "00000001/save00.bin"), "third"));
    store.note_game_stopped(rom);
    CHECK(store.game_in_play().empty());
    CHECK(store.find("CTR-0004000000055D00", found));
    CHECK_EQ(found.history.size(), static_cast<std::size_t>(3));

    // Deleted, then put back from Earlier Saves.
    std::string error;
    CHECK_MSG(store.remove("CTR-0004000000055D00", &error), error);
    CHECK(!save_folder_has_files(folder));
    const std::vector<EarlierSave> earlier = store.earlier("CTR-0004000000055D00");
    CHECK(!earlier.empty());
    if (earlier.empty()) return;
    CHECK_MSG(store.restore("CTR-0004000000055D00", earlier.front().id, &error), error);
    CHECK_STR_EQ(read_text(path_join(folder, "00000001/save00.bin")), "third");

    // Sent to a device that has never seen the game: into its own Azahar
    // folder, with the same versions.
    SaveInfo sending;
    std::vector<uint8_t> bytes;
    CHECK(store.read_for_sending("CTR-0004000000055D00", sending, bytes, &error));
    const std::string other_saves = path_join(sandbox, "saves3ds-other");
    CHECK(ensure_directory(other_saves));
    SaveStore other(other_saves, "Mac");
    CHECK_MSG(other.accept(sending, bytes, "Deck", &error), error);
    const std::string other_folder = sdmc_save(path_join(other_saves, "Azahar/sdmc"), kPilotwings);
    CHECK_STR_EQ(read_text(path_join(other_folder, "00000001/save00.bin")), "third");
    SaveInfo received;
    CHECK(other.find("CTR-0004000000055D00", received));
    CHECK_STR_EQ(received.title, "Pilotwings Resort");
    CHECK(compare_saves(version_state(received), version_state(sending)) == SaveComparison::UpToDate);
}

void test_wii_u_store(const std::string& sandbox) {
    const std::string saves = path_join(sandbox, "saveswiiu");
    const std::string mlc = path_join(sandbox, "mlc01");
    CHECK(ensure_directory(saves));
    write_wii_u_game(path_join(mlc, "usr/title/00050000/10145d00"));
    const std::string folder = path_join(mlc, "usr/save/00050000/10145d00");
    CHECK(write_text(path_join(folder, "user/80000001/game.dat"), "cat suit"));
    // A title with the folders made and nothing saved yet is not listed.
    CHECK(make_directories(path_join(mlc, "usr/save/00050000/10101c00/user/80000001")));

    SaveStore store(saves, "Deck");
    CHECK(store.list().empty());
    FolderSaveRoots roots = store.folder_roots();
    roots.wii_u_mlc = mlc;
    store.set_folder_roots(roots);
    std::vector<SaveInfo> listed = store.list();
    CHECK_EQ(listed.size(), static_cast<std::size_t>(1));
    if (listed.size() != 1) return;
    CHECK_STR_EQ(listed[0].system, kSaveSystemWiiU);
    CHECK_STR_EQ(listed[0].game_id, "WUP-0005000010145D00");
    CHECK_STR_EQ(listed[0].title, "Super Mario 3D World");
    CHECK_STR_EQ(listed[0].publisher, "Nintendo");
    CHECK(!listed[0].icon.empty());

    // Played through Cemu from here, from the game's folder.
    const std::string rpx = path_join(mlc, "usr/title/00050000/10145d00/code/RedPro.rpx");
    store.note_game_started(rpx);
    CHECK_STR_EQ(store.game_in_play(), "WUP-0005000010145D00");
    CHECK(write_text(path_join(folder, "user/80000001/game.dat"), "tanooki"));
    store.note_game_stopped(rpx);
    SaveInfo found;
    CHECK(store.find("WUP-0005000010145D00", found));
    CHECK_EQ(found.history.size(), static_cast<std::size_t>(2));

    // Nowhere to put one without Cemu.
    SaveInfo sending;
    std::vector<uint8_t> bytes;
    std::string error;
    CHECK(store.read_for_sending("WUP-0005000010145D00", sending, bytes, &error));
    const std::string other_saves = path_join(sandbox, "saveswiiu-other");
    CHECK(ensure_directory(other_saves));
    SaveStore other(other_saves, "Switch");
    CHECK(!other.accept(sending, bytes, "Deck", &error));
    CHECK(!error.empty());
}

// A Wii U game kept as Cemu's single-file archive says what it is too, from
// the meta folder of its own title inside, or of its update's.
void test_wii_u_archive(const std::string& sandbox) {
    const uint8_t blue[4] = {0, 0, 255, 255};
    // Big enough to run past the first 64 KiB block.
    const std::vector<uint8_t> icon = tga(128, 128, blue);
    const std::string path = path_join(sandbox, "wiiu/Mario Kart 8.wua");
    CHECK(write_bytes(path, wua({{"0005000010101c00_v0/code/Turbo.rpx", bytes_of("rpx")},
                                 {"0005000010101c00_v0/meta/meta.xml",
                                  bytes_of(meta_xml("0005000010101C00", "Mario Kart 8", "Nintendo"))},
                                 {"0005000010101c00_v0/meta/iconTex.tga", icon},
                                 {"0005000e10101c00_v208/meta/meta.xml",
                                  bytes_of(meta_xml("0005000E10101C00", "Mario Kart 8 (update)", "Nintendo"))}})));
    ConsoleTitleInfo info;
    std::string error;
    CHECK_MSG(read_wii_u_archive_title_info(path, "0005000010101c00_v0", kMarioKart8, info, &error), error);
    CHECK_STR_EQ(info.title, "Mario Kart 8");
    CHECK_STR_EQ(info.publisher, "Nintendo");
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;
    CHECK(decode_rgba_icon(info.icon, width, height, rgba));
    CHECK_EQ(width, 64);
    if (!rgba.empty()) CHECK_EQ(int(pixel(rgba, width, 32, 32)[2]), 255);
    // Found by title ID without the folder's name; the update's for a title
    // whose own folder is not there.
    CHECK_MSG(read_wii_u_archive_title_info(path, "", kMarioKart8, info, &error), error);
    CHECK_STR_EQ(info.title, "Mario Kart 8");
    // An archive of only the update, for the game.
    const std::string update = path_join(sandbox, "wiiu/Mario Kart 8 update.wua");
    CHECK(write_bytes(update, wua({{"0005000e10101c00_v208/meta/meta.xml",
                                   bytes_of(meta_xml("0005000E10101C00", "Mario Kart 8 (update)", "Nintendo"))}})));
    CHECK_MSG(read_wii_u_archive_title_info(update, "missing_v0", kMarioKart8, info, &error), error);
    CHECK_STR_EQ(info.title, "Mario Kart 8 (update)");
    // Not a game in it, and not an archive.
    CHECK(!read_wii_u_archive_title_info(path, "", kZelda, info, &error));
    CHECK(!error.empty());
    CHECK(write_text(path_join(sandbox, "wiiu/junk.wua"), std::string(300, 'x')));
    CHECK(!read_wii_u_archive_title_info(path_join(sandbox, "wiiu/junk.wua"), "", kMarioKart8, info, nullptr));
}

// Wii U games played from somewhere other than Cemu's mlc01 -- a folder, a
// .wua, a disc image -- named in Data Management from Cemu's own game list.
void test_wii_u_names_from_cemu(const std::string& sandbox) {
    const std::string mlc = path_join(sandbox, "cemu-mlc/mlc01");
    for (const uint64_t id : {kMario3dWorld, kMarioKart8, kZelda, kPikmin}) {
        char low[9];
        std::snprintf(low, sizeof(low), "%08x", static_cast<unsigned>(id & 0xFFFFFFFFu));
        CHECK(write_text(path_join(mlc, std::string("usr/save/00050000/") + low + "/user/80000001/save.dat"), "x"));
    }
    // Zelda: only its update installed in mlc01, which says the same.
    CHECK(write_text(path_join(mlc, "usr/title/0005000e/10143500/meta/meta.xml"),
                     meta_xml("0005000E10143500", "Breath of the Wild", "Nintendo")));
    const std::string folder_game = path_join(sandbox, "cemu-games/Super Mario 3D World");
    write_wii_u_game(folder_game);
    const std::string archive = path_join(sandbox, "cemu-games/Mario Kart 8.wua");
    const uint8_t green[4] = {0, 255, 0, 255};
    CHECK(write_bytes(archive, wua({{"0005000010101c00_v0/meta/meta.xml",
                                     bytes_of(meta_xml("0005000010101C00", "Mario Kart 8", "Nintendo"))},
                                    {"0005000010101c00_v0/meta/iconTex.tga", tga(128, 128, green)}})));

    // First without Cemu's game list: titles only.
    const std::string saves = path_join(sandbox, "saves-cemu");
    CHECK(ensure_directory(saves));
    {
        SaveStore store(saves, "Deck");
        FolderSaveRoots roots = store.folder_roots();
        roots.wii_u_mlc = mlc;
        store.set_folder_roots(roots);
        std::vector<SaveInfo> listed = store.list();
        CHECK_EQ(listed.size(), static_cast<std::size_t>(4));
        for (const SaveInfo& info : listed) {
            if (info.game_id == wii_u_game_id(kZelda)) {
                CHECK_STR_EQ(info.title, "Breath of the Wild");
            } else {
                CHECK_MSG(info.title.empty(), info.game_id + " " + info.title);
            }
        }
    }

    // Cemu has them in its game list: the next run names them -- from the
    // folder's meta, from inside the archive, and for a disc image only the
    // name the list kept.
    const std::string list = path_join(sandbox, "cemu-mlc/title_list_cache.xml");
    CHECK(write_text(list, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<title_list>\n" +
                               cemu_title(kMario3dWorld, "Super Mario 3D World", kCemuTitleFolder, folder_game) +
                               cemu_title(kMarioKart8, "Mario Kart 8", kCemuTitleArchive, archive,
                                          "0005000010101c00_v0") +
                               cemu_title(kPikmin, "Pikmin 3 &amp; More", 2, path_join(sandbox, "cemu-games/p.wud")) +
                               "</title_list>\n"));
    const std::vector<CemuTitleListEntry> entries = read_cemu_title_list(list);
    CHECK_EQ(entries.size(), static_cast<std::size_t>(3));
    if (entries.size() == 3) {
        CHECK_EQ(entries[1].title_id, kMarioKart8);
        CHECK_EQ(entries[1].format, kCemuTitleArchive);
        CHECK_STR_EQ(entries[1].sub_path, "0005000010101c00_v0");
        CHECK_STR_EQ(entries[2].name, "Pikmin 3 & More");
    }
    std::vector<std::string> lists = find_cemu_title_lists("", "", mlc);
    CHECK_EQ(lists.size(), static_cast<std::size_t>(1));
    {
        SaveStore store(saves, "Deck");
        FolderSaveRoots roots = store.folder_roots();
        roots.wii_u_mlc = mlc;
        roots.wii_u_title_lists = lists;
        store.set_folder_roots(roots);
        for (const SaveInfo& info : store.list()) {
            if (info.game_id == wii_u_game_id(kMario3dWorld)) {
                CHECK_STR_EQ(info.title, "Super Mario 3D World");
                CHECK(!info.icon.empty());
            } else if (info.game_id == wii_u_game_id(kMarioKart8)) {
                CHECK_STR_EQ(info.title, "Mario Kart 8");
                CHECK_STR_EQ(info.publisher, "Nintendo");
#ifdef DUALSCREEN_HAVE_ZSTD
                CHECK(!info.icon.empty());
#endif
            } else if (info.game_id == wii_u_game_id(kPikmin)) {
                CHECK_STR_EQ(info.title, "Pikmin 3 & More");
                CHECK(info.icon.empty());
            }
        }
    }

    // Where Cemu keeps the list: its user-data folder, the one its own
    // mlc01 goes in, or beside a portable Cemu.
    const std::string home = path_join(sandbox, "cemu-home");
    CHECK(find_cemu_title_lists("", home, "").empty());
    CHECK(write_text(path_join(home, ".local/share/Cemu/title_list_cache.xml"), "<title_list/>"));
    lists = find_cemu_title_lists("cemu", home, mlc);
    CHECK_EQ(lists.size(), static_cast<std::size_t>(2));
    if (lists.size() == 2) {
        CHECK_STR_EQ(lists[0], list);
        CHECK_STR_EQ(lists[1], path_join(home, ".local/share/Cemu/title_list_cache.xml"));
    }
}

void test_find_cemu_mlc(const std::string& sandbox) {
    const std::string home = path_join(sandbox, "home");
    CHECK_STR_EQ(find_cemu_mlc("/somewhere/mlc01", "", home), "/somewhere/mlc01");
    CHECK(find_cemu_mlc("", "", home).empty());

    // Its usual place.
    const std::string usual = path_join(home, ".local/share/Cemu/mlc01");
    CHECK(make_directories(usual));
    CHECK_STR_EQ(find_cemu_mlc("", "cemu", home), usual);

    // Where its settings say, over that.
    const std::string elsewhere = path_join(sandbox, "games/mlc");
    CHECK(make_directories(elsewhere));
    CHECK(write_text(path_join(home, ".config/Cemu/settings.xml"),
                     "<content><mlc_path>" + elsewhere + "</mlc_path></content>"));
    CHECK_STR_EQ(find_cemu_mlc("", "cemu", home), elsewhere);

    // A portable Cemu beside the program, over both.
    const std::string program = path_join(sandbox, "apps/Cemu/Cemu");
    CHECK(write_text(path_join(sandbox, "apps/Cemu/portable/settings.xml"), "<content><mlc_path/></content>"));
    CHECK(make_directories(path_join(sandbox, "apps/Cemu/portable/mlc01")));
    CHECK_STR_EQ(find_cemu_mlc("", program, home), path_join(sandbox, "apps/Cemu/portable/mlc01"));
}

}  // namespace

int main() {
    const std::string sandbox = make_sandbox();
    CHECK(!sandbox.empty());
    test_pack_and_unpack(sandbox);
    test_pieces();
    test_3ds_roms(sandbox);
    test_wii_u_meta(sandbox);
    test_3ds_store(sandbox);
    test_wii_u_store(sandbox);
    test_find_cemu_mlc(sandbox);
    test_wii_u_archive(sandbox);
    test_wii_u_names_from_cemu(sandbox);
    return test::finish("folder saves");
}
