// SPDX-License-Identifier: GPL-3.0-or-later
//
// The libretro interface, over RSDuck's melonDS. What it offers is shaped
// after libretro's own melonDS, the core the Switch app linked before this one,
// so the app treats the two alike: the same option names (the few the app
// sets), the same buttons -- L2 blows into the microphone, L3 closes the lid --
// the touch screen through the pointer, and the cartridge save in the same
// place, <save directory>/<game>.sav, which melonDS keeps up to date itself.

#include "melonds_deko.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "libretro.h"

#include "Config.h"
#include "GPU.h"
#include "NDS.h"
#include "Platform.h"
#include "SPU.h"
#include "Savestate.h"
#include "frontend/FrontendUtil.h"
#include "frontend/switch/Gfx.h"

namespace melonds_deko {

namespace {

constexpr unsigned kWidth = 256;
constexpr unsigned kHeight = 384;  // the upper screen above the touch screen
// The DS's own refresh and sample rates: a 33.5 MHz bus, 263 lines of 355 dots
// at 6 cycles, and the rate its sound mixer runs at.
constexpr double kFramesPerSecond = 59.8261;
constexpr double kSampleRate = 32823.6;

retro_environment_t g_environment = nullptr;
retro_video_refresh_t g_video = nullptr;
retro_audio_sample_batch_t g_audio = nullptr;
retro_input_poll_t g_input_poll = nullptr;
retro_input_state_t g_input_state = nullptr;
retro_log_printf_t g_log = nullptr;

uint32_t g_framebuffer[kWidth * kHeight];
bool g_loaded = false;
bool g_lid_closed = false;
std::string g_rom_path;
std::string g_sram_path;
std::vector<uint8_t> g_state_scratch;

void log_at(retro_log_level level, const std::string& message) {
    if (g_log != nullptr) {
        g_log(level, "%s\n", message.c_str());
    } else {
        std::fprintf(stderr, "%s\n", message.c_str());
    }
}

std::string directory_from(unsigned command) {
    const char* directory = nullptr;
    if (g_environment != nullptr && g_environment(command, &directory) && directory != nullptr) {
        return directory;
    }
    return std::string();
}

std::string stem_of(const std::string& path) {
    const std::size_t slash = path.find_last_of("/\\");
    std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    const std::size_t dot = name.find_last_of('.');
    return dot == std::string::npos ? name : name.substr(0, dot);
}

// BIOS and firmware, which this melonDS cannot do without: it has none of its
// own to fall back on, and boots nothing without all three. Looked for the way
// Platform::OpenLocalFile() looks, and set as full paths so there is no doubt
// afterwards which files were used.
bool find_bios(std::string* missing) {
    struct Wanted {
        const char* name;
        char* path;
    };
    const Wanted wanted[] = {
        {"bios9.bin", Config::BIOS9Path},
        {"bios7.bin", Config::BIOS7Path},
        {"firmware.bin", Config::FirmwarePath},
    };
    const std::string system = directory_from(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY);
    bool all = true;
    for (const Wanted& file : wanted) {
        std::string found;
        for (const std::string& directory :
             {system, std::string("/switch/melonds"), std::string("/melonds")}) {
            if (directory.empty()) continue;
            const std::string candidate = directory + "/" + file.name;
            if (std::FILE* probe = std::fopen(candidate.c_str(), "rb")) {
                std::fclose(probe);
                found = candidate;
                break;
            }
        }
        if (found.empty()) {
            all = false;
            if (missing != nullptr) *missing += (missing->empty() ? "" : ", ") + std::string(file.name);
            file.path[0] = '\0';
        } else {
            std::snprintf(file.path, 1024, "%s", found.c_str());
            log_info(std::string("melonDS (deko3d): ") + file.name + " from " + found);
        }
    }
    return all;
}

void declare_options() {
    // Only what the app sets, and only the values it sets them to: this core
    // does one layout, one kind of touch and one kind of microphone, and
    // declaring those is what lets the app see its settings were taken.
    static retro_core_option_v2_category categories[] = {{nullptr, nullptr, nullptr}};
    static retro_core_option_v2_definition definitions[] = {
        {"melonds_screen_layout", "Screen layout", nullptr, nullptr, nullptr, nullptr,
         {{"Top/Bottom", nullptr}, {nullptr, nullptr}}, "Top/Bottom"},
        {"melonds_screen_gap", "Screen gap", nullptr, nullptr, nullptr, nullptr,
         {{"0", nullptr}, {nullptr, nullptr}}, "0"},
        {"melonds_touch_mode", "Touch mode", nullptr, nullptr, nullptr, nullptr,
         {{"Touch", nullptr}, {nullptr, nullptr}}, "Touch"},
        {"melonds_mic_input", "Microphone input", nullptr, nullptr, nullptr, nullptr,
         {{"Blow Noise", nullptr}, {nullptr, nullptr}}, "Blow Noise"},
        {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, {{nullptr, nullptr}}, nullptr},
    };
    static retro_core_options_v2 options = {categories, definitions};
    unsigned version = 0;
    if (g_environment(RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION, &version) && version >= 2) {
        g_environment(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2, &options);
    }
}

void declare_inputs() {
    static const retro_input_descriptor descriptors[] = {
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT, "Left"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP, "Up"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN, "Down"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "Right"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A, "A"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B, "B"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Select"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Start"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R, "R"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L, "L"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X, "X"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y, "Y"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2, "Make microphone noise"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3, "Close lid"},
        {0, 0, 0, 0, nullptr},
    };
    g_environment(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, const_cast<retro_input_descriptor*>(descriptors));
}

bool pressed(unsigned id) {
    return g_input_state != nullptr && g_input_state(0, RETRO_DEVICE_JOYPAD, 0, id) != 0;
}

void apply_input() {
    if (g_input_poll != nullptr) g_input_poll();

    // The DS's own key order, and inverted: a set bit is a key that is up.
    static const unsigned kKeys[] = {
        RETRO_DEVICE_ID_JOYPAD_A,     RETRO_DEVICE_ID_JOYPAD_B,    RETRO_DEVICE_ID_JOYPAD_SELECT,
        RETRO_DEVICE_ID_JOYPAD_START, RETRO_DEVICE_ID_JOYPAD_RIGHT, RETRO_DEVICE_ID_JOYPAD_LEFT,
        RETRO_DEVICE_ID_JOYPAD_UP,    RETRO_DEVICE_ID_JOYPAD_DOWN, RETRO_DEVICE_ID_JOYPAD_R,
        RETRO_DEVICE_ID_JOYPAD_L,     RETRO_DEVICE_ID_JOYPAD_X,    RETRO_DEVICE_ID_JOYPAD_Y,
    };
    u32 mask = 0xFFF;
    for (unsigned bit = 0; bit < 12; ++bit) {
        if (pressed(kKeys[bit])) mask &= ~(1u << bit);
    }
    NDS::SetKeyMask(mask);

    const bool lid = pressed(RETRO_DEVICE_ID_JOYPAD_L3);
    if (lid != g_lid_closed) {
        g_lid_closed = lid;
        NDS::SetLidClosed(lid);
    }

    if (pressed(RETRO_DEVICE_ID_JOYPAD_L2)) {
        Frontend::Mic_FeedNoise();
    } else {
        Frontend::Mic_FeedSilence();
    }

    // The pointer spans the whole 256x384 picture; the lower half is the
    // touch screen.
    bool touching = false;
    if (g_input_state != nullptr &&
        g_input_state(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_PRESSED) != 0) {
        const int pointer_x = g_input_state(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_X);
        const int pointer_y = g_input_state(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_Y);
        const int x = (pointer_x + 0x8000) * static_cast<int>(kWidth) / 0x10000;
        const int y = (pointer_y + 0x8000) * static_cast<int>(kHeight) / 0x10000 - 192;
        if (x >= 0 && x < 256 && y >= 0 && y < 192) {
            touching = true;
            NDS::TouchScreen(static_cast<u16>(x), static_cast<u16>(y));
        }
    }
    if (!touching) NDS::ReleaseScreen();
}

void send_audio() {
    if (g_audio == nullptr) return;
    static int16_t samples[1024 * 2];
    for (;;) {
        const int available = std::min(SPU::GetOutputSize(), 1024);
        if (available <= 0) break;
        const int read = SPU::ReadOutput(samples, available);
        if (read <= 0) break;
        g_audio(samples, static_cast<size_t>(read));
    }
}

// Writes the running game's state to the scratch buffer, returning its size.
std::size_t save_state_to_scratch() {
    if (g_state_scratch.empty()) g_state_scratch.resize(16u << 20);
    state_target(g_state_scratch.data(), g_state_scratch.size());
    {
        Savestate state(kStateStreamName, true);
        if (state.Error) {
            log_error("melonDS (deko3d): could not start a save state");
            return 0;
        }
        NDS::DoSavestate(&state);
    }
    // Savestate fills in its total length at offset 8 when it closes.
    uint32_t length = 0;
    std::memcpy(&length, g_state_scratch.data() + 8, sizeof(length));
    return std::min<std::size_t>(length, g_state_scratch.size());
}

}  // namespace

void log_info(const std::string& message) { log_at(RETRO_LOG_INFO, message); }
void log_error(const std::string& message) { log_at(RETRO_LOG_ERROR, message); }

}  // namespace melonds_deko

using namespace melonds_deko;

RETRO_API unsigned retro_api_version(void) { return RETRO_API_VERSION; }

RETRO_API void retro_set_environment(retro_environment_t environment) {
    g_environment = environment;
    bool no_game = false;
    environment(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_game);
    retro_log_callback log{};
    if (environment(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log)) g_log = log.log;
    declare_options();
}

RETRO_API void retro_set_video_refresh(retro_video_refresh_t callback) { g_video = callback; }
RETRO_API void retro_set_audio_sample(retro_audio_sample_t) {}
RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t callback) { g_audio = callback; }
RETRO_API void retro_set_input_poll(retro_input_poll_t callback) { g_input_poll = callback; }
RETRO_API void retro_set_input_state(retro_input_state_t callback) { g_input_state = callback; }
RETRO_API void retro_set_controller_port_device(unsigned, unsigned) {}

RETRO_API void retro_get_system_info(struct retro_system_info* info) {
    std::memset(info, 0, sizeof(*info));
    info->library_name = "melonDS (deko3d)";
    info->library_version = "RSDuck switch-new";
    info->valid_extensions = "nds|dsi|srl";
    info->need_fullpath = true;
    info->block_extract = true;
}

RETRO_API void retro_get_system_av_info(struct retro_system_av_info* info) {
    std::memset(info, 0, sizeof(*info));
    info->geometry.base_width = kWidth;
    info->geometry.base_height = kHeight;
    info->geometry.max_width = kWidth;
    info->geometry.max_height = kHeight;
    info->geometry.aspect_ratio = static_cast<float>(kWidth) / static_cast<float>(kHeight);
    info->timing.fps = kFramesPerSecond;
    info->timing.sample_rate = kSampleRate;
}

RETRO_API void retro_init(void) {}
RETRO_API void retro_deinit(void) {}

RETRO_API bool retro_load_game(const struct retro_game_info* game) {
    if (game == nullptr || game->path == nullptr) return false;

    retro_pixel_format format = RETRO_PIXEL_FORMAT_XRGB8888;
    if (!g_environment(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &format)) {
        log_error("melonDS (deko3d): the frontend will not take XRGB8888 pictures");
        return false;
    }
    declare_inputs();
    set_system_directory(directory_from(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY));

    std::string missing;
    if (!find_bios(&missing)) {
        log_error("melonDS (deko3d) needs a DS's BIOS and firmware and has no substitute for "
                  "them: " + missing + " not found in the system directory or /switch/melonds");
        return false;
    }

    std::string error;
    if (!gfx_init(&error)) {
        log_error("melonDS (deko3d): " + error);
        return false;
    }

    // Before NDS::Init(), which sets the JIT up from these.
    Config::JIT_Enable = 1;
    Config::JIT_MaxBlockSize = 32;
    Config::JIT_BranchOptimisations = 1;
    Config::JIT_LiteralOptimisations = 1;
    Config::JIT_FastMemory = 1;
    Config::DLDIEnable = 0;
    Config::DSiSDEnable = 0;
    Config::RandomizeMAC = 0;

    if (!NDS::Init()) {
        log_error("melonDS (deko3d): the emulator would not initialise");
        gfx_deinit();
        return false;
    }
    GPU::InitRenderer(0);
    GPU::RenderSettings settings{true, 1, false};
    GPU::SetRenderSettings(0, settings);

    g_rom_path = game->path;
    const std::string saves = directory_from(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY);
    // melonDS opens the save with a plain fopen(), which cannot make the
    // directory it goes in.
    if (!saves.empty()) mkdir(saves.c_str(), 0777);
    g_sram_path = (saves.empty() ? std::string(".") : saves) + "/" + stem_of(g_rom_path) + ".sav";

    NDS::SetConsoleType(0);
    if (!NDS::LoadROM(g_rom_path.c_str(), g_sram_path.c_str(), true)) {
        log_error("melonDS (deko3d): could not load " + g_rom_path);
        NDS::DeInit();
        gfx_deinit();
        return false;
    }
    log_info("melonDS (deko3d): playing " + g_rom_path + ", saving to " + g_sram_path);
    g_lid_closed = false;
    g_loaded = true;
    return true;
}

RETRO_API bool retro_load_game_special(unsigned, const struct retro_game_info*, size_t) { return false; }

RETRO_API void retro_unload_game(void) {
    if (!g_loaded) return;
    // Nothing of a frame may still be in flight when the renderers free the
    // memory it uses. NDS::DeInit() also writes the cartridge save out.
    Gfx::EmuQueue.waitIdle();
    NDS::DeInit();
    gfx_deinit();
    g_loaded = false;
}

RETRO_API void retro_reset(void) {
    if (!g_loaded) return;
    // melonDS's own frontend resets a game by loading it again.
    NDS::LoadROM(g_rom_path.c_str(), g_sram_path.c_str(), true);
}

RETRO_API void retro_run(void) {
    if (!g_loaded) return;
    apply_input();
    NDS::RunFrame();
    gfx_read_screens(g_framebuffer);
    if (g_video != nullptr) g_video(g_framebuffer, kWidth, kHeight, kWidth * sizeof(uint32_t));
    send_audio();
}

RETRO_API unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }

// The cartridge save is melonDS's own to keep, in g_sram_path: it writes it
// out as the game saves, as libretro's melonDS did. So there is nothing here
// for the frontend to keep as well.
RETRO_API void* retro_get_memory_data(unsigned) { return nullptr; }
RETRO_API size_t retro_get_memory_size(unsigned) { return 0; }

RETRO_API size_t retro_serialize_size(void) {
    if (!g_loaded) return 0;
    return save_state_to_scratch();
}

RETRO_API bool retro_serialize(void* data, size_t size) {
    if (!g_loaded || data == nullptr) return false;
    const std::size_t length = save_state_to_scratch();
    if (length == 0 || length > size) return false;
    std::memcpy(data, g_state_scratch.data(), length);
    return true;
}

RETRO_API bool retro_unserialize(const void* data, size_t size) {
    if (!g_loaded || data == nullptr || size < 16) return false;
    state_source(static_cast<const uint8_t*>(data), size);
    bool ok = false;
    {
        Savestate state(kStateStreamName, false);
        if (!state.Error) {
            NDS::DoSavestate(&state);
            ok = true;
        }
    }
    state_source(nullptr, 0);
    if (!ok) log_error("melonDS (deko3d): that save state could not be read");
    return ok;
}

RETRO_API void retro_cheat_reset(void) {}
RETRO_API void retro_cheat_set(unsigned, bool, const char*) {}
