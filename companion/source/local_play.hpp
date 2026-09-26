#pragma once

// Playing a game on this device, on a build that carries its own emulator.
//
// The companion joins games running elsewhere and plays nothing itself. A
// Switch, though, can run a DS game perfectly well -- just not a 3DS or a
// Wii U one -- so its build can link melonDS in (see switch/Makefile), and then
// it plays DS games here too. It does that with the desktop app: the same
// game browser, per-game options, save points, resume and quick menu a Steam
// Deck has, rather than a second copy of any of it. The companion stays what
// it is for joining.
//
// The two never share a process's lifetime. Choosing Load a game from the
// companion's menu asks the Switch's homebrew loader to start this program
// again straight into the game browser, and quitting the game asks it to start
// the menu again -- each run is one program with one window, which is what
// SDL and the console's graphics stack are built around. A game's forwarder
// starts this program with the ROM as its argument and goes straight in.
//
// DS_LOCAL_PLAY is 1 only on a build with a core linked in; everywhere else
// none of this is compiled.

#ifndef DS_LOCAL_PLAY
#define DS_LOCAL_PLAY 0
#endif

#include <string>

namespace ds::sw {

// What a run of this program was started to do, from its arguments.
struct LocalPlayRequest {
    // A game to go straight into -- a forwarder's argument.
    std::string game_path;
    // The game browser, having been sent here from the companion's menu.
    bool browse = false;
    // Carried over from this program's own options: a window rather than the
    // whole screen, and the test hooks that let a run be driven headless.
    bool windowed = false;
    bool verbose = false;
    double exit_after_seconds = 0.0;
    double screenshot_at_seconds = 0.0;
    std::string screenshot_path;

    bool wanted() const { return !game_path.empty() || browse; }
};

// The arguments the companion's own menu restarts this program with to open
// the game browser -- see relaunch().
inline constexpr const char* kBrowseArgument = "--browse";

#if DS_LOCAL_PLAY
// Plays, in this process: the desktop app, opened on `request`'s game or its
// game browser, writing to the log already open. Returns its exit status.
int play_here(const LocalPlayRequest& request);

// Asks the loader that started this program to start it again, with
// `arguments` after its own path, once this run exits. False when there is no
// loader to ask -- a build started some other way -- in which case nothing has
// been arranged and the caller has to manage without.
bool relaunch(const std::string& self_path, const std::string& arguments);
#endif

}  // namespace ds::sw
