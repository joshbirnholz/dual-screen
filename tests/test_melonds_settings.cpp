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

    // As the core reads it: spaces anywhere, comments, "ours" for the default
    // shaders, a clock melonDS does not offer ignored.
    write_text(path, "# for Mario Kart\n jit = 0\nfastmem=0\n; old\ncpu_mhz=1020\nshaders=ours\nmystery=7\n");
    MelonDsTuning tuning = read_melonds_tuning(path);
    CHECK(!tuning.jit);
    CHECK(!tuning.fastmem);
    CHECK_EQ(tuning.cpu_mhz, 1020);
    CHECK(tuning.shaders.empty());
    CHECK(tuning.pipeline && tuning.texcache && !tuning.gpu_alone && !tuning.vramcheck);
    write_text(path + ".odd", "cpu_mhz=1000\n");
    CHECK_EQ(read_melonds_tuning(path + ".odd").cpu_mhz, 1785);

    // Changed: each setting's line rewritten where it was, one back at its
    // default gone, new ones added at the end, and the rest kept.
    tuning.jit = true;
    tuning.vramcheck = true;
    tuning.shaders = "standalone";
    std::string error;
    CHECK_MSG(write_melonds_tuning(tuning, path, &error), error);
    CHECK_STR_EQ(read_text(path),
                 "# for Mario Kart\nfastmem=0\n; old\ncpu_mhz=1020\nshaders=standalone\nmystery=7\nvramcheck=1\n");
    CHECK(read_melonds_tuning(path) == tuning);

    // All back to the defaults: only what this does not know is left.
    CHECK(write_melonds_tuning(MelonDsTuning(), path, nullptr));
    CHECK_STR_EQ(read_text(path), "# for Mario Kart\n; old\nmystery=7\n");

    // A second line for the same setting would only contradict the first.
    write_text(path, "jit=0\njit=1\n");
    tuning = read_melonds_tuning(path);
    CHECK(tuning.jit);
    tuning.jit = false;
    CHECK(write_melonds_tuning(tuning, path, nullptr));
    CHECK_STR_EQ(read_text(path), "jit=0\n");

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
    CHECK(!screen.on_melonds_page());
    press(screen, UiAction::Confirm);
    CHECK(screen.on_melonds_page());
    CHECK_EQ(screen.focus(), 0);

    // The clock, one step slower; then the JIT off. Each is in the file at once.
    press(screen, UiAction::Left);
    CHECK_EQ(read_melonds_tuning(path).cpu_mhz, 1581);
    press(screen, UiAction::Down);
    press(screen, UiAction::Confirm);
    CHECK(!read_melonds_tuning(path).jit);
    CHECK_STR_EQ(read_text(path), "# mine\ncpu_mhz=1581\njit=0\n");

    // Every row reachable, and no further.
    press(screen, UiAction::Down, 20);
    CHECK_EQ(screen.focus(), 7);
    press(screen, UiAction::Confirm);
    CHECK(read_melonds_tuning(path).vramcheck);

    // Back leaves the page, not Settings, onto the row it was opened from.
    SettingsScreenResult result = press(screen, UiAction::Cancel);
    CHECK(!result.close_requested);
    CHECK(!screen.on_melonds_page());
    CHECK(screen.is_open());
    CHECK_EQ(screen.focus(), melonds_row);
    result = press(screen, UiAction::Cancel);
    CHECK(result.close_requested);

    // Edited by hand while closed: what shows is the file.
    write_text(path, "jit=1\ncpu_mhz=714\n");
    screen.open(Settings(), {});
    press(screen, UiAction::Down, 20);
    press(screen, UiAction::Confirm);
    press(screen, UiAction::Right);  // 714 -> 816
    CHECK_EQ(read_melonds_tuning(path).cpu_mhz, 816);

    // With libretro's melonDS, only the two settings the app acts on.
    SettingsScreen plain(paths);
    plain.set_melonds_page(false, path);
    plain.open(Settings(), {});
    press(plain, UiAction::Down, 20);
    press(plain, UiAction::Confirm);
    CHECK(plain.on_melonds_page());
    press(plain, UiAction::Down, 20);
    CHECK_EQ(plain.focus(), 1);

    // Without it, no row at all.
    SettingsScreen none(paths);
    none.open(Settings(), {});
    press(none, UiAction::Down, 20);
    CHECK_EQ(none.focus(), melonds_row - 1);
    press(none, UiAction::Confirm);
    CHECK(!none.on_melonds_page());
}

}  // namespace

int main() {
    const std::string sandbox = make_sandbox();
    CHECK(!sandbox.empty());
    test_file(sandbox);
    test_page(sandbox);
    return test::finish("melonDS settings");
}
