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
    console.native_width = 615.0f;
    console.native_height = 630.0f;
    console.native_max_output_width = 1280.0f;
    console.native_max_output_height = 800.0f;
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
    console.native_width = kArtWidth;
    console.native_height = kArtHeight;
    // One image pixel per 3DS pixel, so a display this size or smaller gets the
    // artwork at its own size and both screens land exactly one-to-one.
    console.native_max_output_width = 1280.0f;
    console.native_max_output_height = 800.0f;
    console.has_art = true;

    profile.rom_extensions = {"3ds", "3dsx", "cci", "cxi", "app", "elf", "axf", "cia"};
    profile.core_filenames = {"citra_libretro.so", "citra2018_libretro.so",
                              "citra_canary_libretro.so", "azahar_libretro.so"};
    profile.core_description = "Citra";
    // A 3DS is played with the circle pad, so the stick is the circle pad and
    // the d-pad is the d-pad. Sending both from one stick is what made the
    // circle pad unreachable: every core that reads the d-pad saw it pressed.
    profile.has_analog_stick = true;
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

const std::vector<SystemProfile>& system_profiles() {
    static const std::vector<SystemProfile> profiles = {make_ds(), make_three_ds()};
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

const char* system_key(SystemId id) {
    return id == SystemId::ThreeDs ? "3ds" : "ds";
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
    return false;
}

}  // namespace ds
