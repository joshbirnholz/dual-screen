#pragma once

#include <string>
#include <vector>

#include "config/paths.hpp"
#include "core/system.hpp"
#include "input/controller_mapping.hpp"
#include "input/cursor_visibility.hpp"
#include "layout/layout.hpp"
#include "video/video_codec.hpp"

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

    // The program that plays a console this app does not run itself, and the
    // arguments to start it with. Nothing here knows what that program's own
    // flags are called, so they are written out in full and two placeholders
    // are filled in: {socket} becomes the path it should hand its screens over
    // on, {game} the file the player picked.
    //
    // Empty means the player starts it themselves, which works on a desktop
    // and does not work in Game Mode -- where there is nothing to start it
    // from. See docs/cemu-integration.md.
    std::string wii_u_program;
    // The fork's own flags, so that setting the program is enough on its own.
    // See docs/cemu-integration.md.
    std::vector<std::string> wii_u_arguments{"--dualscreen-socket", "{socket}", "--game", "{game}"};

    const std::string& external_program_for(SystemId system) const {
        switch (system) {
            case SystemId::WiiU: return wii_u_program;
            // Played by a core this app loads, so there is nothing to start.
            case SystemId::Ds:
            case SystemId::ThreeDs: break;
        }
        static const std::string none;
        return none;
    }

    const std::vector<std::string>& external_arguments_for(SystemId system) const {
        switch (system) {
            case SystemId::WiiU: return wii_u_arguments;
            case SystemId::Ds:
            case SystemId::ThreeDs: break;
        }
        static const std::vector<std::string> none;
        return none;
    }

    // How screens are sent to a second device, one choice per console: tiles
    // for a DS or a 3DS, video for a Wii U, which is what the measurements in
    // docs/research-notes.md say each is worth by default -- changeable
    // independently per console specifically so one can be tried against the
    // other on real hardware without disturbing the rest. See ScreenCodec's
    // own comment for why there is no automatic fallback between them.
    video::ScreenCodec screen_codec_ds = video::ScreenCodec::Tiles;
    video::ScreenCodec screen_codec_3ds = video::ScreenCodec::Tiles;
    video::ScreenCodec screen_codec_wii_u = video::ScreenCodec::H264;

    video::ScreenCodec& screen_codec_for(SystemId system) {
        switch (system) {
            case SystemId::Ds: return screen_codec_ds;
            case SystemId::ThreeDs: return screen_codec_3ds;
            case SystemId::WiiU: return screen_codec_wii_u;
        }
        // Unreached: the switch above answers every SystemId there is, the
        // same as core_path_for()'s does. A reference has to come from
        // somewhere if a system is ever added without answering here too.
        return screen_codec_ds;
    }
    video::ScreenCodec screen_codec_for(SystemId system) const {
        return const_cast<Settings*>(this)->screen_codec_for(system);
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
    // Off by default: this loop reads input and sends it to the other device
    // in the same iteration it swaps the window (see the main loop's own
    // comment on this), so vsync does not just pace drawing here, it paces
    // input right along with it -- up to a whole display refresh added to
    // every button press and touch before it even leaves this device. Without
    // it, the loop is still correctly paced to the emulator's own frame rate
    // rather than spinning uselessly (same comment), so this is not trading
    // smooth video for a busy-loop -- only for the tear a player who wants it
    // back can still opt into from Settings.
    bool vsync = false;

    // Which of a Switch's six-axis axes is which, as an index into
    // motion_axis_candidates(), or -1 to work it out from the accelerometer.
    //
    // Temporary, and here because the working-it-out took six rounds of
    // guessing and two retractions, every round costing a build, a hardware
    // session and a report of how a game felt. A knob that can be turned on
    // the console itself while a game is running finds the answer in one
    // sitting instead. Once it is known it belongs in switch_gyro_to_sdl() as
    // a constant, and this can go.
    int switch_gyro_axes = -1;

    static Settings load(const AppPaths& paths);
    bool save(const AppPaths& paths) const;
};

// Places a melonDS DS core may be installed, most-preferred first. The
// settings' core_path, then $DUALSCREEN_CORE, take priority over these.
std::vector<std::string> core_search_paths(SystemId system);

}  // namespace ds
