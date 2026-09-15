#include "render/gl_window.hpp"

#include <cstdlib>
#include <iterator>
#include <string>

#include "render/gl_loader.hpp"
#include "util/log.hpp"

namespace ds {

void GlWindow::destroy() {
    if (context != nullptr) {
        SDL_GL_DeleteContext(context);
        context = nullptr;
    }
    if (window != nullptr) {
        SDL_DestroyWindow(window);
        window = nullptr;
    }
}

bool create_gl_window(const GlWindowRequest& request, GlWindow& out, std::string* error) {  // NOLINT
    GlContextNeed need = request.need;
    SDL_Window*& window = out.window;
    SDL_GLContext& gl_context = out.context;
    struct ContextAttempt {
        int profile;
        int major;
        int minor;
        bool gles;
        const char* description;
    };
    // Tried in order, widening as it goes. The last entry asks for nothing at
    // all and takes whatever the driver's default context is: under gamescope
    // an explicitly-profiled request can be refused where the default succeeds,
    // and a working context of some kind always beats refusing to start.
    //
    // OpenGL ES 2.0 comes first by default: it is the common subset across
    // every platform on the roadmap, so the renderer is exercised against the
    // same feature set everywhere. Desktop GL 2.1 is the fallback for drivers
    // that will not hand out an ES context.
#if defined(__APPLE__)
    // Except on Apple's, which have no ES at all -- asking for one is two
    // guaranteed failures before the real attempt, and the log line that
    // reports which context was created would name the fallback as though
    // something had gone wrong.
    //
    // Core 3.3 rather than 2.1 compatibility, which is the other way round
    // from everywhere else. macOS offers exactly two things: a frozen legacy
    // profile that stops at 2.1, and a core profile that goes to 4.1. Where
    // the legacy one is a driver's grudging fallback elsewhere, here it is a
    // museum piece, and the renderer already speaks core -- see
    // shader_preamble(), which has had a core path since a Citra core needed
    // one.
    const ContextAttempt any_attempts[] = {
        {SDL_GL_CONTEXT_PROFILE_CORE, 3, 3, false, "OpenGL 3.3 core"},
        {SDL_GL_CONTEXT_PROFILE_COMPATIBILITY, 2, 1, false, "OpenGL 2.1 compatibility"},
        {0, 0, 0, false, "the driver's default context"},
    };
#else
    const ContextAttempt any_attempts[] = {
        {SDL_GL_CONTEXT_PROFILE_ES, 2, 0, true, "OpenGL ES 2.0"},
        {SDL_GL_CONTEXT_PROFILE_ES, 3, 0, true, "OpenGL ES 3.0"},
        {SDL_GL_CONTEXT_PROFILE_COMPATIBILITY, 2, 1, false, "OpenGL 2.1 compatibility"},
        {SDL_GL_CONTEXT_PROFILE_CORE, 3, 3, false, "OpenGL 3.3 core"},
        {0, 0, 0, false, "the driver's default context"},
    };
#endif
    // A GPU-rendering core will not load without the context it asked for, so
    // when one is going to be loaded the order is reversed and the ES entries
    // drop out entirely -- an ES 2.0 context here would only mean starting up
    // and then failing to open the game.
    const ContextAttempt gl33_attempts[] = {
        {SDL_GL_CONTEXT_PROFILE_CORE, 3, 3, false, "OpenGL 3.3 core"},
        {SDL_GL_CONTEXT_PROFILE_COMPATIBILITY, 3, 3, false, "OpenGL 3.3 compatibility"},
        {0, 0, 0, false, "the driver's default context"},
    };

    // A way to exercise the 3.3 path on a machine whose driver would otherwise
    // hand over an ES context. switch_client_gl33_headless sets it and runs a
    // whole session through it, which is the only place the core-profile
    // shaders are actually drawn with on Linux -- and so the only advance
    // warning of a break on Apple's platforms, where a core context is not a
    // fallback but the only thing there is.
    const char* forced = std::getenv("DUALSCREEN_FORCE_GL33");
    if (forced != nullptr && forced[0] != '\0' && forced[0] != '0') {
        need = GlContextNeed::OpenGl33Core;
    }

    const ContextAttempt* attempts =
        (need == GlContextNeed::OpenGl33Core) ? gl33_attempts : any_attempts;
    const std::size_t attempt_count = (need == GlContextNeed::OpenGl33Core)
                                          ? std::size(gl33_attempts)
                                          : std::size(any_attempts);

    Uint32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
    if (!request.windowed) {
        // Borderless-desktop fullscreen is what gamescope expects from a
        // well-behaved client; it never asks to change the display mode.
        flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    }

    std::string last_error;
    for (std::size_t index = 0; index < attempt_count; ++index) {
        const ContextAttempt& attempt = attempts[index];
        SDL_GL_ResetAttributes();
        if (attempt.profile != 0) {
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, attempt.profile);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, attempt.major);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, attempt.minor);
        }
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
        SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
        SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);

        window = SDL_CreateWindow(request.title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                  request.width, request.height, flags);
        if (window == nullptr) {
            last_error = SDL_GetError();
            log_warn(std::string("no window with ") + attempt.description + ": " + last_error);
            continue;
        }
        gl_context = SDL_GL_CreateContext(window);
        if (gl_context == nullptr) {
            last_error = SDL_GetError();
            log_warn(std::string("no ") + attempt.description + " context: " + last_error);
            SDL_DestroyWindow(window);
            window = nullptr;
            continue;
        }

        SDL_GL_MakeCurrent(window, gl_context);
        std::string load_error;
        // Whether this is really an ES context is asked of the context itself
        // rather than assumed from what was requested, because the default
        // attempt does not request anything and the shaders differ.
        int profile = 0;
        SDL_GL_GetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, &profile);
        const bool is_gles = attempt.profile != 0
                                 ? attempt.gles
                                 : (profile == SDL_GL_CONTEXT_PROFILE_ES);
        if (!gl::load(is_gles, &load_error)) {
            last_error = load_error;
            log_warn(std::string("cannot use the ") + attempt.description + ": " + load_error);
            SDL_GL_DeleteContext(gl_context);
            gl_context = nullptr;
            SDL_DestroyWindow(window);
            window = nullptr;
            continue;
        }

        // What was asked for and what arrived are not the same thing, least of
        // all for the unprofiled attempt. A core that needs 3.3 has to be told
        // now rather than after it has failed to load a game.
        if (need == GlContextNeed::OpenGl33Core && !gl::at_least(3, 3)) {
            int have_major = 0;
            int have_minor = 0;
            gl::version(have_major, have_minor);
            last_error = "this driver offers OpenGL " + std::to_string(have_major) + "." +
                         std::to_string(have_minor) + ", and 3DS emulation needs 3.3";
            log_warn(last_error);
            gl::unload();
            SDL_GL_DeleteContext(gl_context);
            gl_context = nullptr;
            SDL_DestroyWindow(window);
            window = nullptr;
            continue;
        }

        log_info(std::string("created a ") + attempt.description + " context");
        SDL_GL_SetSwapInterval(request.vsync ? 1 : 0);
        // No pointer on screen to begin with: an arrow parked over the game is
        // exactly the kind of thing that gives away that this is not a console.
        // It comes back while the mouse is being moved -- see CursorVisibility.
        SDL_ShowCursor(SDL_DISABLE);
        return true;
    }

    if (error != nullptr) {
        *error = "could not create an OpenGL window: " + last_error;
    }
    return false;
}


}  // namespace ds
