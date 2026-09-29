// Data Management's ground: what a DS ROM says about itself, how two devices'
// saves for one game stand against each other, and what happens to a save
// that is replaced, deleted or put back.
//
// ROMs are laid out by hand, like test_game_title's, since what can go wrong
// is an offset read one step out.

#include "test_support.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <stdlib.h>

#include "config/paths.hpp"
#include "core/ds_rom_info.hpp"
#include "core/save_store.hpp"

using namespace ds;

namespace {

std::string make_sandbox() {
    char pattern[] = "/tmp/dualscreen-savestore-XXXXXX";
    const char* directory = mkdtemp(pattern);
    return directory != nullptr ? std::string(directory) : std::string();
}

void put_u16(std::vector<uint8_t>& bytes, std::size_t at, uint32_t value) {
    if (bytes.size() < at + 2) bytes.resize(at + 2, 0);
    bytes[at] = static_cast<uint8_t>(value & 0xFF);
    bytes[at + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
}

void put_u32(std::vector<uint8_t>& bytes, std::size_t at, uint32_t value) {
    put_u16(bytes, at, value & 0xFFFF);
    put_u16(bytes, at + 2, value >> 16);
}

void put_text(std::vector<uint8_t>& bytes, std::size_t at, const char* ascii) {
    const std::size_t length = std::strlen(ascii);
    if (bytes.size() < at + length) bytes.resize(at + length, 0);
    std::memcpy(bytes.data() + at, ascii, length);
}

void put_utf16(std::vector<uint8_t>& bytes, std::size_t at, const std::u16string& text) {
    for (std::size_t index = 0; index < text.size(); ++index) {
        put_u16(bytes, at + index * 2, text[index]);
    }
}

constexpr std::size_t kBannerAt = 0x4000;

// A DS ROM for `code`, whose English banner title is `title` (lines split by
// '\n', as a real banner's are) and whose icon is all colour 1, pure red, but
// for its top-left pixel, colour 2, pure blue.
std::vector<uint8_t> ds_rom(const char* code, const std::u16string& title, int version = 0,
                            uint16_t crc = 0x1a2b) {
    std::vector<uint8_t> bytes(kBannerAt + 0x1000, 0);
    put_text(bytes, 0x000, "HEADERNAME");
    put_text(bytes, 0x00C, code);
    put_text(bytes, 0x010, "01");
    bytes[0x01E] = static_cast<uint8_t>(version);
    put_u32(bytes, 0x068, kBannerAt);
    put_u16(bytes, 0x15E, crc);
    for (std::size_t at = 0; at < 512; ++at) bytes[kBannerAt + 0x20 + at] = 0x11;
    bytes[kBannerAt + 0x20] = 0x12;  // first tile, first byte: left pixel 2, right pixel 1
    put_u16(bytes, kBannerAt + 0x220 + 2, 0x001F);       // colour 1: red
    put_u16(bytes, kBannerAt + 0x220 + 4, 0x1F << 10);   // colour 2: blue
    put_utf16(bytes, kBannerAt + 0x240 + 256, title);
    return bytes;
}

bool write_bytes(const std::string& path, const std::vector<uint8_t>& bytes) {
    return write_file_atomically(path, bytes.data(), bytes.size(), nullptr);
}

std::vector<uint8_t> read_bytes(const std::string& path) {
    std::vector<uint8_t> bytes;
    read_file(path, bytes, nullptr);
    return bytes;
}

std::vector<uint8_t> save_of(uint8_t fill) { return std::vector<uint8_t>(512, fill); }

void test_rom_info(const std::string& sandbox) {
    const std::string path = path_join(sandbox, "Mario Kart DS.nds");
    CHECK(write_bytes(path, ds_rom("AMCE", u"Mario Kart DS\nNintendo", 1, 0xbeef)));
    DsRomInfo info;
    std::string error;
    CHECK_MSG(read_ds_rom_info(path, info, &error), error);
    CHECK_STR_EQ(info.game_id, "AMCE-01-1-beef");
    CHECK_STR_EQ(info.game_code, "AMCE");
    CHECK_STR_EQ(info.title, "Mario Kart DS");
    CHECK_STR_EQ(info.publisher, "Nintendo");
    CHECK_EQ(info.icon.size(), kDsIconBytes);

    const std::vector<uint32_t> pixels = decode_ds_icon(info.icon);
    CHECK_EQ(pixels.size(), static_cast<std::size_t>(kDsIconSize * kDsIconSize));
    if (pixels.size() == static_cast<std::size_t>(kDsIconSize * kDsIconSize)) {
        CHECK_EQ(pixels[0], 0xFF0000FFu);   // colour 2, blue
        CHECK_EQ(pixels[1], 0xFFFF0000u);   // colour 1, red
        CHECK_EQ(pixels[31 * 32 + 31], 0xFFFF0000u);
    }
    // Colour 0 is see-through.
    std::vector<uint8_t> clear(kDsIconBytes, 0);
    const std::vector<uint32_t> clear_pixels = decode_ds_icon(clear);
    CHECK(!clear_pixels.empty() && clear_pixels[100] == 0u);
    CHECK(decode_ds_icon(std::vector<uint8_t>(10, 0)).empty());

    // A three-line banner: the last line is the publisher, the rest the title.
    CHECK(write_bytes(path, ds_rom("YDTE", u"Dragon Quest IX\nSentinels of the Starry Skies\nSquare Enix")));
    CHECK(read_ds_rom_info(path, info, nullptr));
    CHECK_STR_EQ(info.title, "Dragon Quest IX Sentinels of the Starry Skies");
    CHECK_STR_EQ(info.publisher, "Square Enix");

    // One line: a title with no publisher.
    CHECK(write_bytes(path, ds_rom("HOME", u"Homebrew")));
    CHECK(read_ds_rom_info(path, info, nullptr));
    CHECK_STR_EQ(info.title, "Homebrew");
    CHECK(info.publisher.empty());

    // Not a DS ROM at all.
    std::vector<uint8_t> junk(0x200, 0);
    CHECK(write_bytes(path, junk));
    CHECK(!read_ds_rom_info(path, info, &error));
    CHECK(!info.valid());
    CHECK(!read_ds_rom_info(path_join(sandbox, "missing.nds"), info, nullptr));
}

void test_comparison() {
    const auto state = [](bool has, const char* id, const char* hash,
                          std::vector<std::string> history) {
        SaveVersionState out;
        out.has_save = has;
        out.current_id = id;
        out.current_hash = hash;
        out.history_ids = std::move(history);
        return out;
    };
    const SaveVersionState none = state(false, "", "", {});
    const SaveVersionState a = state(true, "a", "h1", {"a"});
    const SaveVersionState ab = state(true, "b", "h2", {"a", "b"});
    const SaveVersionState ac = state(true, "c", "h3", {"a", "c"});
    const SaveVersionState stranger = state(true, "z", "h9", {"z"});
    const SaveVersionState same_contents = state(true, "y", "h1", {"y"});

    CHECK(compare_saves(none, none) == SaveComparison::Neither);
    CHECK(compare_saves(a, none) == SaveComparison::OnlyHere);
    CHECK(compare_saves(none, a) == SaveComparison::OnlyThere);
    CHECK(compare_saves(a, a) == SaveComparison::UpToDate);
    CHECK(compare_saves(ab, a) == SaveComparison::NewerHere);
    CHECK(compare_saves(a, ab) == SaveComparison::NewerThere);
    // Both changed since they matched.
    CHECK(compare_saves(ab, ac) == SaveComparison::Conflict);
    // Never compared.
    CHECK(compare_saves(a, stranger) == SaveComparison::Conflict);
    // The same bytes, arrived at separately, are the same save.
    CHECK(compare_saves(a, same_contents) == SaveComparison::UpToDate);
}

void test_store(const std::string& sandbox) {
    const std::string roms = path_join(sandbox, "roms");
    const std::string saves = path_join(sandbox, "saves");
    CHECK(ensure_directory(roms));
    CHECK(ensure_directory(saves));
    const std::string rom = path_join(roms, "Mario Kart DS.nds");
    CHECK(write_bytes(rom, ds_rom("AMCE", u"Mario Kart DS\nNintendo")));
    const std::string game_id = "AMCE-01-0-1a2b";
    const std::string save = path_join(saves, "Mario Kart DS.srm");
    const std::string resume = path_join(saves, "Mario Kart DS.resume");

    SaveStore here(saves, "Switch Lite");
    SaveStore there(path_join(sandbox, "deck-saves"), "steamdeck");
    CHECK(ensure_directory(there.saves_dir()));

    // Nothing played yet: nothing listed.
    CHECK(here.list().empty());

    // The game starts with no save, then makes one.
    here.note_game_started(rom);
    CHECK_STR_EQ(here.game_in_play(), game_id);
    CHECK(write_bytes(save, save_of(1)));
    here.note_game_stopped(rom);
    CHECK(here.game_in_play().empty());

    std::vector<SaveInfo> listed = here.list();
    CHECK_EQ(listed.size(), static_cast<std::size_t>(1));
    SaveInfo info;
    CHECK(here.find(game_id, info));
    CHECK_STR_EQ(info.title, "Mario Kart DS");
    CHECK_STR_EQ(info.publisher, "Nintendo");
    CHECK_EQ(info.icon.size(), kDsIconBytes);
    CHECK(info.has_save);
    CHECK_EQ(info.history.size(), static_cast<std::size_t>(1));
    CHECK(info.current() != nullptr && info.current()->made_on == "Switch Lite");
    CHECK(info.saved_at > 0);

    // Stopping again with the same save adds no version.
    here.note_game_started(rom);
    here.note_game_stopped(rom);
    CHECK(here.find(game_id, info));
    CHECK_EQ(info.history.size(), static_cast<std::size_t>(1));

    // Sent to the Deck, which has never seen the game: it arrives under the
    // sender's name, and the two agree.
    std::vector<uint8_t> bytes;
    SaveInfo sent;
    std::string error;
    CHECK_MSG(here.read_for_sending(game_id, sent, bytes, &error), error);
    CHECK(bytes == save_of(1));
    CHECK_MSG(there.accept(sent, bytes, "Switch Lite", &error), error);
    SaveInfo deck;
    CHECK(there.find(game_id, deck));
    CHECK_STR_EQ(deck.stem, "Mario Kart DS");
    CHECK_STR_EQ(deck.title, "Mario Kart DS");
    CHECK(compare_saves(version_state(info), version_state(deck)) == SaveComparison::UpToDate);
    // Nothing was there to set aside.
    CHECK(there.earlier(game_id).empty());

    // Played on, here: newer here.
    CHECK(write_bytes(save, save_of(2)));
    here.note_game_started(rom);
    here.note_game_stopped(rom);
    CHECK(here.find(game_id, info));
    CHECK_EQ(info.history.size(), static_cast<std::size_t>(2));
    CHECK(compare_saves(version_state(info), version_state(deck)) == SaveComparison::NewerHere);
    CHECK(compare_saves(version_state(deck), version_state(info)) == SaveComparison::NewerThere);

    // And on the Deck too: a conflict.
    CHECK(write_bytes(path_join(there.saves_dir(), "Mario Kart DS.srm"), save_of(3)));
    const std::string deck_rom = path_join(sandbox, "deck-rom/Mario Kart DS.nds");
    CHECK(ensure_directory(path_join(sandbox, "deck-rom")));
    CHECK(write_bytes(deck_rom, ds_rom("AMCE", u"Mario Kart DS\nNintendo")));
    there.note_game_started(deck_rom);
    there.note_game_stopped(deck_rom);
    CHECK(there.find(game_id, deck));
    CHECK(compare_saves(version_state(info), version_state(deck)) == SaveComparison::Conflict);

    // Get the Deck's here: this one's goes to Earlier Saves, the resume point
    // goes, and they agree again.
    CHECK(write_bytes(resume, save_of(9)));
    CHECK(here.has_resume_point(game_id));
    CHECK(there.read_for_sending(game_id, sent, bytes, &error));
    CHECK_MSG(here.accept(sent, bytes, "steamdeck", &error), error);
    CHECK(!file_exists(resume));
    CHECK(!here.has_resume_point(game_id));
    CHECK(read_bytes(save) == save_of(3));
    CHECK(here.find(game_id, info));
    CHECK(compare_saves(version_state(info), version_state(deck)) == SaveComparison::UpToDate);
    // The ROM path is this device's own, not the Deck's.
    CHECK_STR_EQ(info.rom_path, rom);
    std::vector<EarlierSave> earlier = here.earlier(game_id);
    CHECK_EQ(earlier.size(), static_cast<std::size_t>(1));
    if (!earlier.empty()) {
        CHECK_STR_EQ(earlier[0].reason, "Replaced by steamdeck's save");
        CHECK_STR_EQ(earlier[0].made_on, "Switch Lite");
        CHECK(read_bytes(earlier[0].path) == save_of(2));
    }

    // Put the earlier one back: newer than the Deck's now, since it was put
    // back after it.
    CHECK_MSG(here.restore(game_id, earlier.empty() ? "" : earlier[0].id, &error), error);
    CHECK(read_bytes(save) == save_of(2));
    CHECK(here.find(game_id, info));
    CHECK(compare_saves(version_state(info), version_state(deck)) == SaveComparison::NewerHere);
    earlier = here.earlier(game_id);
    CHECK_EQ(earlier.size(), static_cast<std::size_t>(1));
    if (!earlier.empty()) {
        CHECK_STR_EQ(earlier[0].reason, "Replaced by a restored save");
        CHECK(read_bytes(earlier[0].path) == save_of(3));
    }

    // Nothing replaces or deletes the save of the game being played.
    here.note_game_started(rom);
    CHECK(!here.remove(game_id, &error));
    CHECK(!here.accept(sent, bytes, "steamdeck", &error));
    CHECK(file_exists(save));
    here.note_game_stopped(rom);

    // Delete: gone, but listed still, for its earlier saves.
    CHECK_MSG(here.remove(game_id, &error), error);
    CHECK(!file_exists(save));
    listed = here.list();
    CHECK_EQ(listed.size(), static_cast<std::size_t>(1));
    if (!listed.empty()) CHECK(!listed[0].has_save);
    earlier = here.earlier(game_id);
    CHECK_EQ(earlier.size(), static_cast<std::size_t>(2));
    if (!earlier.empty()) CHECK_STR_EQ(earlier[0].reason, "Deleted");
    CHECK(!here.remove(game_id, &error));

    // Only the newest ten are kept.
    for (int round = 0; round < 12; ++round) {
        CHECK(write_bytes(save, save_of(static_cast<uint8_t>(20 + round))));
        CHECK(here.remove(game_id, nullptr));
    }
    earlier = here.earlier(game_id);
    CHECK_EQ(earlier.size(), kEarlierSavesKept);
    // Newest first, the oldest gone, however many were set aside in a second.
    if (earlier.size() == kEarlierSavesKept) {
        CHECK(read_bytes(earlier.front().path) == save_of(31));
        CHECK(read_bytes(earlier.back().path) == save_of(22));
    }
}

void test_renamed_rom(const std::string& sandbox) {
    const std::string saves = path_join(sandbox, "renamed-saves");
    CHECK(ensure_directory(saves));
    SaveStore store(saves, "steamdeck");
    const std::string old_rom = path_join(sandbox, "mkds.nds");
    const std::string new_rom = path_join(sandbox, "Mario Kart DS (USA).nds");
    CHECK(write_bytes(old_rom, ds_rom("AMCE", u"Mario Kart DS\nNintendo")));
    CHECK(write_bytes(new_rom, ds_rom("AMCE", u"Mario Kart DS\nNintendo")));
    store.note_game_started(old_rom);
    CHECK(write_bytes(path_join(saves, "mkds.srm"), save_of(5)));
    store.note_game_stopped(old_rom);

    // The same game under another file name finds its save.
    store.note_game_started(new_rom);
    CHECK(!file_exists(path_join(saves, "mkds.srm")));
    CHECK(read_bytes(path_join(saves, "Mario Kart DS (USA).srm")) == save_of(5));
    store.note_game_stopped(new_rom);
    SaveInfo info;
    CHECK(store.find("AMCE-01-0-1a2b", info));
    CHECK_STR_EQ(info.stem, "Mario Kart DS (USA)");
    CHECK_EQ(info.history.size(), static_cast<std::size_t>(1));

    // Another region is another game.
    const std::string european = path_join(sandbox, "mkds-eu.nds");
    CHECK(write_bytes(european, ds_rom("AMCP", u"Mario Kart DS\nNintendo")));
    store.note_game_started(european);
    store.note_game_stopped(european);
    CHECK(file_exists(path_join(saves, "Mario Kart DS (USA).srm")));
    CHECK(!store.find("AMCP-01-0-1a2b", info) || !info.has_save);
}

void test_unknown_save_matched(const std::string& sandbox) {
    // A save copied in by hand, never played here: matched to its ROM by name.
    const std::string saves = path_join(sandbox, "copied-saves");
    const std::string roms = path_join(sandbox, "copied-roms");
    CHECK(ensure_directory(saves));
    CHECK(ensure_directory(path_join(roms, "Racing")));
    CHECK(write_bytes(path_join(roms, "Racing/Mario Kart DS.nds"),
                      ds_rom("AMCE", u"Mario Kart DS\nNintendo")));
    CHECK(write_bytes(path_join(saves, "Mario Kart DS.srm"), save_of(7)));
    CHECK(write_bytes(path_join(saves, "Nothing Like It.srm"), save_of(8)));
    SaveStore store(saves, "PC");
    store.set_rom_dirs({roms});
    const std::vector<SaveInfo> listed = store.list();
    CHECK_EQ(listed.size(), static_cast<std::size_t>(2));
    bool matched = false;
    bool unmatched = false;
    for (const SaveInfo& info : listed) {
        if (info.stem == "Mario Kart DS") {
            matched = info.game_id == "AMCE-01-0-1a2b" && info.title == "Mario Kart DS" &&
                      info.history.size() == 1;
        }
        if (info.stem == "Nothing Like It") {
            unmatched = info.game_id.empty() && info.display_title() == "Nothing Like It";
        }
    }
    CHECK(matched);
    CHECK(unmatched);
}

}  // namespace

int main() {
    const std::string sandbox = make_sandbox();
    CHECK(!sandbox.empty());
    test_rom_info(sandbox);
    test_comparison();
    test_store(sandbox);
    test_renamed_rom(sandbox);
    test_unknown_save_matched(sandbox);
    return test::finish("save store");
}
