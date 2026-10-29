#pragma once

#include <cstdint>
#include <string>
#include <utility>
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
    // When it was last played on that machine, in seconds since 1970, or 0
    // for never -- see remember_played().
    int64_t last_played = 0;
};

// Every game played on this machine, kept in the file at `record_path` by
// the game's path -- the app notes each one as it starts it -- for a
// connected device's row of recently played games.
void remember_played(const std::string& record_path, const std::string& rom_path, int64_t when);
// When each was last played, by its path.
std::vector<std::pair<std::string, int64_t>> played_games(const std::string& record_path);
// `games` with each one's last_played filled in from the record, and every
// game played that is not among them yet still there to start added, as a
// file: one opened from the file picker while Steam has shortcuts for
// others.
void add_played(std::vector<LaunchableGame>& games, const std::string& record_path);

// Every game of this app's among Steam's shortcuts under `home`, by title.
std::vector<LaunchableGame> games_from_steam_shortcuts(const std::string& home);

// Every game file in `folders` and the folders just inside them, by title:
// the console's own folder in an EmuDeck-style roms folder, say.
std::vector<LaunchableGame> games_from_folders(const std::vector<std::string>& folders);

// What to offer: the Steam shortcuts, which start a game the way the player
// set it up -- Steam Input and all -- or the ROM folders when there are none.
std::vector<LaunchableGame> launchable_games(const std::string& home, const std::vector<std::string>& folders);

}  // namespace ds
