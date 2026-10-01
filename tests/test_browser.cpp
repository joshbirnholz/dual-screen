// Covers the game browser's file-system behaviour: which files it offers, where
// it starts, and how it moves between directories.

#include "test_support.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <unistd.h>

#include "config/paths.hpp"
#include "ui/launcher_screen.hpp"
#include "input/ds_input.hpp"
#include "input/cursor_visibility.hpp"
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

// The pointer has to be reachable without being permanently on screen: hidden
// until the mouse moves, back for a couple of seconds, gone again.
void test_cursor_auto_hides() {
    CursorVisibility cursor(2.0);

    // Nothing has been touched yet, so there is no arrow over the game.
    CHECK(!cursor.visible());
    cursor.update(10.0);
    CHECK(!cursor.visible());

    // Moving the mouse brings it straight back.
    cursor.notice_mouse_activity();
    CHECK(cursor.visible());

    // It stays up while the mouse is being used, however long that goes on.
    for (int frame = 0; frame < 100; ++frame) {
        cursor.update(1.0 / 60.0);
        cursor.notice_mouse_activity();
        CHECK(cursor.visible());
    }

    // Held still, it survives a moment and then goes.
    cursor.update(1.5);
    CHECK_MSG(cursor.visible(), "the pointer vanished while the mouse was barely still");
    cursor.update(0.6);
    CHECK(!cursor.visible());

    // And it comes back again afterwards -- hiding is not one-way.
    cursor.notice_mouse_activity();
    CHECK(cursor.visible());
    cursor.update(2.0);
    CHECK(!cursor.visible());

    // Turned off, nothing brings it back -- which is the whole point of the
    // setting, and was broken for as long as the mode was never applied.
    cursor.set_mode(CursorMode::Hidden);
    cursor.notice_mouse_activity();
    CHECK_MSG(!cursor.visible(), "the pointer showed up with the pointer turned off");
    cursor.update(0.1);
    CHECK(!cursor.visible());

    // Turned back on it behaves as before, from what the timer says rather than
    // from whatever it was showing when it was switched off.
    cursor.set_mode(CursorMode::Shown);
    CHECK(cursor.visible());  // the mouse moved a moment ago
    cursor.update(2.0);
    CHECK(!cursor.visible());
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

// The reported symptom: "the A button doesn't actually select a directory".
// Two Confirms in one frame -- which is what a duplicated Steam Input device
// produces -- used to enter the folder and immediately back out of it.
void test_duplicate_confirms_navigate_once() {
    const std::string sandbox = make_sandbox();
    if (sandbox.empty()) return;
    const std::string inner = path_join(sandbox, "inner");
    ensure_directory(inner);
    touch_file(path_join(inner, "game.nds"));

    GameBrowser browser;
    browser.open(sandbox);
    CHECK_STR_EQ(browser.current_directory(), sandbox);

    InputManager input;  // never initialized: no pointer, no controllers
    const std::vector<PointerEvent> no_pointers;

    // Focus starts on "..", so step down onto "inner" first.
    browser.update(input, {UiAction::Down}, no_pointers, 1280.0f, 800.0f);

    // Two Confirms, one frame.
    const GameBrowserResult result =
        browser.update(input, {UiAction::Confirm, UiAction::Confirm}, no_pointers, 1280.0f, 800.0f);
    CHECK(result.directory_changed);
    CHECK_MSG(browser.current_directory() == inner,
              "ended up in " + browser.current_directory() + " instead of " + inner);

    // A Confirm and a Cancel together must not cancel out either.
    browser.update(input, {UiAction::Down}, no_pointers, 1280.0f, 800.0f);
    browser.update(input, {UiAction::Confirm, UiAction::Cancel}, no_pointers, 1280.0f, 800.0f);
    CHECK_MSG(browser.current_directory() == inner,
              "a Confirm+Cancel pair moved to " + browser.current_directory());
}

// Plus is Main Menu: out of the browser from however deep in the folders it
// is, where B only climbs one. And there is no Quit here any more -- leaving
// the app is the main menu's to offer -- so Quit leaves the browser as it was.
void test_plus_goes_to_the_main_menu_and_quit_is_not_here() {
    const std::string sandbox = make_sandbox();
    if (sandbox.empty()) return;
    const std::string inner = path_join(sandbox, "inner");
    ensure_directory(inner);
    touch_file(path_join(inner, "game.nds"));

    GameBrowser browser;
    browser.open(inner);
    InputManager input;
    const std::vector<PointerEvent> no_pointers;

    const GameBrowserResult quit = browser.update(input, {UiAction::Quit}, no_pointers, 1280.0f, 800.0f);
    CHECK(!quit.back_requested);
    CHECK(!quit.game_chosen);
    CHECK(browser.is_open());
    CHECK_STR_EQ(browser.current_directory(), inner);

    const GameBrowserResult plus =
        browser.update(input, {UiAction::MenuDisconnect}, no_pointers, 1280.0f, 800.0f);
    CHECK_MSG(plus.back_requested, "plus did not ask for the main menu");
    CHECK_STR_EQ(browser.current_directory(), inner);
}

void test_confirm_on_a_rom_chooses_it() {
    const std::string sandbox = make_sandbox();
    if (sandbox.empty()) return;
    touch_file(path_join(sandbox, "only.nds"));

    GameBrowser browser;
    browser.open(sandbox);
    InputManager input;
    const std::vector<PointerEvent> no_pointers;

    // Rows are ".." then "only.nds".
    browser.update(input, {UiAction::Down}, no_pointers, 1280.0f, 800.0f);
    const GameBrowserResult result =
        browser.update(input, {UiAction::Confirm}, no_pointers, 1280.0f, 800.0f);
    CHECK(result.game_chosen);
    CHECK_STR_EQ(result.chosen_path, path_join(sandbox, "only.nds"));
    CHECK(!browser.is_open());
}

// Walking up out of a deep folder has to keep working at every level. The
// parent row's path is handed straight to navigate_to, which rebuilds the row
// list -- so if navigate_to reads its argument after that rebuild it is
// reading a string it just destroyed, and the ".." it writes for the next
// level points at nothing. That shows up as a browser that climbs one level
// and then refuses to move, which is exactly what a player would hit after
// landing somewhere they did not mean to be.
void test_cancel_climbs_all_the_way_out() {
    const std::string sandbox = make_sandbox();
    if (sandbox.empty()) return;

    // Long names on purpose: a short path fits inside std::string's inline
    // buffer, where a use-after-free leaves the bytes readable and the bug
    // stays hidden.
    const std::string deep = path_join(
        sandbox, "Emulation-roms-nintendo-ds-collection/sorted-by-publisher/nintendo-first-party");
    CHECK(ensure_directory(deep));
    touch_file(path_join(deep, "only.nds"));

    GameBrowser browser;
    browser.open(deep);
    CHECK_STR_EQ(browser.current_directory(), deep);

    InputManager input;
    const std::vector<PointerEvent> no_pointers;

    std::string here = deep;
    while (here != "/") {
        const std::string expected = path_dirname(here);
        browser.update(input, {UiAction::Cancel}, no_pointers, 1280.0f, 800.0f);
        CHECK_MSG(browser.current_directory() == expected,
                  "climbing out of " + here + " landed in '" + browser.current_directory() +
                      "' rather than " + expected);
        if (browser.current_directory() != expected) return;
        here = expected;
    }
}

// The trap that stranded a player in the app's own build directory: the
// browser held a relative path. path_dirname maps "." -- and any bare name --
// to "." forever, so the ".." row pointed at the directory it was already in
// and there was no way up. It displayed as "." too, which is how a build
// directory came to look like the root of the filesystem.
void test_a_relative_directory_never_traps_the_browser() {
    const std::string sandbox = make_sandbox();
    if (sandbox.empty()) return;
    CHECK(ensure_directory(path_join(sandbox, "generated/tests")));

    InputManager input;
    const std::vector<PointerEvent> no_pointers;

    // A remembered folder that is not absolute could only be junk left behind
    // by a bug, so it is dropped for a default rather than resolved against
    // wherever the app happens to have been started from.
    for (const char* remembered : {".", "./generated", "CMakeFiles", ""}) {
        GameBrowser browser;
        browser.open(remembered);
        const std::string landed = browser.current_directory();
        CHECK_MSG(!landed.empty() && landed.front() == '/',
                  std::string("opening with \"") + remembered + "\" left the browser at '" +
                      landed + "'");
        if (landed.empty() || landed.front() != '/') continue;
        CHECK_MSG(path_dirname(landed) != landed,
                  std::string("opening with \"") + remembered +
                      "\" left the browser in a directory that is its own parent");
        CHECK_MSG(directory_exists(landed), landed + " does not exist");

        // And Back really moves, rather than pressing forever against a wall.
        if (landed == "/") continue;
        browser.update(input, {UiAction::Cancel}, no_pointers, 1280.0f, 800.0f);
        CHECK_MSG(browser.current_directory() == path_dirname(landed),
                  "Back did not move the browser out of " + landed);
    }
}

// Every choice the opening screen can reach, by tapping over the whole thing:
// the rows' positions are the screen's own business, so this finds them rather
// than being told where they are.
std::vector<LauncherChoice> launcher_choices(bool runs_games) {
    constexpr float kWidth = 1280.0f;
    constexpr float kHeight = 800.0f;
    std::vector<LauncherChoice> found;
    for (float y = 4.0f; y < kHeight; y += 8.0f) {
        for (float x = 4.0f; x < kWidth; x += 8.0f) {
            LauncherScreen launcher;
            launcher.open(runs_games);
            PointerEvent tap;
            tap.x = x;
            tap.y = y;
            // Down and up in the same place, which is what a tap is.
            tap.press_x = x;
            tap.press_y = y;
            tap.released = true;
            // Twice: the first tap highlights a row, the second chooses it.
            const LauncherChoice choice =
                launcher.update({}, {tap, tap}, kWidth, kHeight).choice;
            if (choice != LauncherChoice::None &&
                std::find(found.begin(), found.end(), choice) == found.end()) {
                found.push_back(choice);
            }
        }
    }
    return found;
}

bool offers(const std::vector<LauncherChoice>& choices, LauncherChoice choice) {
    return std::find(choices.begin(), choices.end(), choice) != choices.end();
}

// A companion client has no ROMs of its own to load and nothing in Settings
// that would apply to it -- folders, cores and what a new game starts with all
// belong to the device actually running one. What is left is which screen to
// be, and leaving.
void test_the_opening_screen_offers_what_the_device_can_do() {
    const std::vector<LauncherChoice> full = launcher_choices(/*runs_games=*/true);
    CHECK(offers(full, LauncherChoice::LoadGame));
    CHECK(offers(full, LauncherChoice::ConnectAsTopScreen));
    CHECK(offers(full, LauncherChoice::ConnectAsBottomScreen));
    CHECK(offers(full, LauncherChoice::Settings));
    CHECK(offers(full, LauncherChoice::Quit));

    const std::vector<LauncherChoice> companion = launcher_choices(/*runs_games=*/false);
    CHECK_MSG(!offers(companion, LauncherChoice::LoadGame),
              "a companion offered a game to load");
    CHECK_MSG(!offers(companion, LauncherChoice::Settings),
              "a companion offered Settings");
    // Both screens, because which one to be is the whole question there.
    CHECK(offers(companion, LauncherChoice::ConnectAsTopScreen));
    CHECK(offers(companion, LauncherChoice::ConnectAsBottomScreen));
    CHECK(offers(companion, LauncherChoice::Quit));
}

// A Switch with its own emulator plays games, so it has Settings here too --
// and Data Management inside them, as everywhere.
void test_a_switch_that_plays_games_offers_settings() {
    LauncherScreen launcher;
    launcher.set_offers_load_game(true);
    launcher.open(/*runs_games=*/false);
    std::vector<LauncherChoice> seen;
    for (int step = 0; step < 8; ++step) {
        LauncherScreen walker;
        walker.set_offers_load_game(true);
        walker.open(false);
        for (int down = 0; down < step; ++down) walker.update({UiAction::Down}, {}, 1280.0f, 800.0f);
        seen.push_back(walker.update({UiAction::Confirm}, {}, 1280.0f, 800.0f).choice);
    }
    CHECK(offers(seen, LauncherChoice::LoadGame));
    CHECK(offers(seen, LauncherChoice::Settings));
}

// Moving down the list reaches every row and stops at the last, whichever set
// of rows this device has: lists do not go round.
void test_the_opening_screen_stops_at_its_last_row() {
    for (const bool runs_games : {true, false}) {
        LauncherScreen launcher;
        launcher.open(runs_games);
        const std::size_t rows = launcher_choices(runs_games).size();
        CHECK(rows > 0);

        std::vector<LauncherChoice> seen;
        for (std::size_t step = 0; step < rows; ++step) {
            LauncherScreen walker;
            walker.open(runs_games);
            for (std::size_t down = 0; down < step; ++down) {
                walker.update({UiAction::Down}, {}, 1280.0f, 800.0f);
            }
            seen.push_back(walker.update({UiAction::Confirm}, {}, 1280.0f, 800.0f).choice);
        }
        CHECK_EQ(seen.size(), rows);

        // More Downs than there are rows stays on the last; Up from the
        // first stays on the first.
        LauncherScreen past_end;
        past_end.open(runs_games);
        for (std::size_t down = 0; down < rows + 3; ++down) {
            past_end.update({UiAction::Down}, {}, 1280.0f, 800.0f);
        }
        CHECK_MSG(past_end.update({UiAction::Confirm}, {}, 1280.0f, 800.0f).choice == seen.back(),
                  "moving past the last row did not stay on it");
        LauncherScreen before_start;
        before_start.open(runs_games);
        before_start.update({UiAction::Up}, {}, 1280.0f, 800.0f);
        CHECK_MSG(before_start.update({UiAction::Confirm}, {}, 1280.0f, 800.0f).choice == seen.front(),
                  "moving up from the first row did not stay on it");
    }
}

// A single tap highlights a row and chooses nothing; a second on the same row
// chooses it.
void test_a_tap_highlights_before_it_chooses() {
    LauncherScreen launcher;
    launcher.open(true);
    const std::size_t rows = launcher_choices(true).size();
    CHECK(rows > 1);
    // The cursor on the second row; then the first, found by tapping down the
    // screen until the highlight moves.
    launcher.update({UiAction::Down}, {}, 1280.0f, 800.0f);
    bool found = false;
    for (float y = 4.0f; y < 800.0f && !found; y += 8.0f) {
        PointerEvent tap;
        tap.x = 640.0f;
        tap.y = y;
        tap.press_x = tap.x;
        tap.press_y = y;
        tap.released = true;
        const int before = launcher.focus();
        const LauncherChoice once = launcher.update({}, {tap}, 1280.0f, 800.0f).choice;
        if (launcher.focus() != before) {
            CHECK_MSG(once == LauncherChoice::None, "one tap on a new row chose it");
            CHECK_MSG(launcher.update({}, {tap}, 1280.0f, 800.0f).choice != LauncherChoice::None,
                      "a second tap on the highlighted row did not choose it");
            found = true;
        }
    }
    CHECK_MSG(found, "no tap moved the highlight");
}

}  // namespace

int main() {
    test_rom_extensions();
    test_listing_sorts_and_filters();
    test_browser_navigation();
    test_cancel_climbs_all_the_way_out();
    test_a_relative_directory_never_traps_the_browser();
    test_default_browse_dirs_all_exist();
    test_held_input_gate();
    test_cursor_auto_hides();
    test_duplicate_confirms_navigate_once();
    test_confirm_on_a_rom_chooses_it();
    test_plus_goes_to_the_main_menu_and_quit_is_not_here();
    test_the_opening_screen_offers_what_the_device_can_do();
    test_a_switch_that_plays_games_offers_settings();
    test_the_opening_screen_stops_at_its_last_row();
    test_a_tap_highlights_before_it_chooses();
    return test::finish("browser");
}
