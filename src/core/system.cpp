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

    // No artwork yet, so the console frame is built from the machine's own
    // measurements instead: 134 x 74 mm per half, an upper screen with a
    // 76.8 x 46.08 mm active area and a lower one of 61.44 x 46.08 mm, each
    // centred in its half. The screens land where a 3DS's do; the space around
    // them is simply blank until there is a picture to put behind it.
    constexpr float kBodyWidth = 134.0f;
    constexpr float kBodyHeight = 74.0f * 2.0f;
    constexpr float kTopScreenWidth = 76.8f;
    constexpr float kBottomScreenWidth = 61.44f;
    constexpr float kScreenHeight = 46.08f;

    ConsoleFrame& console = profile.screens.console;
    console.aspect = kBodyWidth / kBodyHeight;
    console.top = console_screen((kBodyWidth - kTopScreenWidth) * 0.5f / kBodyWidth,
                                 0.25f - kScreenHeight * 0.5f / kBodyHeight,
                                 kTopScreenWidth / kBodyWidth, kScreenHeight / kBodyHeight,
                                 ScreenId::Top);
    console.bottom = console_screen((kBodyWidth - kBottomScreenWidth) * 0.5f / kBodyWidth,
                                    0.75f - kScreenHeight * 0.5f / kBodyHeight,
                                    kBottomScreenWidth / kBodyWidth, kScreenHeight / kBodyHeight,
                                    ScreenId::Bottom);
    console.margin = 0.03f;
    console.has_art = false;

    profile.rom_extensions = {"3ds", "3dsx", "cci", "cxi", "app", "elf", "axf", "cia"};
    profile.core_filenames = {"citra_libretro.so", "citra2018_libretro.so",
                              "citra_canary_libretro.so", "azahar_libretro.so"};
    profile.core_description = "Citra";
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
