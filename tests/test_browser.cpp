// Covers the game browser's file-system behaviour: which files it offers, where
// it starts, and how it moves between directories.

#include "test_support.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <unistd.h>

#include "config/paths.hpp"
#include "input/ds_input.hpp"
#include "input/held_input_gate.hpp"
#include "ui/game_browser.hpp"

using namespace ds;

namespace {

std::string make_sandbox() {
    char pattern[] = "/tmp/dualscreen-browse-XXXXXX";
    const char* directory = mkdtemp(pattern);
    return directory != nullptr ? std::string(directory) : std::string();
}

void touch_file(const std::string& path) {
    std::ofstream file(path, std::ios::binary);
    file << "x";
}

void test_rom_extensions() {
    // What melonDS DS actually accepts.
    CHECK(is_supported_rom("game.nds"));
    CHECK(is_supported_rom("GAME.NDS"));
    CHECK(is_supported_rom("game.dsi"));
    CHECK(is_supported_rom("game.ids"));
    CHECK(is_supported_rom("game.srl"));
    CHECK(is_supported_rom("Pokemon Platinum (USA).nds"));

    // Not offered: archives would need a libretro VFS this frontend does not
    // implement, and the rest are simply not ROMs.
    CHECK(!is_supported_rom("game.zip"));
    CHECK(!is_supported_rom("game.7z"));
    CHECK(!is_supported_rom("game.sav"));
    CHECK(!is_supported_rom("readme.txt"));
    CHECK(!is_supported_rom("nds"));
    CHECK(!is_supported_rom(""));
    CHECK(!is_supported_rom(".nds"));  // a dotfile with no name
}

void test_listing_sorts_and_filters() {
    const std::string sandbox = make_sandbox();
    CHECK(!sandbox.empty());
    if (sandbox.empty()) return;

    ensure_directory(path_join(sandbox, "zebra"));
    ensure_directory(path_join(sandbox, "Alpha"));
    touch_file(path_join(sandbox, "beta.nds"));
    touch_file(path_join(sandbox, "Alpha.nds"));
    touch_file(path_join(sandbox, "notes.txt"));
    touch_file(path_join(sandbox, ".hidden.nds"));

    std::vector<DirectoryEntry> entries;
    std::string error;
    CHECK_MSG(list_directory(sandbox, entries, &error), error);

    // Directories first, then files, each group case-insensitively sorted.
    CHECK_EQ(entries.size(), static_cast<std::size_t>(5));
    if (entries.size() == 5) {
        CHECK_STR_EQ(entries[0].name, "Alpha");
        CHECK(entries[0].is_directory);
        CHECK_STR_EQ(entries[1].name, "zebra");
        CHECK(entries[1].is_directory);
        CHECK_STR_EQ(entries[2].name, "Alpha.nds");
        CHECK(!entries[2].is_directory);
        CHECK_STR_EQ(entries[3].name, "beta.nds");
        CHECK_STR_EQ(entries[4].name, "notes.txt");
    }
    // Dotfiles never show up.
    for (const DirectoryEntry& entry : entries) {
        CHECK(entry.name[0] != '.');
    }

    // A directory that cannot be read reports why rather than throwing.
    CHECK(!list_directory(path_join(sandbox, "no-such-place"), entries, &error));
    CHECK(!error.empty());
    CHECK(entries.empty());
}

void test_browser_navigation() {
    const std::string sandbox = make_sandbox();
    if (sandbox.empty()) return;
    const std::string games = path_join(sandbox, "games");
    ensure_directory(games);
    touch_file(path_join(games, "one.nds"));
    touch_file(path_join(games, "two.nds"));
    touch_file(path_join(games, "manual.pdf"));

    GameBrowser browser;
    browser.open(games);
    CHECK(browser.is_open());
    CHECK_STR_EQ(browser.current_directory(), games);

    // A directory that no longer exists falls back to somewhere usable rather
    // than showing an empty screen.
    GameBrowser fallback;
    fallback.open(path_join(sandbox, "deleted-since-last-time"));
    CHECK(fallback.is_open());
    CHECK(directory_exists(fallback.current_directory()));
}

void test_default_browse_dirs_all_exist() {
    // Every suggested starting point must be real, since one of them is used
    // sight-unseen on a first run.
    const std::vector<std::string> candidates = default_browse_dirs();
    CHECK(!candidates.empty());
    for (const std::string& candidate : candidates) {
        CHECK_MSG(directory_exists(candidate), candidate + " does not exist");
    }
}

// The bug this guards: dismissing a menu with A left the button physically
// down, and because button state is polled rather than edge-triggered, the
// game saw it as a fresh press on the very next frame.
void test_held_input_gate() {
    const uint32_t a = 1u << static_cast<unsigned>(DsButton::A);
    const uint32_t b = 1u << static_cast<unsigned>(DsButton::B);

    HeldInputGate gate;

    // Menu open: the gate is reset and nothing is carried.
    gate.reset();

    // "Resume" is chosen with A, so A is still down as the game resumes.
    gate.arm();
    gate.update(a, false);
    CHECK_EQ(gate.buttons(), 0u);          // suppressed, not passed through
    CHECK(gate.is_masking());

    // Still held a few frames later: still suppressed.
    gate.update(a, false);
    CHECK_EQ(gate.buttons(), 0u);

    // Another button pressed while A is still down gets through on its own.
    gate.update(a | b, false);
    CHECK_EQ(gate.buttons(), b);

    // A released: the mask lifts.
    gate.update(b, false);
    CHECK_EQ(gate.buttons(), b);
    // ...and pressing A again now counts.
    gate.update(a | b, false);
    CHECK_EQ(gate.buttons(), a | b);
    CHECK(!gate.is_masking());

    // A touch still down when the menu closes is suppressed the same way.
    gate.reset();
    gate.arm();
    gate.update(0, true);
    CHECK(!gate.touching());
    gate.update(0, true);
    CHECK(!gate.touching());
    gate.update(0, false);   // finger lifted
    CHECK(!gate.touching());
    gate.update(0, true);    // a genuinely new touch
    CHECK(gate.touching());

    // Arming with nothing held suppresses nothing at all.
    gate.reset();
    gate.arm();
    gate.update(0, false);
    CHECK(!gate.is_masking());
    gate.update(a, true);
    CHECK_EQ(gate.buttons(), a);
    CHECK(gate.touching());
}

}  // namespace

int main() {
    test_rom_extensions();
    test_listing_sorts_and_filters();
    test_browser_navigation();
    test_default_browse_dirs_all_exist();
    test_held_input_gate();
    return test::finish("browser");
}
