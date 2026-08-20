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
#include <cstdio>
#include <cstring>
#include <string>

namespace {

// Built twice from this one source: once shaped like a DS, once like a 3DS,
// whose two screens are different widths. The 3DS build is what checks that
// nothing in the frontend still assumes the screens match.
#ifdef MOCK_CORE_3DS
constexpr unsigned kTopWidth = 400;
constexpr unsigned kTopHeight = 240;
constexpr unsigned kBottomWidth = 320;
constexpr unsigned kBottomHeight = 240;
constexpr bool kIsThreeDs = true;
#else
constexpr unsigned kTopWidth = 256;
constexpr unsigned kTopHeight = 192;
constexpr unsigned kBottomWidth = 256;
constexpr unsigned kBottomHeight = 192;
constexpr bool kIsThreeDs = false;
#endif

// Stacked with no gap, each screen centred across the buffer -- the arrangement
// both real cores are pinned to.
constexpr unsigned kFrameWidth = kTopWidth > kBottomWidth ? kTopWidth : kBottomWidth;
constexpr unsigned kFrameHeight = kTopHeight + kBottomHeight;
constexpr unsigned kTopX = (kFrameWidth - kTopWidth) / 2;
constexpr unsigned kBottomX = (kFrameWidth - kBottomWidth) / 2;

// The 3DS build renders on the GPU, the way Citra does: it asks the frontend
// for an OpenGL context and a framebuffer, draws into that framebuffer, and
// reports RETRO_HW_FRAME_BUFFER_VALID instead of handing over pixels. Without
// this the hardware-render path in the frontend would never be exercised, and
// that path is the whole reason a 3DS core loads at all.
constexpr bool kUsesHwRender = kIsThreeDs;

retro_environment_t g_environment = nullptr;
retro_video_refresh_t g_video_refresh = nullptr;
retro_audio_sample_batch_t g_audio_batch = nullptr;
retro_input_poll_t g_input_poll = nullptr;
retro_input_state_t g_input_state = nullptr;

uint32_t g_framebuffer[kFrameWidth * kFrameHeight];

// --- the GPU path ---------------------------------------------------------
// Everything below is resolved through the frontend's get_proc_address, which
// is the only way a libretro core is allowed to reach GL.
struct retro_hw_render_callback g_hw_render;
bool g_hw_ready = false;

using GLenum_ = unsigned;
using GLuint_ = unsigned;
using GLint_ = int;
using GLsizei_ = int;
using GLbitfield_ = unsigned;

void (*gl_GenTextures)(GLsizei_, GLuint_*) = nullptr;
void (*gl_DeleteTextures)(GLsizei_, const GLuint_*) = nullptr;
void (*gl_BindTexture)(GLenum_, GLuint_) = nullptr;
void (*gl_TexImage2D)(GLenum_, GLint_, GLint_, GLsizei_, GLsizei_, GLint_, GLenum_, GLenum_,
                      const void*) = nullptr;
void (*gl_TexParameteri)(GLenum_, GLenum_, GLint_) = nullptr;
void (*gl_GenFramebuffers)(GLsizei_, GLuint_*) = nullptr;
void (*gl_DeleteFramebuffers)(GLsizei_, const GLuint_*) = nullptr;
void (*gl_BindFramebuffer)(GLenum_, GLuint_) = nullptr;
void (*gl_FramebufferTexture2D)(GLenum_, GLenum_, GLenum_, GLuint_, GLint_) = nullptr;
void (*gl_BlitFramebuffer)(GLint_, GLint_, GLint_, GLint_, GLint_, GLint_, GLint_, GLint_,
                           GLbitfield_, GLenum_) = nullptr;

constexpr GLenum_ kGlTexture2d = 0x0DE1;
constexpr GLenum_ kGlRgba = 0x1908;
constexpr GLenum_ kGlBgra = 0x80E1;
constexpr GLenum_ kGlUnsignedByte = 0x1401;
constexpr GLenum_ kGlNearest = 0x2600;
constexpr GLenum_ kGlTextureMinFilter = 0x2801;
constexpr GLenum_ kGlTextureMagFilter = 0x2800;
constexpr GLenum_ kGlReadFramebuffer = 0x8CA8;
constexpr GLenum_ kGlDrawFramebuffer = 0x8CA9;
constexpr GLenum_ kGlFramebuffer = 0x8D40;
constexpr GLenum_ kGlColorAttachment0 = 0x8CE0;
constexpr GLbitfield_ kGlColorBufferBit = 0x00004000;

GLuint_ g_gpu_texture = 0;
GLuint_ g_gpu_framebuffer = 0;

template <typename T>
void resolve_gl(T& out, const char* name) {
    if (g_hw_render.get_proc_address == nullptr) return;
    out = reinterpret_cast<T>(g_hw_render.get_proc_address(name));
}

void RETRO_CALLCONV hw_context_reset() {
    resolve_gl(gl_GenTextures, "glGenTextures");
    resolve_gl(gl_DeleteTextures, "glDeleteTextures");
    resolve_gl(gl_BindTexture, "glBindTexture");
    resolve_gl(gl_TexImage2D, "glTexImage2D");
    resolve_gl(gl_TexParameteri, "glTexParameteri");
    resolve_gl(gl_GenFramebuffers, "glGenFramebuffers");
    resolve_gl(gl_DeleteFramebuffers, "glDeleteFramebuffers");
    resolve_gl(gl_BindFramebuffer, "glBindFramebuffer");
    resolve_gl(gl_FramebufferTexture2D, "glFramebufferTexture2D");
    resolve_gl(gl_BlitFramebuffer, "glBlitFramebuffer");

    g_hw_ready = gl_GenTextures != nullptr && gl_BlitFramebuffer != nullptr &&
                 gl_GenFramebuffers != nullptr && gl_TexImage2D != nullptr;
    if (!g_hw_ready) return;

    gl_GenTextures(1, &g_gpu_texture);
    gl_BindTexture(kGlTexture2d, g_gpu_texture);
    gl_TexParameteri(kGlTexture2d, kGlTextureMinFilter, static_cast<GLint_>(kGlNearest));
    gl_TexParameteri(kGlTexture2d, kGlTextureMagFilter, static_cast<GLint_>(kGlNearest));
    gl_TexImage2D(kGlTexture2d, 0, static_cast<GLint_>(kGlRgba), kFrameWidth, kFrameHeight, 0,
                  kGlBgra, kGlUnsignedByte, nullptr);
    gl_BindTexture(kGlTexture2d, 0);

    gl_GenFramebuffers(1, &g_gpu_framebuffer);
    gl_BindFramebuffer(kGlFramebuffer, g_gpu_framebuffer);
    gl_FramebufferTexture2D(kGlFramebuffer, kGlColorAttachment0, kGlTexture2d, g_gpu_texture, 0);
    gl_BindFramebuffer(kGlFramebuffer, 0);
}

void RETRO_CALLCONV hw_context_destroy() {
    if (g_gpu_framebuffer != 0 && gl_DeleteFramebuffers != nullptr) {
        gl_DeleteFramebuffers(1, &g_gpu_framebuffer);
    }
    if (g_gpu_texture != 0 && gl_DeleteTextures != nullptr) {
        gl_DeleteTextures(1, &g_gpu_texture);
    }
    g_gpu_framebuffer = 0;
    g_gpu_texture = 0;
    g_hw_ready = false;
}

// Puts the frame that was just drawn on the CPU into the framebuffer the
// frontend handed over. A real GPU core would render straight in; this only
// has to prove the handover works.
void present_through_gpu() {
    if (!g_hw_ready) return;

    // XRGB8888 in a uint32 is B,G,R,X in memory, so it goes up as BGRA and
    // lands in the texture in the right order.
    gl_BindTexture(kGlTexture2d, g_gpu_texture);
    gl_TexImage2D(kGlTexture2d, 0, static_cast<GLint_>(kGlRgba), kFrameWidth, kFrameHeight, 0,
                  kGlBgra, kGlUnsignedByte, g_framebuffer);
    gl_BindTexture(kGlTexture2d, 0);

    const GLuint_ target = g_hw_render.get_current_framebuffer != nullptr
                               ? static_cast<GLuint_>(g_hw_render.get_current_framebuffer())
                               : 0;
    gl_BindFramebuffer(kGlReadFramebuffer, g_gpu_framebuffer);
    gl_BindFramebuffer(kGlDrawFramebuffer, target);
    // Flipped on the way over: the frame is stored top row first, and this core
    // declares bottom_left_origin, so the frontend expects the bottom row at
    // y = 0.
    gl_BlitFramebuffer(0, 0, kFrameWidth, kFrameHeight, 0, kFrameHeight, kFrameWidth, 0,
                       kGlColorBufferBit, kGlNearest);
    gl_BindFramebuffer(kGlReadFramebuffer, 0);
    gl_BindFramebuffer(kGlDrawFramebuffer, 0);
}

int16_t g_silence[1024 * 2];
unsigned g_frame_counter = 0;
bool g_layout_option_honoured = false;
int16_t g_reported_circle_x = 0;
int16_t g_reported_circle_y = 0;

// Stand-in for cartridge save RAM. The "game" stamps a magic value into it as
// it runs; on a later launch, finding that magic proves the frontend both wrote
// the save out and read it back in before the first frame.
constexpr std::size_t kSaveRamSize = 1024;
constexpr char kSaveMagic[4] = {'D', 'S', 'S', 'V'};
uint8_t g_save_ram[kSaveRamSize];
bool g_save_was_loaded = false;

const retro_core_option_v2_definition kOptions[] = {
    {"citra_layout_option", "Screen layout", nullptr, "How the two screens are arranged", nullptr,
     "screen",
     {{"Default Top-Bottom Screen", nullptr},
      {"Single Screen Only", nullptr},
      {"Side by Side", nullptr},
      {nullptr, nullptr}},
     "Default Top-Bottom Screen"},
    // Values copied from Citra's own option table, not invented. An earlier
    // version of this mock guessed "disabled|enabled" here, so the frontend
    // forcing "disabled" passed every test and was rejected by the real core.
    {"citra_swap_screen", "Prominent 3DS screen", nullptr, nullptr, nullptr, "screen",
     {{"Top", nullptr}, {"Bottom", nullptr}, {nullptr, nullptr}}, "Top"},
    {"citra_touch_touchscreen", "Simulate touchscreen interactions with touchscreen", nullptr,
     nullptr, nullptr, "screen",
     {{"disabled", nullptr}, {"enabled", nullptr}, {nullptr, nullptr}}, "disabled"},
    {"citra_render_touchscreen", "Render simulated touchscreen interactions", nullptr, nullptr,
     nullptr, "screen",
     {{"disabled", nullptr}, {"enabled", nullptr}, {nullptr, nullptr}}, "disabled"},
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
    for (unsigned y = 0; y < kTopHeight; ++y) {
        for (unsigned x = 0; x < kTopWidth; ++x) {
            const bool stripe = ((x + y) / 16) % 2 == 0;
            const uint8_t blue = static_cast<uint8_t>(120 + (y * 100) / kTopHeight);
            g_framebuffer[y * kFrameWidth + kTopX + x] =
                stripe ? pack(20, 40, blue) : pack(10, 20, static_cast<uint8_t>(blue / 2));
        }
    }
    // A red border makes it obvious if the crop takes rows from the wrong half.
    const int x0 = static_cast<int>(kTopX);
    fill_rect(x0, 0, kTopWidth, 3, pack(230, 60, 60));
    fill_rect(x0, static_cast<int>(kTopHeight) - 3, kTopWidth, 3, pack(230, 60, 60));
    fill_rect(x0, 0, 3, kTopHeight, pack(230, 60, 60));
    fill_rect(x0 + static_cast<int>(kTopWidth) - 3, 0, 3, kTopHeight, pack(230, 60, 60));

    const int bar_x = x0 + static_cast<int>((g_frame_counter * 2) % (kTopWidth - 24));
    fill_rect(bar_x, 20, 24, 24, pack(255, 255, 255));

    if (!g_layout_option_honoured) {
        // Loud failure state: the frontend did not pin the layout option.
        fill_rect(x0, 60, kTopWidth, 40, pack(255, 0, 255));
    }

    // Visible proof that save data survived a restart.
    if (g_save_was_loaded) {
        fill_rect(x0, 60, kTopWidth, 72, pack(255, 140, 0));
    }
}

// The bottom screen: warm greens, a checkerboard, and a crosshair at the touch
// point so touch mapping can be checked by eye.
void draw_bottom_screen() {
    const unsigned base = kTopHeight;
    for (unsigned y = 0; y < kBottomHeight; ++y) {
        for (unsigned x = 0; x < kBottomWidth; ++x) {
            const bool check = ((x / 16) + (y / 16)) % 2 == 0;
            g_framebuffer[(base + y) * kFrameWidth + kBottomX + x] =
                check ? pack(30, 90, 40) : pack(15, 55, 25);
        }
    }
    const int x0 = static_cast<int>(kBottomX);
    fill_rect(x0, static_cast<int>(base), kBottomWidth, 3, pack(60, 220, 220));
    fill_rect(x0, static_cast<int>(base + kBottomHeight) - 3, kBottomWidth, 3, pack(60, 220, 220));
    fill_rect(x0, static_cast<int>(base), 3, kBottomHeight, pack(60, 220, 220));
    fill_rect(x0 + static_cast<int>(kBottomWidth) - 3, static_cast<int>(base), 3, kBottomHeight,
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
        fill_rect(static_cast<int>(kTopX) + indicator.x, static_cast<int>(kTopHeight) - 24, 14, 14,
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
    if (kUsesHwRender) {
        // Exactly what Citra asks for, and in the same order: a 3.3 core
        // profile, bottom-left origin, no cached context. A frontend that
        // refuses gets the same "cannot load" a real 3DS core would give.
        g_hw_render.context_type = RETRO_HW_CONTEXT_OPENGL_CORE;
        g_hw_render.version_major = 3;
        g_hw_render.version_minor = 3;
        g_hw_render.context_reset = hw_context_reset;
        g_hw_render.context_destroy = hw_context_destroy;
        g_hw_render.depth = true;
        g_hw_render.stencil = false;
        g_hw_render.bottom_left_origin = true;
        if (g_environment == nullptr ||
            !g_environment(RETRO_ENVIRONMENT_SET_HW_RENDER, &g_hw_render)) {
            return false;
        }
    }

    // Report whether the frontend actually pinned the layout we depend on, in
    // whichever core's language this build speaks.
    if (kIsThreeDs) {
        // Every option the frontend depends on, not just the layout: touch is
        // ignored outright unless citra_touch_touchscreen is turned on, and the
        // split lands on the wrong screen if the prominent one is swapped.
        g_layout_option_honoured =
            read_variable("citra_layout_option") == "Default Top-Bottom Screen" &&
            read_variable("citra_swap_screen") == "Top" &&
            read_variable("citra_touch_touchscreen") == "enabled";
    } else {
        g_layout_option_honoured = (read_variable("melonds_screen_layout1") == "top-bottom") &&
                                   (read_variable("melonds_number_of_screen_layouts") == "1") &&
                                   (read_variable("melonds_screen_gap") == "0");
    }
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
    // Report the circle pad back, so a test can see whether the frontend's
    // analog plumbing reached the core at all. A 3DS is played with this stick;
    // a core that never receives it cannot be walked around in.
    if (g_input_state != nullptr && g_environment != nullptr) {
        const int16_t circle_x = g_input_state(0, RETRO_DEVICE_ANALOG,
                                               RETRO_DEVICE_INDEX_ANALOG_LEFT,
                                               RETRO_DEVICE_ID_ANALOG_X);
        const int16_t circle_y = g_input_state(0, RETRO_DEVICE_ANALOG,
                                               RETRO_DEVICE_INDEX_ANALOG_LEFT,
                                               RETRO_DEVICE_ID_ANALOG_Y);
        if (circle_x != g_reported_circle_x || circle_y != g_reported_circle_y) {
            g_reported_circle_x = circle_x;
            g_reported_circle_y = circle_y;
            static char text[64];
            std::snprintf(text, sizeof(text), "circle %d,%d", circle_x, circle_y);
            struct retro_message message = {text, 60};
            g_environment(RETRO_ENVIRONMENT_SET_MESSAGE, &message);
        }
    }

    draw_top_screen();
    draw_bottom_screen();
    draw_button_indicators();
    ++g_frame_counter;

    if (g_video_refresh != nullptr) {
        if (kUsesHwRender) {
            present_through_gpu();
            g_video_refresh(RETRO_HW_FRAME_BUFFER_VALID, kFrameWidth, kFrameHeight, 0);
        } else {
            g_video_refresh(g_framebuffer, kFrameWidth, kFrameHeight,
                            kFrameWidth * sizeof(uint32_t));
        }
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
