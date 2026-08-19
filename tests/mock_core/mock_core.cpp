// A stand-in libretro core used to exercise dualscreen end to end without the
// real melonDS DS core or a commercial ROM.
//
// It declares the same core-option keys the app forces, renders a 256x384
// framebuffer whose two halves are deliberately easy to tell apart, and draws a
// crosshair wherever the frontend says the pointer is — which makes the screen
// split, the layout compositing and the touch mapping all visible in a
// screenshot.

#include "libretro/libretro.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace {

constexpr unsigned kScreenWidth = 256;
constexpr unsigned kScreenHeight = 192;
constexpr unsigned kFrameWidth = kScreenWidth;
constexpr unsigned kFrameHeight = kScreenHeight * 2;

retro_environment_t g_environment = nullptr;
retro_video_refresh_t g_video_refresh = nullptr;
retro_audio_sample_batch_t g_audio_batch = nullptr;
retro_input_poll_t g_input_poll = nullptr;
retro_input_state_t g_input_state = nullptr;

uint32_t g_framebuffer[kFrameWidth * kFrameHeight];
int16_t g_silence[1024 * 2];
unsigned g_frame_counter = 0;
bool g_layout_option_honoured = false;

// Stand-in for cartridge save RAM. The "game" stamps a magic value into it as
// it runs; on a later launch, finding that magic proves the frontend both wrote
// the save out and read it back in before the first frame.
constexpr std::size_t kSaveRamSize = 1024;
constexpr char kSaveMagic[4] = {'D', 'S', 'S', 'V'};
uint8_t g_save_ram[kSaveRamSize];
bool g_save_was_loaded = false;

const retro_core_option_v2_definition kOptions[] = {
    {"melonds_number_of_screen_layouts", "Number of layouts", nullptr,
     "How many layouts to cycle through", nullptr, "screen",
     {{"1", nullptr}, {"2", nullptr}, {nullptr, nullptr}}, "2"},
    {"melonds_screen_layout1", "Screen Layout #1", nullptr, "Layout #1", nullptr, "screen",
     {{"top-bottom", "Top/Bottom"},
      {"bottom-top", "Bottom/Top"},
      {"left-right", "Left/Right"},
      {"top", "Top Only"},
      {"bottom", "Bottom Only"},
      {nullptr, nullptr}},
     "top-bottom"},
    {"melonds_screen_gap", "Screen gap", nullptr, "Gap between screens", nullptr, "screen",
     {{"0", nullptr}, {"32", nullptr}, {nullptr, nullptr}}, "0"},
    {"melonds_render_mode", "Render mode", nullptr, nullptr, nullptr, "video",
     {{"software", "Software"}, {"opengl", "OpenGL"}, {nullptr, nullptr}}, "software"},
    {"melonds_touch_mode", "Touch mode", nullptr, nullptr, nullptr, "input",
     {{"auto", "Auto"}, {"touch", "Touch"}, {"joystick", "Joystick"}, {nullptr, nullptr}}, "auto"},
    {"melonds_show_cursor", "Show cursor", nullptr, nullptr, nullptr, "screen",
     {{"disabled", "Disabled"}, {"enabled", "Enabled"}, {nullptr, nullptr}}, "enabled"},
    {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, {{nullptr, nullptr}}, nullptr},
};

const retro_core_option_v2_category kCategories[] = {
    {"screen", "Screen", nullptr},
    {"video", "Video", nullptr},
    {"input", "Input", nullptr},
    {nullptr, nullptr, nullptr},
};

std::string read_variable(const char* key) {
    if (g_environment == nullptr) return std::string();
    retro_variable variable {};
    variable.key = key;
    variable.value = nullptr;
    if (!g_environment(RETRO_ENVIRONMENT_GET_VARIABLE, &variable) || variable.value == nullptr) {
        return std::string();
    }
    return std::string(variable.value);
}

inline uint32_t pack(uint8_t r, uint8_t g, uint8_t b) {
    return (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | b;
}

void fill_rect(int x, int y, int w, int h, uint32_t color) {
    for (int row = y; row < y + h; ++row) {
        if (row < 0 || row >= static_cast<int>(kFrameHeight)) continue;
        for (int column = x; column < x + w; ++column) {
            if (column < 0 || column >= static_cast<int>(kFrameWidth)) continue;
            g_framebuffer[static_cast<unsigned>(row) * kFrameWidth + column] = color;
        }
    }
}

// The top screen: cool blues, diagonal stripes, a bar that sweeps with time.
void draw_top_screen() {
    for (unsigned y = 0; y < kScreenHeight; ++y) {
        for (unsigned x = 0; x < kScreenWidth; ++x) {
            const bool stripe = ((x + y) / 16) % 2 == 0;
            const uint8_t blue = static_cast<uint8_t>(120 + (y * 100) / kScreenHeight);
            g_framebuffer[y * kFrameWidth + x] =
                stripe ? pack(20, 40, blue) : pack(10, 20, static_cast<uint8_t>(blue / 2));
        }
    }
    // A red border makes it obvious if the crop takes rows from the wrong half.
    fill_rect(0, 0, kScreenWidth, 3, pack(230, 60, 60));
    fill_rect(0, static_cast<int>(kScreenHeight) - 3, kScreenWidth, 3, pack(230, 60, 60));
    fill_rect(0, 0, 3, kScreenHeight, pack(230, 60, 60));
    fill_rect(static_cast<int>(kScreenWidth) - 3, 0, 3, kScreenHeight, pack(230, 60, 60));

    const int bar_x = static_cast<int>((g_frame_counter * 2) % (kScreenWidth - 24));
    fill_rect(bar_x, 20, 24, 24, pack(255, 255, 255));

    if (!g_layout_option_honoured) {
        // Loud failure state: the frontend did not pin the layout option.
        fill_rect(0, 60, kScreenWidth, 40, pack(255, 0, 255));
    }

    // Visible proof that save data survived a restart.
    if (g_save_was_loaded) {
        fill_rect(0, 60, kScreenWidth, 72, pack(255, 140, 0));
    }
}

// The bottom screen: warm greens, a checkerboard, and a crosshair at the touch
// point so touch mapping can be checked by eye.
void draw_bottom_screen() {
    const unsigned base = kScreenHeight;
    for (unsigned y = 0; y < kScreenHeight; ++y) {
        for (unsigned x = 0; x < kScreenWidth; ++x) {
            const bool check = ((x / 16) + (y / 16)) % 2 == 0;
            g_framebuffer[(base + y) * kFrameWidth + x] =
                check ? pack(30, 90, 40) : pack(15, 55, 25);
        }
    }
    fill_rect(0, static_cast<int>(base), kScreenWidth, 3, pack(60, 220, 220));
    fill_rect(0, static_cast<int>(base + kScreenHeight) - 3, kScreenWidth, 3, pack(60, 220, 220));
    fill_rect(0, static_cast<int>(base), 3, kScreenHeight, pack(60, 220, 220));
    fill_rect(static_cast<int>(kScreenWidth) - 3, static_cast<int>(base), 3, kScreenHeight,
              pack(60, 220, 220));

    if (g_input_state == nullptr) {
        return;
    }
    const int16_t pressed = g_input_state(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_PRESSED);
    if (pressed == 0) {
        return;
    }
    // libretro pointer coordinates span the whole framebuffer, exactly as the
    // real core reads them.
    const int16_t pointer_x = g_input_state(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_X);
    const int16_t pointer_y = g_input_state(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_Y);
    const int frame_x = static_cast<int>((pointer_x + 32767.0f) / 65534.0f * kFrameWidth);
    const int frame_y = static_cast<int>((pointer_y + 32767.0f) / 65534.0f * kFrameHeight);

    fill_rect(frame_x - 12, frame_y - 2, 24, 4, pack(255, 240, 80));
    fill_rect(frame_x - 2, frame_y - 12, 4, 24, pack(255, 240, 80));
}

void draw_button_indicators() {
    if (g_input_state == nullptr) return;
    const struct {
        unsigned id;
        int x;
    } kIndicators[] = {
        {RETRO_DEVICE_ID_JOYPAD_UP, 8},     {RETRO_DEVICE_ID_JOYPAD_DOWN, 28},
        {RETRO_DEVICE_ID_JOYPAD_LEFT, 48},  {RETRO_DEVICE_ID_JOYPAD_RIGHT, 68},
        {RETRO_DEVICE_ID_JOYPAD_A, 96},     {RETRO_DEVICE_ID_JOYPAD_B, 116},
        {RETRO_DEVICE_ID_JOYPAD_X, 136},    {RETRO_DEVICE_ID_JOYPAD_Y, 156},
        {RETRO_DEVICE_ID_JOYPAD_L, 184},    {RETRO_DEVICE_ID_JOYPAD_R, 204},
        {RETRO_DEVICE_ID_JOYPAD_START, 224}, {RETRO_DEVICE_ID_JOYPAD_SELECT, 240},
    };
    for (const auto& indicator : kIndicators) {
        const bool down = g_input_state(0, RETRO_DEVICE_JOYPAD, 0, indicator.id) != 0;
        fill_rect(indicator.x, static_cast<int>(kScreenHeight) - 24, 14, 14,
                  down ? pack(255, 255, 120) : pack(50, 50, 60));
    }
}

}  // namespace

extern "C" {

RETRO_API unsigned retro_api_version(void) { return RETRO_API_VERSION; }

RETRO_API void retro_set_environment(retro_environment_t callback) {
    g_environment = callback;

    bool no_game = false;
    callback(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_game);

    unsigned options_version = 0;
    if (callback(RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION, &options_version) &&
        options_version >= 2) {
        retro_core_options_v2 options {};
        options.categories = const_cast<retro_core_option_v2_category*>(kCategories);
        options.definitions = const_cast<retro_core_option_v2_definition*>(kOptions);
        callback(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2, &options);
    }
}

RETRO_API void retro_set_video_refresh(retro_video_refresh_t callback) { g_video_refresh = callback; }
RETRO_API void retro_set_audio_sample(retro_audio_sample_t) {}
RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t callback) {
    g_audio_batch = callback;
}
RETRO_API void retro_set_input_poll(retro_input_poll_t callback) { g_input_poll = callback; }
RETRO_API void retro_set_input_state(retro_input_state_t callback) { g_input_state = callback; }

RETRO_API void retro_init(void) {
    std::memset(g_framebuffer, 0, sizeof(g_framebuffer));
    std::memset(g_silence, 0, sizeof(g_silence));
    std::memset(g_save_ram, 0, sizeof(g_save_ram));
    g_frame_counter = 0;
    g_save_was_loaded = false;
}

RETRO_API void retro_deinit(void) {}

RETRO_API void retro_get_system_info(struct retro_system_info* info) {
    std::memset(info, 0, sizeof(*info));
    info->library_name = "Mock DS";
    info->library_version = "1.0";
    info->valid_extensions = "nds";
    info->need_fullpath = false;
    info->block_extract = false;
}

RETRO_API void retro_get_system_av_info(struct retro_system_av_info* info) {
    std::memset(info, 0, sizeof(*info));
    info->geometry.base_width = kFrameWidth;
    info->geometry.base_height = kFrameHeight;
    info->geometry.max_width = kFrameWidth;
    info->geometry.max_height = kFrameHeight;
    info->geometry.aspect_ratio = 0.0f;
    info->timing.fps = 59.8261;
    info->timing.sample_rate = 32823.6;
}

RETRO_API void retro_set_controller_port_device(unsigned, unsigned) {}
RETRO_API void retro_reset(void) { g_frame_counter = 0; }

RETRO_API bool retro_load_game(const struct retro_game_info*) {
    enum retro_pixel_format format = RETRO_PIXEL_FORMAT_XRGB8888;
    if (g_environment != nullptr &&
        !g_environment(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &format)) {
        return false;
    }
    // Report whether the frontend actually pinned the layout we depend on.
    g_layout_option_honoured = (read_variable("melonds_screen_layout1") == "top-bottom") &&
                               (read_variable("melonds_number_of_screen_layouts") == "1") &&
                               (read_variable("melonds_screen_gap") == "0");
    return true;
}

RETRO_API void retro_unload_game(void) {}

RETRO_API void retro_run(void) {
    if (g_input_poll != nullptr) {
        g_input_poll();
    }

    // On the first frame, report whether the frontend restored a save. The
    // frontend must have copied the file into this buffer before now.
    if (g_frame_counter == 0) {
        g_save_was_loaded = std::memcmp(g_save_ram, kSaveMagic, sizeof(kSaveMagic)) == 0;
    }
    // Then dirty the buffer, so there is something for the frontend to write.
    std::memcpy(g_save_ram, kSaveMagic, sizeof(kSaveMagic));
    g_save_ram[4] = static_cast<uint8_t>(g_frame_counter & 0xffu);
    draw_top_screen();
    draw_bottom_screen();
    draw_button_indicators();
    ++g_frame_counter;

    if (g_video_refresh != nullptr) {
        g_video_refresh(g_framebuffer, kFrameWidth, kFrameHeight, kFrameWidth * sizeof(uint32_t));
    }
    if (g_audio_batch != nullptr) {
        g_audio_batch(g_silence, 548);
    }
}

// The rest of the libretro surface, present so the frontend's symbol lookup
// succeeds; none of it is exercised by V1.
RETRO_API unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }
RETRO_API void* retro_get_memory_data(unsigned type) {
    return type == RETRO_MEMORY_SAVE_RAM ? g_save_ram : nullptr;
}
RETRO_API size_t retro_get_memory_size(unsigned type) {
    return type == RETRO_MEMORY_SAVE_RAM ? kSaveRamSize : 0;
}
RETRO_API size_t retro_serialize_size(void) { return 0; }
RETRO_API bool retro_serialize(void*, size_t) { return false; }
RETRO_API bool retro_unserialize(const void*, size_t) { return false; }
RETRO_API void retro_cheat_reset(void) {}
RETRO_API void retro_cheat_set(unsigned, bool, const char*) {}
RETRO_API bool retro_load_game_special(unsigned, const struct retro_game_info*, size_t) {
    return false;
}

}  // extern "C"
