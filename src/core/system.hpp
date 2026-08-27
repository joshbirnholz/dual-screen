#pragma once

#include <string>
#include <vector>

#include "layout/layout.hpp"

namespace ds {

// Which console a game belongs to. Everything that differs between them -- the
// size and shape of each screen, the console the Standard layout draws, which
// core plays it, what its ROMs are called -- hangs off this and nowhere else.
enum class SystemId { Ds, ThreeDs, WiiU };

// What kind of OpenGL context a system's cores need. A core that renders on the
// CPU is happy with anything; one that renders on the GPU is not, and asks for
// a specific context through the libretro hardware-render callback -- refuse it
// and the core will not load the game at all.
enum class GlContextNeed {
    // Whatever the driver hands over; the renderer's own ES 2.0 baseline.
    Any,
    // A desktop OpenGL 3.3 core profile, which is what Citra asks for.
    OpenGl33Core,
};

// Where a system's frames come from. Not every console this app shows is one it
// runs: a Wii U is played in Cemu, which is a program of its own rather than a
// core this app loads, and which hands its two screens over rather than being
// asked for them.
// What one device shows when there is nobody on the other end of a layout that
// puts a screen on each. A DS's two screens are one picture between them, so
// half of it is not a game and both are shown stacked. A Wii U's are separate
// pictures -- the television alone is the whole game and the GamePad is a
// second view of it -- so one is shown and the player says which.
enum class SoloScreens { Both, One };

enum class FrameOrigin {
    LibretroCore,
    External,
};

struct SystemProfile {
    SystemId id = SystemId::Ds;
    // Shown to the player, so it is the console's name rather than an id.
    std::string name;
    // The screens and the console they sit in.
    SystemScreens screens;
    // Filename extensions that mean "this is a game for this system", lowercase
    // and without the dot.
    std::vector<std::string> rom_extensions;
    // Library filenames of the cores that can play it, most preferred first.
    std::vector<std::string> core_filenames;
    // What to call the core when telling the player one is missing.
    std::string core_description;
    // The context this system's cores need to be given.
    GlContextNeed gl_context = GlContextNeed::Any;
    // Whether the console itself has an analogue stick. One that does reads the
    // left stick as that stick and nothing else; one that does not lets the
    // stick stand in for the d-pad, since players reach for it either way.
    bool has_analog_stick = false;
    // The largest internal resolution multiplier the quick settings offer for
    // this system. 1 means the core draws at the console's own resolution and
    // there is nothing to choose, so the button does not appear at all.
    int max_render_scale = 1;
    // Whether this app runs the game itself. Everything about cores -- looking
    // for one, loading it, telling the player where to put it -- only applies
    // where this is LibretroCore.
    FrameOrigin frame_origin = FrameOrigin::LibretroCore;
    SoloScreens solo_screens = SoloScreens::Both;
    // Whether the game speaks for every control the console itself has. A Wii U
    // game reads both sticks pressed and uses its touch screen throughout, so
    // nothing this shell wants may be taken from the pad or the glass: not a
    // screen swap on L3, not smoothing on R3, not the menu on both together or
    // on a second finger. Those move to the keyboard, which the console never
    // had and which Steam Input can be pointed at.
    //
    // The cost is that a device with neither keyboard nor spare buttons has no
    // way into the menu at all. That is nobody yet -- the consoles that need
    // this are played between two desktops -- and it is the thing to solve
    // before one of them is a handheld.
    bool game_uses_every_control = false;
};

const SystemProfile& system_profile(SystemId id);
// Every system, in a stable order.
const std::vector<SystemProfile>& system_profiles();

// Which system a file belongs to, by extension. False if nothing recognises it.
bool system_for_rom(const std::string& path, SystemId& out_system);
// True if any system claims this file, which is what the browser lists.
bool is_supported_rom_for_any_system(const std::string& filename);

// Round-trips through config files and over the wire.
const char* system_key(SystemId id);
bool system_from_key(const std::string& key, SystemId& out_system);

// Where this system's core path is looked for, when it has one. Named here
// rather than spelled out where they are read, so that a console added to
// SystemId has one place to answer rather than several to be missed in.
const char* core_path_settings_key(SystemId id);
const char* core_path_environment_variable(SystemId id);

}  // namespace ds
