// Reading a game's real name out of the file the player loaded.
//
// Tested against built-by-hand containers rather than real ROMs, because the
// part that can be wrong is arithmetic on offsets and nothing else: a field
// read one step out comes back as plausible text, not as a failure, so the
// only way to know it is right is to lay the bytes out deliberately and check
// what comes back.

#include "test_support.hpp"

#include <cstring>
#include <string>
#include <vector>

#include <unistd.h>

#include "config/paths.hpp"
#include "core/game_title.hpp"

using namespace ds;

namespace {

void put_u16(std::vector<uint8_t>& bytes, std::size_t at, uint32_t value) {
    if (bytes.size() < at + 2) bytes.resize(at + 2, 0);
    bytes[at] = static_cast<uint8_t>(value & 0xFF);
    bytes[at + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
}

void put_u32(std::vector<uint8_t>& bytes, std::size_t at, uint32_t value) {
    if (bytes.size() < at + 4) bytes.resize(at + 4, 0);
    for (int byte = 0; byte < 4; ++byte) {
        bytes[at + byte] = static_cast<uint8_t>((value >> (byte * 8)) & 0xFF);
    }
}

void put_text(std::vector<uint8_t>& bytes, std::size_t at, const char* ascii) {
    const std::size_t length = std::strlen(ascii);
    if (bytes.size() < at + length) bytes.resize(at + length, 0);
    std::memcpy(bytes.data() + at, ascii, length);
}

// Both consoles store titles as UTF-16, so every fixture writes them that way.
void put_utf16(std::vector<uint8_t>& bytes, std::size_t at, const std::u16string& text) {
    if (bytes.size() < at + (text.size() + 1) * 2) bytes.resize(at + (text.size() + 1) * 2, 0);
    for (std::size_t index = 0; index < text.size(); ++index) {
        put_u16(bytes, at + index * 2, text[index]);
    }
}

constexpr std::size_t kBannerAt = 0x4000;
constexpr std::size_t kBannerTitlesAt = 0x240;
constexpr std::size_t kEnglishTitleAt = kBannerAt + kBannerTitlesAt + 128 * 2;

std::vector<uint8_t> nds_rom(const std::u16string& banner_title) {
    std::vector<uint8_t> bytes(kBannerAt + 0x1000, 0);
    put_text(bytes, 0x000, "SOMEGAME");  // the header's short title, deliberately ignored
    put_u32(bytes, 0x068, kBannerAt);
    put_utf16(bytes, kEnglishTitleAt, banner_title);
    return bytes;
}

std::vector<uint8_t> smdh_blob(const std::u16string& english_short) {
    std::vector<uint8_t> bytes(0x36C0, 0);
    put_text(bytes, 0, "SMDH");
    // Japanese first, English second: writing something into Japanese as well
    // is what catches reading the wrong entry.
    put_utf16(bytes, 0x08, u"Japanese Name");
    put_utf16(bytes, 0x08 + 0x200, english_short);
    return bytes;
}

void test_a_ds_rom_gives_up_its_banner_name() {
    // Three lines, the last of them the publisher.
    const std::vector<uint8_t> rom = nds_rom(u"The Legend of Zelda\nPhantom Hourglass\nNintendo");
    CHECK_STR_EQ(nds_title_from_rom(rom.data(), rom.size()),
             std::string("The Legend of Zelda Phantom Hourglass"));

    // Two lines: title then publisher, which is the commonest shape.
    const std::vector<uint8_t> kart = nds_rom(u"Mario Kart DS\nNintendo");
    CHECK_STR_EQ(nds_title_from_rom(kart.data(), kart.size()), std::string("Mario Kart DS"));

    // One line is all title -- there is no publisher to drop, and dropping the
    // only line would leave nothing.
    const std::vector<uint8_t> alone = nds_rom(u"Homebrew Thing");
    CHECK_STR_EQ(nds_title_from_rom(alone.data(), alone.size()), std::string("Homebrew Thing"));

    // Not ASCII, which is the whole reason the field is UTF-16.
    const std::vector<uint8_t> accents = nds_rom(u"Pokémon Perle\nNintendo");
    CHECK_STR_EQ(nds_title_from_rom(accents.data(), accents.size()), std::string("Pokémon Perle"));
}

void test_a_ds_rom_with_no_banner_says_so_rather_than_guessing() {
    std::vector<uint8_t> rom = nds_rom(u"Whatever");
    put_u32(rom, 0x068, 0);  // homebrew with no banner at all
    CHECK(nds_title_from_rom(rom.data(), rom.size()).empty());

    // A banner pointer past the end of the file: a truncated download, or a
    // file that is not a DS ROM at all. Must not read off the end.
    std::vector<uint8_t> truncated = nds_rom(u"Whatever");
    put_u32(truncated, 0x068, 0x7FFFFF00);
    CHECK(nds_title_from_rom(truncated.data(), truncated.size()).empty());

    CHECK(nds_title_from_rom(nullptr, 0).empty());
    const std::vector<uint8_t> tiny(4, 0);
    CHECK(nds_title_from_rom(tiny.data(), tiny.size()).empty());
}

void test_an_smdh_gives_the_english_name_and_not_the_first_one() {
    const std::vector<uint8_t> smdh = smdh_blob(u"Super Mario 3D Land");
    CHECK_STR_EQ(smdh_title(smdh.data(), smdh.size()), std::string("Super Mario 3D Land"));

    // Anything that is not one is not one.
    std::vector<uint8_t> wrong = smdh;
    put_text(wrong, 0, "XXXX");
    CHECK(smdh_title(wrong.data(), wrong.size()).empty());
}

void test_a_3ds_card_image_is_walked_rather_than_searched() {
    // NCSD header, one partition, whose NCCH holds an ExeFS with an "icon".
    constexpr std::size_t kMediaUnit = 0x200;
    constexpr std::size_t kNcchUnits = 0x10;   // partition starts 0x2000 in
    constexpr std::size_t kExeFsUnits = 0x08;  // ExeFS 0x1000 into the NCCH
    constexpr std::size_t kNcchAt = kNcchUnits * kMediaUnit;
    constexpr std::size_t kExeFsAt = kNcchAt + kExeFsUnits * kMediaUnit;
    constexpr std::size_t kIconAt = 0x400;  // within the ExeFS's file data

    std::vector<uint8_t> rom(kExeFsAt + 0x200 + kIconAt + 0x4000, 0);
    put_text(rom, 0x100, "NCSD");
    put_u32(rom, 0x120, kNcchUnits);
    put_text(rom, kNcchAt + 0x100, "NCCH");
    put_u32(rom, kNcchAt + 0x1A0, kExeFsUnits);
    put_u32(rom, kNcchAt + 0x1A4, 0x40);
    // ExeFS header: the icon is the second entry, so a reader that only looks
    // at the first will miss it.
    put_text(rom, kExeFsAt + 0x00, ".code");
    put_u32(rom, kExeFsAt + 0x08, 0);
    put_u32(rom, kExeFsAt + 0x0C, kIconAt);
    put_text(rom, kExeFsAt + 0x10, "icon");
    put_u32(rom, kExeFsAt + 0x18, kIconAt);
    put_u32(rom, kExeFsAt + 0x1C, 0x36C0);

    const std::vector<uint8_t> smdh = smdh_blob(u"Ocarina of Time 3D");
    std::memcpy(rom.data() + kExeFsAt + 0x200 + kIconAt, smdh.data(), smdh.size());

    CHECK_STR_EQ(three_ds_title_from_rom(rom.data(), rom.size()),
             std::string("Ocarina of Time 3D"));

    // An encrypted card image has an ExeFS header that is noise, so no entry
    // is named "icon" and there is nothing to read. Empty, not nonsense --
    // the caller falls back to the file name, which is the honest answer when
    // the title is behind a key this app does not have.
    std::vector<uint8_t> encrypted = rom;
    put_text(encrypted, kExeFsAt + 0x10, "\x9f\x3a\xd1\x07");
    CHECK(three_ds_title_from_rom(encrypted.data(), encrypted.size()).empty());
}

void test_a_cia_keeps_its_name_in_the_meta_region_at_the_end() {
    // Every section is padded to 64 bytes before the next one starts, which is
    // the arithmetic worth pinning: sizes that are not already multiples of 64
    // are what a reader gets wrong.
    constexpr uint32_t kHeader = 0x2020;
    constexpr uint32_t kCert = 0xA00;
    constexpr uint32_t kTicket = 0x350;   // not a multiple of 64
    constexpr uint32_t kTmd = 0xB34;      // nor this
    constexpr uint32_t kContent = 0x1234; // nor this
    const auto align = [](std::size_t value) { return (value + 63) / 64 * 64; };
    const std::size_t meta_at =
        align(kHeader) + align(kCert) + align(kTicket) + align(kTmd) + align(kContent);

    std::vector<uint8_t> cia(meta_at + 0x3AC0, 0);
    put_u32(cia, 0x00, kHeader);
    put_u32(cia, 0x08, kCert);
    put_u32(cia, 0x0C, kTicket);
    put_u32(cia, 0x10, kTmd);
    put_u32(cia, 0x14, 0x3AC0);
    put_u32(cia, 0x18, kContent);
    put_u32(cia, 0x1C, 0);

    const std::vector<uint8_t> smdh = smdh_blob(u"Fire Emblem Awakening");
    std::memcpy(cia.data() + meta_at + 0x400, smdh.data(), smdh.size());

    CHECK_STR_EQ(three_ds_title_from_rom(cia.data(), cia.size()),
             std::string("Fire Emblem Awakening"));

    // Built without a meta region, which is legal and common for homebrew.
    std::vector<uint8_t> bare = cia;
    put_u32(bare, 0x14, 0);
    CHECK(three_ds_title_from_rom(bare.data(), bare.size()).empty());
}

void test_a_wii_u_meta_xml_gives_the_name_cemu_shows() {
    const std::string xml =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        "<menu>\n"
        "  <longname_ja type=\"complex\" length=\"512\">Japanese</longname_ja>\n"
        "  <longname_en type=\"complex\" length=\"512\">The Legend of Zelda&#10;"
        "The Wind Waker HD</longname_en>\n"
        "  <shortname_en type=\"complex\" length=\"64\">Wind Waker HD</shortname_en>\n"
        "</menu>\n";
    // The entity is left as written; what matters is that the element found is
    // the English one and not the Japanese one above it.
    CHECK(wii_u_title_from_meta_xml(xml).find("The Legend of Zelda") != std::string::npos);
    CHECK(wii_u_title_from_meta_xml(xml).find("Japanese") == std::string::npos);

    // A real two-line name, which is how the Wii U menu stores them.
    const std::string wrapped =
        "<menu><longname_en type=\"complex\">Super Mario\n3D World</longname_en></menu>";
    CHECK_STR_EQ(wii_u_title_from_meta_xml(wrapped), std::string("Super Mario 3D World"));

    // Ampersands and the like come through as themselves.
    const std::string escaped =
        "<menu><longname_en>Jett Tailfin &amp; Friends</longname_en></menu>";
    CHECK_STR_EQ(wii_u_title_from_meta_xml(escaped), std::string("Jett Tailfin & Friends"));

    CHECK(wii_u_title_from_meta_xml("<menu></menu>").empty());
    CHECK(wii_u_title_from_meta_xml("").empty());
}

// And the half that is not arithmetic: finding the file at all. A Wii U's
// title is not inside what the player picked, so this is the only part of the
// feature that can be right about the bytes and still wrong in practice.
void test_a_wii_u_game_is_found_from_the_file_the_player_actually_picks() {
    char pattern[] = "/tmp/dualscreen-title-XXXXXX";
    const char* root = mkdtemp(pattern);
    CHECK(root != nullptr);
    if (root == nullptr) return;

    const std::string game = std::string(root) + "/Some Game Folder";
    CHECK(ensure_directory(game + "/code"));
    CHECK(ensure_directory(game + "/meta"));
    const std::string xml =
        "<menu><longname_en type=\"complex\">Captain Toad\nTreasure Tracker</longname_en></menu>";
    CHECK(write_file_atomically(game + "/meta/meta.xml", xml.data(), xml.size(), nullptr));
    const std::string rpx = "not really an executable";
    CHECK(write_file_atomically(game + "/code/Whatever.rpx", rpx.data(), rpx.size(), nullptr));

    // What Cemu is actually pointed at is the .rpx, two directories away from
    // the name.
    CHECK_STR_EQ(game_title_for_file(game + "/code/Whatever.rpx"),
                 std::string("Captain Toad Treasure Tracker"));
    // And the game's folder, which is the other way people refer to one.
    CHECK_STR_EQ(game_title_for_file(game), std::string("Captain Toad Treasure Tracker"));

    // A DS ROM, read out of the file itself, through the same entry point.
    const std::vector<uint8_t> rom = nds_rom(u"Elite Beat Agents\nNintendo");
    const std::string nds = std::string(root) + "/eba.nds";
    CHECK(write_file_atomically(nds, rom.data(), rom.size(), nullptr));
    CHECK_STR_EQ(game_title_for_file(nds), std::string("Elite Beat Agents"));

    // The same names over the lines their consoles' menus give them, for
    // Discord: two for the Wii U's, and the DS's one, its publisher dropped.
    const std::vector<std::string> toad = game_title_lines_for_file(game + "/code/Whatever.rpx");
    CHECK(toad.size() == 2 && toad[0] == "Captain Toad" && toad[1] == "Treasure Tracker");
    const std::vector<std::string> eba = game_title_lines_for_file(nds);
    CHECK(eba.size() == 1 && eba[0] == "Elite Beat Agents");
    const std::vector<uint8_t> zelda = nds_rom(u"The Legend of Zelda\nSpirit Tracks\nNintendo");
    const std::string tracks = std::string(root) + "/st.nds";
    CHECK(write_file_atomically(tracks, zelda.data(), zelda.size(), nullptr));
    CHECK(game_title_lines_for_file(tracks).size() == 2);
    CHECK_STR_EQ(game_title_for_file(tracks), std::string("The Legend of Zelda Spirit Tracks"));

    // Nothing to read is not an error, it is the file name's turn. The caller
    // falls back, so this must be empty rather than a guess.
    const std::string mystery = std::string(root) + "/mystery.rpx";
    CHECK(write_file_atomically(mystery, rpx.data(), rpx.size(), nullptr));
    CHECK(game_title_for_file(mystery).empty());
    CHECK(game_title_for_file(std::string(root) + "/nothing-here.nds").empty());
}

}  // namespace

int main() {
    test_a_ds_rom_gives_up_its_banner_name();
    test_a_ds_rom_with_no_banner_says_so_rather_than_guessing();
    test_an_smdh_gives_the_english_name_and_not_the_first_one();
    test_a_3ds_card_image_is_walked_rather_than_searched();
    test_a_cia_keeps_its_name_in_the_meta_region_at_the_end();
    test_a_wii_u_meta_xml_gives_the_name_cemu_shows();
    test_a_wii_u_game_is_found_from_the_file_the_player_actually_picks();
    return test::finish("game title");
}
