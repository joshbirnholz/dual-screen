#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ds {

// The games Steam has as non-Steam shortcuts on this machine, read from its
// own `shortcuts.vdf` -- for a connected device to start one of this app's
// games here. Started through the shortcut, Steam applies its Steam Input
// layout for that game, which is the only say over what a button does.
//
// The file is Steam's binary key-value format: a tree of maps, strings and
// 32-bit integers, each named. Only what this needs is kept.
struct SteamShortcut {
    uint32_t appid = 0;
    std::string name;
    // As Steam keeps it: quoted, usually.
    std::string exe;
    std::string start_dir;
    std::string launch_options;
    bool hidden = false;

    // What steam://rungameid/ takes to start it: the shortcut's id in the
    // high half, and the mark of a non-Steam game.
    uint64_t run_game_id() const { return (static_cast<uint64_t>(appid) << 32) | 0x02000000u; }
};

// Every shortcut in one file's bytes. False when they are not a shortcuts
// file at all; a file cut short gives the shortcuts before the cut.
bool parse_steam_shortcuts(const std::vector<uint8_t>& bytes, std::vector<SteamShortcut>& out);

// Writes the same format back, for tests and for nothing else.
std::vector<uint8_t> write_steam_shortcuts(const std::vector<SteamShortcut>& shortcuts);

// Each Steam user's shortcuts file under a home folder, for the usual places
// Steam lives on Linux (native and Flatpak).
std::vector<std::string> steam_shortcut_files(const std::string& home);

// A game of this app's, started by a shortcut: one whose program is this app
// and whose launch options say `--game` with a path. Fills the path.
bool dualscreen_game_of(const SteamShortcut& shortcut, std::string& game_path);

}  // namespace ds
