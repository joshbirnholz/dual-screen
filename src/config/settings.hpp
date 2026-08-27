#pragma once

#include <string>
#include <vector>

#include "config/paths.hpp"
#include "core/system.hpp"
#include "input/controller_mapping.hpp"
#include "input/cursor_visibility.hpp"
#include "layout/layout.hpp"

namespace ds {

// App-level defaults, stored in ~/.config/dualscreen/settings.json.
struct Settings {
    std::string default_layout_id = "standard";

    // How this device identifies itself to the other one. Defaults to the
    // machine's hostname, which is what a player will recognise in a list.
    std::string device_name;

    // Directory the in-app game browser opens in. Updated as the player
    // navigates, so launching with no ROM picks up where they left off.
    std::string last_browse_dir;

    // Directory handed to the core as its libretro "system" directory; this is
    // where the user's own BIOS/firmware dumps live. Nothing Nintendo-owned is
    // bundled with this app.
    std::string system_dir;
    // Path to the DS libretro core (.so). Empty means "search the usual places"
    // — see core_search_paths().
    std::string core_path;
    // And the same for 3DS games, which need a different core.
    std::string core_path_3ds;

    // The path this system's core should be loaded from, or empty to search.
    const std::string& core_path_for(SystemId system) const {
        switch (system) {
            case SystemId::Ds: return core_path;
            case SystemId::ThreeDs: return core_path_3ds;
            // Played in a program of its own rather than a core this app loads.
            case SystemId::WiiU: break;
        }
        // A console whose frames do not come from a core this app loads has no
        // path to hold. Reached only if one is added without answering here,
        // which the switch above will not let happen quietly.
        static const std::string none;
        return none;
    }

    // Starting point for new games; each game remembers its own afterwards.
    bool default_bilinear_filtering = false;

    // And the same for the internal render resolution. 1 means the core draws
    // at the console's own resolution; a system whose core cannot upscale
    // ignores it.
    int default_render_scale = 1;

    // Starting point for whether a layout shows the gap between its screens,
    // for whichever layouts offer the choice; each game remembers its own
    // choice afterwards.
    bool default_screen_gap = true;

    // And for what the right stick is read as, on a console that has sticks of
    // its own. Which a 3DS game wants is the game's business, so each one
    // remembers its own; this is only where a new one starts.
    RightStickMode default_right_stick_mode = RightStickMode::CStick;

    // Which device plays the sound while two are connected. Chosen from the
    // layout menu rather than here, since it is the kind of thing that changes
    // with the room rather than once at setup.
    AudioTarget audio_target = AudioTarget::Both;

    // Whether this device has a mouse pointer at all.
    CursorMode cursor_mode = CursorMode::Shown;

    // Which hand holds the stylus. Decides which side the small screen sits on
    // in the layouts that have a side.
    Handedness handedness = Handedness::Right;

    float stick_deadzone = 0.35f;   // fraction of full stick travel
    float trigger_deadzone = 0.30f;
    bool vsync = true;

    static Settings load(const AppPaths& paths);
    bool save(const AppPaths& paths) const;
};

// Places a melonDS DS core may be installed, most-preferred first. The
// settings' core_path, then $DUALSCREEN_CORE, take priority over these.
std::vector<std::string> core_search_paths(SystemId system);

}  // namespace ds
