// Loads a real libretro core against a real OpenGL context and reports what it
// asks the frontend for.
//
// No ROM is needed, which is the point: a core requests its hardware context,
// its pixel format and its options before it looks at the game file, so the
// whole handshake can be checked against the genuine core without owning a
// game for it. That is the difference between "the mock agrees with me" and
// "the core agrees with me" -- and this project has been caught out by that
// distinction more than once.
//
//   probe_core /path/to/core_libretro.so [rom]
//
// Needs a display; under CI, run it through xvfb-run.
#include <SDL.h>
#include <dlfcn.h>
#include <cstdio>
#include <cstring>
#include <string>
#include "libretro/libretro.h"

static retro_hw_render_callback g_hw{};
static bool g_hw_requested = false;

static uintptr_t RETRO_CALLCONV get_fb() { return 1; }
static retro_proc_address_t RETRO_CALLCONV get_proc(const char* s) {
    return reinterpret_cast<retro_proc_address_t>(SDL_GL_GetProcAddress(s));
}
static void RETRO_CALLCONV core_log(enum retro_log_level, const char* fmt, ...) { (void)fmt; }

static bool RETRO_CALLCONV env(unsigned cmd, void* data) {
    switch (cmd) {
        case RETRO_ENVIRONMENT_SET_HW_RENDER: {
            auto* cb = static_cast<retro_hw_render_callback*>(data);
            g_hw = *cb;
            g_hw_requested = true;
            std::printf("SET_HW_RENDER: context_type=%u version=%u.%u depth=%d stencil=%d "
                        "bottom_left_origin=%d cache_context=%d\n",
                        (unsigned)cb->context_type, cb->version_major, cb->version_minor,
                        (int)cb->depth, (int)cb->stencil, (int)cb->bottom_left_origin,
                        (int)cb->cache_context);
            cb->get_current_framebuffer = &get_fb;
            cb->get_proc_address = &get_proc;
            return true;
        }
        case RETRO_ENVIRONMENT_SET_HW_SHARED_CONTEXT:
            std::printf("SET_HW_SHARED_CONTEXT requested\n");
            return true;
        case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
            std::printf("SET_PIXEL_FORMAT: %d\n", *static_cast<int*>(data));
            return true;
        case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
            static_cast<retro_log_callback*>(data)->log = &core_log;
            return true;
        case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
        case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
            *static_cast<const char**>(data) = "/tmp";
            return true;
        case RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER:
            *static_cast<unsigned*>(data) = RETRO_HW_CONTEXT_OPENGL_CORE;
            return true;
        case RETRO_ENVIRONMENT_GET_VARIABLE: {
            auto* v = static_cast<retro_variable*>(data);
            v->value = nullptr;
            return false;
        }
        default:
            return false;
    }
}
static void RETRO_CALLCONV video(const void*, unsigned, unsigned, size_t) {}
static void RETRO_CALLCONV audio_batch_noop(const int16_t*, size_t) {}
static size_t RETRO_CALLCONV audio_batch(const int16_t*, size_t f) { return f; }
static void RETRO_CALLCONV input_poll() {}
static int16_t RETRO_CALLCONV input_state(unsigned, unsigned, unsigned, unsigned) { return 0; }

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: probe_core <core.so> [rom]\n"); return 2; }

    SDL_Init(SDL_INIT_VIDEO);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_Window* w = SDL_CreateWindow("probe", 0, 0, 64, 64, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!w) { std::fprintf(stderr, "no window: %s\n", SDL_GetError()); return 1; }
    SDL_GLContext ctx = SDL_GL_CreateContext(w);
    if (!ctx) { std::fprintf(stderr, "no context: %s\n", SDL_GetError()); return 1; }
    SDL_GL_MakeCurrent(w, ctx);

    void* h = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!h) { std::fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }

    auto sym = [&](const char* n) { return dlsym(h, n); };
    auto set_env = (void(*)(retro_environment_t))sym("retro_set_environment");
    auto init = (void(*)())sym("retro_init");
    auto sysinfo = (void(*)(retro_system_info*))sym("retro_get_system_info");
    auto load = (bool(*)(const retro_game_info*))sym("retro_load_game");
    auto set_video = (void(*)(retro_video_refresh_t))sym("retro_set_video_refresh");
    auto set_ab = (void(*)(retro_audio_sample_batch_t))sym("retro_set_audio_sample_batch");
    auto set_ip = (void(*)(retro_input_poll_t))sym("retro_set_input_poll");
    auto set_is = (void(*)(retro_input_state_t))sym("retro_set_input_state");
    if (!set_env || !init || !load) { std::fprintf(stderr, "missing entry points\n"); return 1; }

    retro_system_info info{};
    if (sysinfo) {
        sysinfo(&info);
        std::printf("core: %s %s  extensions: %s\n", info.library_name ? info.library_name : "?",
                    info.library_version ? info.library_version : "?",
                    info.valid_extensions ? info.valid_extensions : "?");
    }

    set_env(&env);
    init();
    if (set_video) set_video(&video);
    if (set_ab) set_ab(&audio_batch);
    if (set_ip) set_ip(&input_poll);
    if (set_is) set_is(&input_state);

    retro_game_info game{};
    game.path = (argc > 2) ? argv[2] : "/tmp/definitely-not-a-real-rom.3ds";
    std::printf("--- calling retro_load_game(\"%s\") ---\n", game.path);
    const bool ok = load(&game);
    std::printf("--- retro_load_game returned %s ---\n", ok ? "true" : "false");
    std::printf("hardware context was %srequested\n", g_hw_requested ? "" : "NOT ");
    (void)audio_batch_noop;
    return 0;
}
