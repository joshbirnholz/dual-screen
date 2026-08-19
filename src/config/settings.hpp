#pragma once

#include <string>
#include <vector>

#include "config/paths.hpp"

namespace ds {

// App-level defaults, stored in ~/.config/dualscreen/settings.json.
struct Settings {
    std::string default_layout_id = "vertical";

    // Directory the in-app game browser opens in. Updated as the player
    // navigates, so launching with no ROM picks up where they left off.
    std::string last_browse_dir;

    // Directory handed to the core as its libretro "system" directory; this is
    // where the user's own BIOS/firmware dumps live. Nothing Nintendo-owned is
    // bundled with this app.
    std::string system_dir;
    // Path to the melonDS DS libretro core (.so). Empty means "search the usual
    // places" — see core_search_paths().
    std::string core_path;

    float stick_deadzone = 0.35f;   // fraction of full stick travel
    float trigger_deadzone = 0.30f;
    bool vsync = true;

    static Settings load(const AppPaths& paths);
    bool save(const AppPaths& paths) const;
};

// Places a melonDS DS core may be installed, most-preferred first. The
// settings' core_path, then $DUALSCREEN_CORE, take priority over these.
std::vector<std::string> core_search_paths();

}  // namespace ds
