// Settings > melonDS: the Switch's melonds.ini read and written -- a file
// written by hand keeping its comments and the lines it does not know, a
// setting back at its default losing its line -- and the page in Settings
// that edits it, opened from its row and left back to it.

#include "test_support.hpp"

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <stdlib.h>

#include "config/melonds_tuning.hpp"
#include "config/paths.hpp"
#include "ui/settings_screen.hpp"
#include "ui/accent.hpp"

using namespace ds;

namespace {

std::string make_sandbox() {
    char pattern[] = "/tmp/dualscreen-melondsini-XXXXXX";
    const char* directory = mkdtemp(pattern);
    return directory != nullptr ? std::string(directory) : std::string();
}

std::string read_text(const std::string& path) {
    std::ifstream file(path);
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}

void write_text(const std::string& path, const std::string& text) { std::ofstream(path) << text; }

void test_file(const std::string& sandbox) {
    const std::string path = path_join(sandbox, "melonds.ini");

    // Nothing there: every default.
    CHECK(read_melonds_tuning(path) == MelonDsTuning());

    // As the core reads it: spaces anywhere, comments, a clock melonDS does
    // not offer ignored. The core's diagnostic keys are not this file's to
    // know, and stay as they are written.
    write_text(path, "# for Mario Kart\n jit = 0\n; old\ncpu_mhz=1020\npipeline=0\nmystery=7\n");
    MelonDsTuning tuning = read_melonds_tuning(path);
    CHECK_EQ(tuning.cpu_mhz, 1020);
    CHECK(!tuning.pipeline);
    CHECK(!tuning.gpu_alone);
    write_text(path + ".odd", "cpu_mhz=1000\n");
    CHECK_EQ(read_melonds_tuning(path + ".odd").cpu_mhz, 1785);

    // Changed: each setting's line rewritten where it was, one back at its
    // default gone, new ones added at the end, and the rest kept.
    tuning.pipeline = true;
    tuning.gpu_alone = true;
    std::string error;
    CHECK_MSG(write_melonds_tuning(tuning, path, &error), error);
    CHECK_STR_EQ(read_text(path), "# for Mario Kart\n jit = 0\n; old\ncpu_mhz=1020\nmystery=7\ngpu_alone=1\n");
    CHECK(read_melonds_tuning(path) == tuning);

    // All back to the defaults: only what this does not know is left.
    CHECK(write_melonds_tuning(MelonDsTuning(), path, nullptr));
    CHECK_STR_EQ(read_text(path), "# for Mario Kart\n jit = 0\n; old\nmystery=7\n");

    // Settings retired since go when the file is next written.
    write_text(path, "gpucache=0\nl2flush=1\nsettle=2\nvramcheck=1\n");
    CHECK(write_melonds_tuning(MelonDsTuning(), path, nullptr));
    CHECK_STR_EQ(read_text(path), "vramcheck=1\n");

    // A second line for the same setting would only contradict the first.
    write_text(path, "cpu_mhz=1020\ncpu_mhz=714\n");
    tuning = read_melonds_tuning(path);
    CHECK_EQ(tuning.cpu_mhz, 714);
    tuning.cpu_mhz = 816;
    CHECK(write_melonds_tuning(tuning, path, nullptr));
    CHECK_STR_EQ(read_text(path), "cpu_mhz=816\n");

    // Made where nothing was, folder and all.
    const std::string fresh = path_join(path_join(sandbox, "dualscreen"), "melonds.ini");
    tuning = MelonDsTuning();
    tuning.gpu_alone = true;
    CHECK(write_melonds_tuning(tuning, fresh, nullptr));
    CHECK_STR_EQ(read_text(fresh), "gpu_alone=1\n");

    // The clock steps through melonDS's speeds and stops at the ends.
    CHECK_EQ(next_melonds_cpu_mhz(1785, -1), 1581);
    CHECK_EQ(next_melonds_cpu_mhz(1581, 1), 1785);
    CHECK_EQ(next_melonds_cpu_mhz(1785, 1), 1785);
    CHECK_EQ(next_melonds_cpu_mhz(714, -1), 714);
}

SettingsScreenResult press(SettingsScreen& screen, UiAction action, int times = 1) {
    SettingsScreenResult result;
    for (int i = 0; i < times; ++i) {
        const SettingsScreenResult step = screen.update({action}, {}, 1280.0f, 720.0f);
        result.close_requested = result.close_requested || step.close_requested;
        result.settings_changed = result.settings_changed || step.settings_changed;
        result.open_data_management = result.open_data_management || step.open_data_management;
    }
    return result;
}

void test_page(const std::string& sandbox) {
    const std::string path = path_join(sandbox, "page.ini");
    write_text(path, "# mine\n");
    AppPaths paths;
    SettingsScreen screen(paths);
    screen.set_melonds_page(true, path);
    screen.open(Settings(), {});

    // The melonDS row is last; going down stops on it.
    press(screen, UiAction::Down, 20);
    const int melonds_row = screen.focus();
    CHECK(melonds_row > 0);
    // Back does nothing on a way in; forwards opens it.
    press(screen, UiAction::Left);
    CHECK(!screen.on_emulators_page());
    press(screen, UiAction::Confirm);
    CHECK(screen.on_emulators_page());
    CHECK_EQ(screen.focus(), 0);

    // The clock, one step slower, chosen from its popup -- fastest first,
    // the one in effect under the cursor; then each frame shown as soon as
    // drawn. Each is in the file at once.
    press(screen, UiAction::Left);  // nothing: values are not stepped from the side
    CHECK_EQ(read_melonds_tuning(path).cpu_mhz, MelonDsTuning().cpu_mhz);
    press(screen, UiAction::Confirm);
    press(screen, UiAction::Down);
    press(screen, UiAction::Confirm);
    CHECK_EQ(read_melonds_tuning(path).cpu_mhz, 1581);
    press(screen, UiAction::Down);
    press(screen, UiAction::Confirm);
    press(screen, UiAction::Up);
    press(screen, UiAction::Confirm);
    CHECK(!read_melonds_tuning(path).pipeline);
    CHECK_STR_EQ(read_text(path), "# mine\ncpu_mhz=1581\npipeline=0\n");

    // Only those two, and no further.
    press(screen, UiAction::Down, 20);
    CHECK_EQ(screen.focus(), 1);

    // Back leaves the page, not Settings, onto the row it was opened from.
    SettingsScreenResult result = press(screen, UiAction::Cancel);
    CHECK(!result.close_requested);
    CHECK(!screen.on_emulators_page());
    CHECK(screen.is_open());
    CHECK_EQ(screen.focus(), melonds_row);
    result = press(screen, UiAction::Cancel);
    CHECK(result.close_requested);

    // Edited by hand while closed: what shows is the file.
    write_text(path, "pipeline=1\ncpu_mhz=714\n");
    screen.open(Settings(), {});
    press(screen, UiAction::Down, 20);
    press(screen, UiAction::Confirm);
    press(screen, UiAction::Confirm);  // the popup, on 714
    press(screen, UiAction::Up);       // 816
    press(screen, UiAction::Confirm);
    CHECK_EQ(read_melonds_tuning(path).cpu_mhz, 816);

    // With libretro's melonDS, only the clock.
    SettingsScreen plain(paths);
    plain.set_melonds_page(false, path);
    plain.open(Settings(), {});
    press(plain, UiAction::Down, 20);
    press(plain, UiAction::Confirm);
    CHECK(plain.on_emulators_page());
    press(plain, UiAction::Down, 20);
    CHECK_EQ(plain.focus(), 0);

    // A switch on the Settings page flips with A, never sideways.
    {
        SettingsScreen switches(paths);
        switches.open(Settings(), {});
        for (int row = 0; row < 12; ++row) {
            const SettingsScreenResult sideways = press(switches, UiAction::Right);
            const SettingsScreenResult back = press(switches, UiAction::Left);
            CHECK_MSG(!sideways.settings_changed && !back.settings_changed,
                      "a sideways press changed a setting");
            press(switches, UiAction::Down);
        }
    }

    // Data Management first, on a device with saves: chosen, it asks to be
    // opened; come back from, the cursor is on it again. The melonDS row is
    // still last.
    SettingsScreen with_saves(paths);
    with_saves.set_offers_data_management(true);
    with_saves.set_melonds_page(true, path);
    with_saves.open(Settings(), {});
    CHECK_EQ(with_saves.focus(), 0);
    CHECK(press(with_saves, UiAction::Confirm).open_data_management);
    CHECK(!press(with_saves, UiAction::Left).open_data_management);
    press(with_saves, UiAction::Down, 20);
    CHECK_EQ(with_saves.focus(), melonds_row + 1);
    press(with_saves, UiAction::Confirm);
    CHECK(with_saves.on_emulators_page());
    press(with_saves, UiAction::Cancel);
    with_saves.return_from_data_management();
    CHECK_EQ(with_saves.focus(), 0);

    // Without it, no row at all.
    SettingsScreen none(paths);
    none.open(Settings(), {});
    press(none, UiAction::Down, 20);
    CHECK_EQ(none.focus(), melonds_row - 1);
    press(none, UiAction::Confirm);
    CHECK(!none.on_emulators_page());
}

// The DS's owner: a name typed into a popup -- Backspace taking characters
// off, ten at the most, OK keeping it and B not -- and a favorite color
// chosen from the sixteen, which is the menus' accent too.
void test_the_ds_owner(const std::string& sandbox) {
    (void)sandbox;
    AppPaths paths;
    SettingsScreen screen(paths);
    screen.open(Settings(), {});
    // Down to the nickname row: it says it is not set.
    int guard = 0;
    while (guard++ < 20) {
        press(screen, UiAction::Confirm);
        if (screen.wants_text()) break;
        // A row with a popup of its choices puts it away again.
        press(screen, UiAction::Cancel);
        press(screen, UiAction::Down);
    }
    CHECK_MSG(screen.wants_text(), "no row opened a field to type the DS's name into");
    screen.type("Josh Birnholz!", 0);
    press(screen, UiAction::Down);  // nothing to move: the field takes it
    CHECK(screen.wants_text());
    screen.type("", 2);
    CHECK(press(screen, UiAction::Confirm).settings_changed);
    CHECK(!screen.wants_text());
    // Ten characters, the last two taken off again.
    CHECK_STR_EQ(screen.settings().ds_nickname, "Josh Bir");

    // Typed again and backed out of: as it was.
    press(screen, UiAction::Confirm);
    screen.type("XYZ", 0);
    CHECK(!press(screen, UiAction::Cancel).settings_changed);
    CHECK_STR_EQ(screen.settings().ds_nickname, "Josh Bir");

    // The next row: the favorite color, Light Blue to begin with, from a
    // grid of the sixteen four to a line, as a DS lays them out -- Up a line
    // and Right one along from Light Blue is Green.
    CHECK_EQ(Settings().ds_favorite_color, 10);
    press(screen, UiAction::Down);
    press(screen, UiAction::Confirm);
    press(screen, UiAction::Up);
    press(screen, UiAction::Right);
    CHECK(press(screen, UiAction::Confirm).settings_changed);
    CHECK_EQ(screen.settings().ds_favorite_color, 7);
    CHECK_STR_EQ(kDsFavoriteColors[7].name, "Green");
    // The edges stop the cursor rather than wrap it.
    press(screen, UiAction::Confirm);
    press(screen, UiAction::Right, 6);
    press(screen, UiAction::Up, 3);
    CHECK(press(screen, UiAction::Confirm).settings_changed);
    CHECK_EQ(screen.settings().ds_favorite_color, 3);

    // The birthday: a month, then a day of it; B on the day changes nothing.
    press(screen, UiAction::Down);
    press(screen, UiAction::Confirm);
    press(screen, UiAction::Down, 2);  // March
    CHECK(!press(screen, UiAction::Confirm).settings_changed);
    CHECK(!press(screen, UiAction::Cancel).settings_changed);
    CHECK_EQ(screen.settings().ds_birth_month, 0);
    press(screen, UiAction::Confirm);
    press(screen, UiAction::Down, 2);
    press(screen, UiAction::Confirm);
    press(screen, UiAction::Down, 13);  // the 14th
    CHECK(press(screen, UiAction::Confirm).settings_changed);
    CHECK_EQ(screen.settings().ds_birth_month, 3);
    CHECK_EQ(screen.settings().ds_birth_day, 14);

    // As the accent: the color exactly, and the ring running from a lighter
    // shade of it into it.
    apply_favorite_color_accent(12);  // Dark Blue
    CHECK_EQ(ui_theme().accent.b, kDsFavoriteColors[12].b);
    CHECK_EQ(menu_ring_right().b, kDsFavoriteColors[12].b);
    CHECK(menu_ring_left().r > kDsFavoriteColors[12].r && menu_ring_left().b > kDsFavoriteColors[12].b);
    // None chosen: Light Blue.
    apply_favorite_color_accent(-1);
    CHECK_EQ(ui_theme().accent.g, kDsFavoriteColors[10].g);
}

}  // namespace

int main() {
    const std::string sandbox = make_sandbox();
    CHECK(!sandbox.empty());
    test_file(sandbox);
    test_page(sandbox);
    test_the_ds_owner(sandbox);
    return test::finish("melonDS settings");
}
