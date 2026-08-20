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

static unsigned g_fb = 0;
static uintptr_t RETRO_CALLCONV get_fb() { return g_fb; }
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
            // Nothing forced: the core falls back to its own defaults, which is
            // the plain "someone just ran this in RetroArch" case.
            auto* v = static_cast<retro_variable*>(data);
            v->value = nullptr;
            return false;
        }
        case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
        case RETRO_ENVIRONMENT_SET_GEOMETRY: {
            auto* g = static_cast<retro_game_geometry*>(data);
            if (cmd == RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO) {
                g = &static_cast<retro_system_av_info*>(data)->geometry;
            }
            std::printf("geometry now %ux%u (max %ux%u)\n", g->base_width, g->base_height,
                        g->max_width, g->max_height);
            return true;
        }
        case RETRO_ENVIRONMENT_GET_CAN_DUPE:
            *static_cast<bool*>(data) = true;
            return true;
        default:
            return false;
    }
}
// --- a minimal hardware-render frontend -------------------------------------
// Enough of one to answer a single question: does this core, with this game,
// draw anything into a framebuffer handed to it the way libretro says? If it
// does here and not in the app, the difference is the app's. If it does not,
// the app is not the reason.
static unsigned g_tex = 0;
static unsigned g_fb_w = 0, g_fb_h = 0;
static bool g_frame_seen = false;
static unsigned g_frame_w = 0, g_frame_h = 0;

typedef void (*PFNGENFB)(int, unsigned*);
typedef void (*PFNBINDFB)(unsigned, unsigned);
typedef void (*PFNFBTEX)(unsigned, unsigned, unsigned, unsigned, int);
typedef void (*PFNGENTEX)(int, unsigned*);
typedef void (*PFNBINDTEX)(unsigned, unsigned);
typedef void (*PFNTEXIMG)(unsigned, int, int, int, int, int, unsigned, unsigned, const void*);
typedef void (*PFNTEXPARAM)(unsigned, unsigned, int);
typedef unsigned (*PFNCHECKFB)(unsigned);
typedef void (*PFNREADPIX)(int, int, int, int, unsigned, unsigned, void*);

static void make_framebuffer(unsigned w, unsigned h) {
    auto GenFramebuffers = (PFNGENFB)SDL_GL_GetProcAddress("glGenFramebuffers");
    auto BindFramebuffer = (PFNBINDFB)SDL_GL_GetProcAddress("glBindFramebuffer");
    auto FramebufferTexture2D = (PFNFBTEX)SDL_GL_GetProcAddress("glFramebufferTexture2D");
    auto GenTextures = (PFNGENTEX)SDL_GL_GetProcAddress("glGenTextures");
    auto BindTexture = (PFNBINDTEX)SDL_GL_GetProcAddress("glBindTexture");
    auto TexImage2D = (PFNTEXIMG)SDL_GL_GetProcAddress("glTexImage2D");
    auto TexParameteri = (PFNTEXPARAM)SDL_GL_GetProcAddress("glTexParameteri");
    auto CheckFramebufferStatus = (PFNCHECKFB)SDL_GL_GetProcAddress("glCheckFramebufferStatus");
    if (!GenFramebuffers) { std::printf("no framebuffer objects\n"); return; }

    GenTextures(1, &g_tex);
    BindTexture(0x0DE1, g_tex);
    TexImage2D(0x0DE1, 0, 0x8058 /*GL_RGBA8*/, (int)w, (int)h, 0, 0x1908, 0x1401, nullptr);
    TexParameteri(0x0DE1, 0x2801, 0x2601);  // MIN_FILTER, LINEAR
    TexParameteri(0x0DE1, 0x2800, 0x2601);  // MAG_FILTER, LINEAR
    BindTexture(0x0DE1, 0);
    GenFramebuffers(1, &g_fb);
    BindFramebuffer(0x8D40, g_fb);
    FramebufferTexture2D(0x8D40, 0x8CE0, 0x0DE1, g_tex, 0);
    const unsigned status = CheckFramebufferStatus(0x8D40);
    BindFramebuffer(0x8D40, 0);
    g_fb_w = w; g_fb_h = h;
    std::printf("framebuffer %ux%u name=%u status=0x%X (0x8CD5 is complete)\n", w, h, g_fb, status);
}

static void RETRO_CALLCONV video_hw(const void* data, unsigned w, unsigned h, size_t) {
    if (data == RETRO_HW_FRAME_BUFFER_VALID) {
        g_frame_seen = true;
        g_frame_w = w;
        g_frame_h = h;
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
    if (!ok || !g_hw_requested) { (void)audio_batch_noop; return ok ? 0 : 1; }

    // From here on this behaves like a frontend: it makes the framebuffer the
    // core will draw into, tells the core its context is ready, then runs
    // frames and looks at what came out.
    auto av = (void(*)(retro_system_av_info*))sym("retro_get_system_av_info");
    retro_system_av_info info2{};
    if (av) {
        av(&info2);
        std::printf("av_info: base %ux%u max %ux%u %.2f fps\n", info2.geometry.base_width,
                    info2.geometry.base_height, info2.geometry.max_width,
                    info2.geometry.max_height, info2.timing.fps);
    }
    const unsigned fw = info2.geometry.base_width ? info2.geometry.base_width : 400;
    const unsigned fh = info2.geometry.base_height ? info2.geometry.base_height : 480;
    make_framebuffer(fw, fh);
    if (set_video) set_video(&video_hw);

    if (g_hw.context_reset) {
        std::printf("--- context_reset ---\n");
        g_hw.context_reset();
    }

    auto run = (void(*)())sym("retro_run");
    if (!run) { std::fprintf(stderr, "no retro_run\n"); return 1; }

    auto BindFramebuffer = (PFNBINDFB)SDL_GL_GetProcAddress("glBindFramebuffer");
    auto ReadPixels = (PFNREADPIX)SDL_GL_GetProcAddress("glReadPixels");

    const int frames = (argc > 3) ? std::atoi(argv[3]) : 300;
    int last_bright = -1;
    for (int i = 0; i < frames; ++i) {
        BindFramebuffer(0x8D40, g_fb);
        run();
        BindFramebuffer(0x8D40, 0);

        // Every so often, look at what is in the framebuffer.
        if (i % 60 == 59 || i == frames - 1) {
            BindFramebuffer(0x8CA8 /*READ*/, g_fb);
            int brightest = 0;
            for (unsigned y = 0; y < g_fb_h; y += 16) {
                for (unsigned x = 0; x < g_fb_w; x += 16) {
                    unsigned char px[4] = {0, 0, 0, 0};
                    ReadPixels((int)x, (int)y, 1, 1, 0x1908, 0x1401, px);
                    const int sum = px[0] + px[1] + px[2];
                    if (sum > brightest) brightest = sum;
                }
            }
            BindFramebuffer(0x8CA8, 0);
            if (brightest != last_bright) {
                std::printf("frame %d: presented=%s %ux%u, brightest pixel in the core's "
                            "framebuffer = %d\n", i + 1, g_frame_seen ? "yes" : "no",
                            g_frame_w, g_frame_h, brightest);
                last_bright = brightest;
            }
        }
    }
    std::printf("--- done: the core %s draw anything ---\n",
                last_bright > 0 ? "DID" : "did NOT");
    (void)audio_batch_noop;
    return 0;
}
