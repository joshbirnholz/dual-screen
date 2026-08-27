#include "core/system.hpp"

#include <algorithm>
#include <cctype>

namespace ds {
namespace {

ScreenPlacement console_screen(float x, float y, float w, float h, ScreenId screen) {
    ScreenPlacement placement;
    placement.x = x;
    placement.y = y;
    placement.w = w;
    placement.h = h;
    placement.screen = screen;
    // The rects below are already each screen's exact shape, so containing them
    // inside themselves would only add a hairline of letterbox.
    placement.fit = ScreenFit::Stretch;
    return placement;
}

SystemProfile make_ds() {
    SystemProfile profile;
    profile.id = SystemId::Ds;
    profile.name = "Nintendo DS";
    profile.screens.top = ScreenSize{256, 192};
    profile.screens.bottom = ScreenSize{256, 192};

    // Measured from assets/ds_bezel.png and the overlay that came with it: the
    // screens are 256x192 at (179, 60) and (179, 337) in a 615x630 image, one
    // image pixel per DS pixel.
    ConsoleFrame& console = profile.screens.console;
    console.aspect = 615.0f / 630.0f;
    console.top = console_screen(179.0f / 615.0f, 60.0f / 630.0f, 256.0f / 615.0f,
                                 192.0f / 630.0f, ScreenId::Top);
    console.bottom = console_screen(179.0f / 615.0f, 337.0f / 630.0f, 256.0f / 615.0f,
                                    192.0f / 630.0f, ScreenId::Bottom);
    console.margin = 0.03f;
    console.has_art = true;

    profile.rom_extensions = {"nds", "dsi", "ids", "srl"};
    profile.core_filenames = {"melondsds_libretro.so", "melonds_libretro.so"};
    profile.core_description = "melonDS DS";
    return profile;
}

SystemProfile make_three_ds() {
    SystemProfile profile;
    profile.id = SystemId::ThreeDs;
    profile.name = "Nintendo 3DS";
    // The two screens are different shapes, which is the whole reason screen
    // sizes stopped being constants: 5:3 above, 4:3 below.
    profile.screens.top = ScreenSize{400, 240};
    profile.screens.bottom = ScreenSize{320, 240};

    // Measured from assets/three_ds_bezel.png and the overlay that came with
    // it, the same way the DS's was: the screens are 400x240 at (131, 66) and
    // 320x240 at (171, 391) in a 663x688 image, one image pixel per 3DS pixel.
    constexpr float kArtWidth = 663.0f;
    constexpr float kArtHeight = 688.0f;

    ConsoleFrame& console = profile.screens.console;
    console.aspect = kArtWidth / kArtHeight;
    console.top = console_screen(131.0f / kArtWidth, 66.0f / kArtHeight, 400.0f / kArtWidth,
                                 240.0f / kArtHeight, ScreenId::Top);
    console.bottom = console_screen(171.0f / kArtWidth, 391.0f / kArtHeight, 320.0f / kArtWidth,
                                    240.0f / kArtHeight, ScreenId::Bottom);
    console.margin = 0.03f;
    console.has_art = true;

    profile.rom_extensions = {"3ds", "3dsx", "cci", "cxi", "app", "elf", "axf", "cia"};
    profile.core_filenames = {"citra_libretro.so", "citra2018_libretro.so",
                              "citra_canary_libretro.so", "azahar_libretro.so"};
    profile.core_description = "Citra";
    // A 3DS is played with the circle pad, so the stick is the circle pad and
    // the d-pad is the d-pad. Sending both from one stick is what made the
    // circle pad unreachable: every core that reads the d-pad saw it pressed.
    profile.has_analog_stick = true;
    // Its one physical stick can mean the Circle Pad, the C-Stick, or a d-pad
    // -- which is a real question this console has and a Wii U does not.
    profile.offers_right_stick_mode = true;
    // Citra has no software renderer. It asks for a 3.3 core profile through
    // the hardware-render callback and refuses to load the game if it does not
    // get one, so the window has to be built for it before the core is loaded.
    profile.gl_context = GlContextNeed::OpenGl33Core;
    // Citra will go to 10x, but this is a menu to be cycled through with a
    // thumbstick on a handheld GPU: 3x already fills a 1080p screen from the
    // upper screen alone, and everything past it costs frames to no visible
    // end on the hardware this runs on.
    profile.max_render_scale = 3;
    return profile;
}

std::string lowercase_extension(const std::string& path) {
    const std::size_t dot = path.find_last_of('.');
    const std::size_t slash = path.find_last_of('/');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) {
        return std::string();
    }
    // A dotfile has no extension, it has a name that starts with a dot, so
    // ".nds" is a hidden file rather than a nameless DS ROM.
    const std::size_t name_start = (slash == std::string::npos) ? 0 : slash + 1;
    if (dot == name_start) {
        return std::string();
    }
    std::string extension = path.substr(dot + 1);
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension;
}

}  // namespace

SystemProfile make_wii_u() {
    SystemProfile profile;
    profile.id = SystemId::WiiU;
    profile.name = "Wii U";
    // The television and the GamePad. "Top" and "Bottom" are this app's names
    // for the two screens of a console rather than a claim about where they sit
    // -- on a Wii U one of them is across the room.
    profile.screens.top = ScreenSize{1280, 720};
    profile.screens.bottom = ScreenSize{854, 480};
    // No console is drawn behind these. A Wii U is a box under a television and
    // a pad in your hands, which is not a picture the Standard layout can put
    // two screens inside -- so there is no console frame and the space around
    // the screens is simply blank.

    profile.rom_extensions = {"wud", "wux", "rpx", "wua"};
    // Nothing to look for: Cemu is a program rather than a core, and it is
    // asked for its screens rather than loaded. See FrameOrigin.
    profile.core_filenames = {};
    profile.core_description = "Cemu";
    profile.frame_origin = FrameOrigin::External;
    // A GamePad has two sticks of its own, and the game reads both of them --
    // including pressing them, which is why nothing in this shell may take L3
    // or R3 while a Wii U game is running.
    profile.has_analog_stick = true;
    profile.game_uses_every_control = true;
    // The television is the game; the GamePad is a second view of it. So one
    // device on its own shows one screen rather than both squeezed together.
    profile.prefers_video = true;
    profile.solo_screens = SoloScreens::One;
    // One screen on each device is the Wii U's own arrangement, so it is where
    // a game starts rather than something to go and find. Alone, that shows
    // the television and the swap key reaches the GamePad -- see SoloScreens.
    profile.default_layout_id = "upper_on_tv";
    // Cemu has its own resolution settings and they are not this app's to
    // second-guess, so there is nothing here to cycle through.
    profile.max_render_scale = 1;
    // Neither of these is this shell's to offer for a Wii U -- see the fields'
    // own comments.
    profile.offers_reset = false;
    profile.offers_smoothing = false;
    return profile;
}

const std::vector<SystemProfile>& system_profiles() {
    static const std::vector<SystemProfile> profiles = {make_ds(), make_three_ds(), make_wii_u()};
    return profiles;
}

const SystemProfile& system_profile(SystemId id) {
    for (const SystemProfile& profile : system_profiles()) {
        if (profile.id == id) {
            return profile;
        }
    }
    return system_profiles().front();
}

bool system_for_rom(const std::string& path, SystemId& out_system) {
    const std::string extension = lowercase_extension(path);
    if (extension.empty()) {
        return false;
    }
    for (const SystemProfile& profile : system_profiles()) {
        for (const std::string& candidate : profile.rom_extensions) {
            if (candidate == extension) {
                out_system = profile.id;
                return true;
            }
        }
    }
    return false;
}

bool is_supported_rom_for_any_system(const std::string& filename) {
    SystemId ignored = SystemId::Ds;
    return system_for_rom(filename, ignored);
}

// Every one of these is a switch with no default on purpose. A ternary would
// hand a console nobody had thought about whatever the DS gets, silently and
// everywhere; this way adding one to SystemId stops the build until each place
// has been given an answer.
const char* system_key(SystemId id) {
    switch (id) {
        case SystemId::Ds: return "ds";
        case SystemId::ThreeDs: return "3ds";
        case SystemId::WiiU: return "wiiu";
    }
    return "ds";
}

const char* core_path_settings_key(SystemId id) {
    switch (id) {
        case SystemId::Ds: return "core_path";
        case SystemId::ThreeDs: return "core_path_3ds";
        // A console this app does not run has no core path to name.
        case SystemId::WiiU: return "";
    }
    return "core_path";
}

const char* core_path_environment_variable(SystemId id) {
    switch (id) {
        case SystemId::Ds: return "DUALSCREEN_CORE";
        case SystemId::ThreeDs: return "DUALSCREEN_CORE_3DS";
        case SystemId::WiiU: return "";
    }
    return "DUALSCREEN_CORE";
}

bool system_from_key(const std::string& key, SystemId& out_system) {
    if (key == "ds") {
        out_system = SystemId::Ds;
        return true;
    }
    if (key == "3ds") {
        out_system = SystemId::ThreeDs;
        return true;
    }
    if (key == "wiiu") {
        out_system = SystemId::WiiU;
        return true;
    }
    return false;
}

}  // namespace ds
