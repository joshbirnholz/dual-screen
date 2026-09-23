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
    // Whether the quick menu offers a right-stick mapping cycle. True only for
    // a 3DS: its single physical stick can mean the Circle Pad, the C-Stick,
    // or a d-pad depending which the game wants, so which one is a real
    // question. A Wii U's GamePad has two real sticks and neither is ever
    // anything else, so there is nothing here to cycle -- has_analog_stick
    // above still applies to it, this does not.
    bool offers_right_stick_mode = false;
    // The largest internal resolution multiplier the quick settings offer for
    // this system. 1 means the core draws at the console's own resolution and
    // there is nothing to choose, so the button does not appear at all.
    int max_render_scale = 1;
    // Whether this app runs the game itself. Everything about cores -- looking
    // for one, loading it, telling the player where to put it -- only applies
    // where this is LibretroCore.
    FrameOrigin frame_origin = FrameOrigin::LibretroCore;
    SoloScreens solo_screens = SoloScreens::Both;
    // Which layout a game for this console starts on when nothing else has
    // been remembered, and what an unavailable one falls back to. Empty means
    // the registry's own fallback -- the console's own shape, which is the
    // right answer for any console this app has a picture of.
    std::string default_layout_id;
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
    // Whether the quick menu offers Reset. False for a Wii U: Cemu owns
    // restarting the game, and there is nothing here that can do it in its
    // place.
    bool offers_reset = true;
    // Whether the quick menu offers Screen Smoothing. False for a Wii U:
    // game_uses_every_control already keeps RightStick from ever becoming the
    // toggle there, so the menu's own prompt for it would be advertising a
    // control that silently does nothing.
    bool offers_smoothing = true;
    // Whether the console being played reads how the pad is being held.
    //
    // True for a Wii U, whose GamePad has a gyroscope and accelerometer that a
    // lot of its games steer, aim or look around with -- Splatoon, Wind Waker
    // HD's telescope, ZombiU, Mario Kart 8 -- and true for a 3DS, which has
    // both and games that use them, reached through libretro's own sensor
    // interface (RETRO_ENVIRONMENT_GET_SENSOR_INTERFACE in libretro_core.cpp).
    //
    // False for a DS, which has no sensors at all and no game that looks for
    // any.
    //
    // What this decides is whether a companion is asked to turn its sensors on
    // at all (SessionStateMessage::motion_wanted). A gyro left running costs
    // battery on a handheld, so a console with no use for one never starts it.
    bool uses_motion = false;
    // Whether the console's pad belongs to one device -- the one showing the
    // bottom screen -- rather than being shared between the two.
    //
    // A DS or a 3DS is one console with one set of controls, and two people
    // sharing it want either device's buttons to press them. A Wii U is not
    // like that: the GamePad is one player's controller, and every other
    // player has one of their own, which Cemu reads for itself. Merging the
    // host's pads into the GamePad there meant every Pro Controller on the
    // Steam Deck was also pressing the GamePad's buttons, underneath the
    // player actually holding it.
    bool pad_belongs_to_its_screen = false;
    // Whether this console has a microphone a game can ask to be blown into.
    //
    // A DS does, and a handful of games will not let you past a room without
    // it. The emulator can synthesise the noise they are listening for -- more
    // reliably than a person can produce it -- so what this really decides is
    // whether a button has to be found for it, which on a console with this
    // few of them means taking one back off this app.
    bool has_microphone = false;
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
