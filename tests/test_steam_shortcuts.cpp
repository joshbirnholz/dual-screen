// Steam's own list of non-Steam games, read for the ones that start a game of
// this app's: what a connected device is offered to start here.

#include <cstdio>
#include <filesystem>
#include <fstream>

#include "launch/game_catalog.hpp"
#include "launch/steam_shortcuts.hpp"
#include "test_support.hpp"

using namespace ds;

namespace {

std::vector<SteamShortcut> sample() {
    SteamShortcut zelda;
    zelda.appid = 0x8a1b2c3du;
    zelda.name = "Zelda: Spirit Tracks";
    zelda.exe = "\"/home/deck/Applications/Dualscreen.AppImage\"";
    zelda.start_dir = "\"/home/deck/Applications/\"";
    zelda.launch_options = "--game \"/home/deck/Emulation/roms/nds/Spirit Tracks.nds\"";
    SteamShortcut other;
    other.appid = 0x12345678u;
    other.name = "Some Other Game";
    other.exe = "\"/usr/bin/othergame\"";
    other.launch_options = "--game \"/home/deck/x.bin\"";
    SteamShortcut flatpak;
    flatpak.appid = 0x0badf00du;
    flatpak.name = "Professor Layton";
    flatpak.exe = "\"/usr/bin/flatpak\"";
    flatpak.launch_options = "run io.github.dualscreen.Dualscreen --game '/run/media/sd/Layton.nds'";
    flatpak.hidden = true;
    return {zelda, other, flatpak};
}

void test_shortcuts_are_read_back() {
    std::vector<SteamShortcut> read;
    CHECK(parse_steam_shortcuts(write_steam_shortcuts(sample()), read));
    CHECK_EQ(read.size(), 3u);
    CHECK_EQ(read[0].appid, 0x8a1b2c3du);
    CHECK_STR_EQ(read[0].name, "Zelda: Spirit Tracks");
    CHECK_STR_EQ(read[1].exe, "\"/usr/bin/othergame\"");
    CHECK(read[2].hidden);
    // What Steam's own run link takes for a non-Steam game.
    CHECK_EQ(read[0].run_game_id(), 0x8a1b2c3d02000000ull);
}

void test_only_this_apps_games_count() {
    const std::vector<SteamShortcut> shortcuts = sample();
    std::string path;
    CHECK(dualscreen_game_of(shortcuts[0], path));
    CHECK_STR_EQ(path, "/home/deck/Emulation/roms/nds/Spirit Tracks.nds");
    CHECK(!dualscreen_game_of(shortcuts[1], path));
    CHECK(dualscreen_game_of(shortcuts[2], path));
    CHECK_STR_EQ(path, "/run/media/sd/Layton.nds");

    SteamShortcut no_game = shortcuts[0];
    no_game.launch_options = "--windowed";
    CHECK(!dualscreen_game_of(no_game, path));
}

void test_bad_bytes_are_not_shortcuts() {
    std::vector<SteamShortcut> read;
    CHECK(!parse_steam_shortcuts({}, read));
    CHECK(!parse_steam_shortcuts({0x00, 'x', 0x00, 0x08}, read));
    // Cut short: the shortcuts before the cut.
    std::vector<uint8_t> bytes = write_steam_shortcuts(sample());
    bytes.resize(bytes.size() * 2 / 3);
    CHECK(parse_steam_shortcuts(bytes, read));
    CHECK(read.size() >= 1u && read.size() < 3u);
    // Every length of it, without reading past the end.
    const std::vector<uint8_t> whole = write_steam_shortcuts(sample());
    for (std::size_t length = 0; length < whole.size(); ++length) {
        parse_steam_shortcuts(std::vector<uint8_t>(whole.begin(), whole.begin() + length), read);
    }
}

void test_each_users_file_is_found() {
    const std::filesystem::path home = std::filesystem::temp_directory_path() / "dualscreen_shortcuts_home";
    std::filesystem::remove_all(home);
    const std::filesystem::path config = home / ".local/share/Steam/userdata/12345/config";
    std::filesystem::create_directories(config);
    std::ofstream(config / "shortcuts.vdf") << "x";
    std::filesystem::create_directories(home / ".local/share/Steam/userdata/0/config");
    const std::vector<std::string> files = steam_shortcut_files(home.string());
    CHECK_EQ(files.size(), 1u);
    CHECK(!files.empty() && files[0].find("12345") != std::string::npos);
    std::filesystem::remove_all(home);
    CHECK(steam_shortcut_files(home.string()).empty());
}

// What a connected device is offered: this app's games among Steam's
// shortcuts, and the ROM folders only when there are none.
void test_the_catalog_prefers_steam() {
    const std::filesystem::path home = std::filesystem::temp_directory_path() / "dualscreen_catalog_home";
    std::filesystem::remove_all(home);
    const std::filesystem::path roms = home / "Emulation/roms";
    std::filesystem::create_directories(roms / "nds");
    std::filesystem::create_directories(roms / "n3ds");
    std::ofstream(roms / "nds/Kirby.nds") << "x";
    std::ofstream(roms / "n3ds/Zelda.3ds") << "x";
    std::ofstream(roms / "nds/readme.txt") << "x";

    std::vector<LaunchableGame> games = launchable_games(home.string(), {roms.string()});
    CHECK_EQ(games.size(), 2u);
    if (games.size() == 2u) {
        CHECK_STR_EQ(games[0].title, "Kirby");
        CHECK_STR_EQ(games[0].system, "ds");
        CHECK_STR_EQ(games[1].system, "3ds");
        CHECK_EQ(games[0].steam_run_id, 0u);
    }

    const std::filesystem::path config = home / ".local/share/Steam/userdata/42/config";
    std::filesystem::create_directories(config);
    const std::vector<uint8_t> bytes = write_steam_shortcuts(sample());
    std::ofstream(config / "shortcuts.vdf", std::ios::binary)
        .write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    games = launchable_games(home.string(), {roms.string()});
    // The hidden one and the other program's are not offered.
    CHECK_EQ(games.size(), 1u);
    if (!games.empty()) {
        CHECK_STR_EQ(games[0].title, "Zelda: Spirit Tracks");
        CHECK_STR_EQ(games[0].system, "ds");
        CHECK_EQ(games[0].steam_run_id, 0x8a1b2c3d02000000ull);
        CHECK_STR_EQ(games[0].id, "steam:" + std::to_string(0x8a1b2c3d02000000ull));
    }
    std::filesystem::remove_all(home);
}

}  // namespace

int main() {
    test_shortcuts_are_read_back();
    test_only_this_apps_games_count();
    test_bad_bytes_are_not_shortcuts();
    test_each_users_file_is_found();
    test_the_catalog_prefers_steam();
    return test::finish("steam-shortcuts");
}
