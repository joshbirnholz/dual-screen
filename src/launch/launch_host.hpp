#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "launch/game_catalog.hpp"

namespace ds {

// The PC end of starting a game from a connected device, run on its own as
// `dualscreen --service`: no window, just LaunchService answering until it is
// told to stop. Returns the process's exit status.
int run_launch_service();

// Starts one of the games LaunchService lists, apart from this process so it
// outlives it: a Steam shortcut through Steam, so its Steam Input layout
// applies, and a file by running this app on it.
bool launch_game_here(const LaunchableGame& game, std::string* error);

// A game's icon, read out of the game itself -- a DS banner, a 3DS SMDH, a
// Wii U meta folder -- for a device's grid of games.
bool launchable_game_icon(const LaunchableGame& game, int& width, int& height, std::vector<uint8_t>& rgba);

// This program's own path, as a player would start it again: the AppImage
// rather than where it is mounted, when it is one.
std::string this_program();

// Whether the service starts at login -- a systemd user unit, which runs in
// Game Mode as well as on the desktop -- and turning that on or off. On also
// starts it now; off stops it.
bool service_starts_at_login();
bool set_service_starts_at_login(bool on, std::string* error);

}  // namespace ds
