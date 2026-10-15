#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ds {

// A game this machine can start for a connected device: one of its Steam
// shortcuts that starts a game of this app's, or, failing any, a game file in
// one of its ROM folders.
struct LaunchableGame {
    // Names it to the machine that listed it, and nothing more: a device asks
    // for a game by this, and only a game listed is ever started.
    std::string id;
    std::string title;
    std::string system;  // system_key(): "ds", "3ds", "wiiu"
    std::string path;
    // Steam's run id for its shortcut, or 0 for a file started directly.
    uint64_t steam_run_id = 0;
};

// Every game of this app's among Steam's shortcuts under `home`, by title.
std::vector<LaunchableGame> games_from_steam_shortcuts(const std::string& home);

// Every game file in `folders` and the folders just inside them, by title:
// the console's own folder in an EmuDeck-style roms folder, say.
std::vector<LaunchableGame> games_from_folders(const std::vector<std::string>& folders);

// What to offer: the Steam shortcuts, which start a game the way the player
// set it up -- Steam Input and all -- or the ROM folders when there are none.
std::vector<LaunchableGame> launchable_games(const std::string& home, const std::vector<std::string>& folders);

}  // namespace ds
