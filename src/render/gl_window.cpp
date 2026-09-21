#include "render/gl_window.hpp"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE
#define DS_UIKIT_SCENES 1
// Defined in apple/ios_scene_lifecycle.m. Objective-C, and one function, so
// there is no header worth making for it.
extern "C" void ds_attach_sdl_window_to_scene(SDL_Window* window);
#endif
#endif

#include <cstdlib>
#include <string>
#include <vector>

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

GlFamily native_gl_family() {
#if defined(__APPLE__)
    // Apple is two answers, not one, and the difference is total: macOS has
    // desktop GL and no ES, while iOS and tvOS have ES and no desktop GL.
    // TargetConditionals.h is the only thing that tells them apart, and
    // TARGET_OS_MAC is true on all three -- TARGET_OS_OSX is the one that
    // means a Mac.
#if TARGET_OS_IPHONE
    return GlFamily::EmbeddedOnly;
#else
    return GlFamily::DesktopOnly;
#endif
#else
    return GlFamily::EitherKind;
#endif
}

std::vector<GlContextAttempt> gl_context_attempts(GlFamily family, GlContextNeed need) {
    // A GPU-rendering core will not load without the context it asked for, so
    // when one is going to be loaded the ES entries drop out entirely -- an
    // ES 2.0 context here would only mean starting up and then failing to open
    // the game.
    //
    // No such core runs on a platform with only ES, and none is built for one:
    // this is the host's need, and the host is a desktop program. Answered
    // anyway rather than left to fall through, because "there is no context
    // that can do this" is a better thing for a caller to be told by an empty
    // list than by a context that cannot.
    if (need == GlContextNeed::OpenGl33Core) {
        if (family == GlFamily::EmbeddedOnly) {
            return {};
        }
        return {
            {SDL_GL_CONTEXT_PROFILE_CORE, 3, 3, false, "OpenGL 3.3 core"},
            {SDL_GL_CONTEXT_PROFILE_COMPATIBILITY, 3, 3, false, "OpenGL 3.3 compatibility"},
            {},
        };
    }
    switch (family) {
        case GlFamily::DesktopOnly:
            // Core 3.3 before 2.1 compatibility, which is the other way round
            // from everywhere else. Where the legacy profile is a driver's
            // grudging fallback elsewhere, on macOS it is a museum piece
            // frozen at 2.1, and the renderer already speaks core -- see
            // shader_preamble(), which has had a core path since a Citra core
            // needed one.
            return {
                {SDL_GL_CONTEXT_PROFILE_CORE, 3, 3, false, "OpenGL 3.3 core"},
                {SDL_GL_CONTEXT_PROFILE_COMPATIBILITY, 2, 1, false, "OpenGL 2.1 compatibility"},
                {},
            };
        case GlFamily::EmbeddedOnly:
            // ES 3.0 is offered above 2.0 and both are kept: every iOS and
            // tvOS device that runs this has ES 3.0, but asking for 2.0 as
            // well costs one refused attempt in the worst case and is the
            // feature set the renderer is actually written against.
            //
            // Nothing desktop is offered at all. On these platforms it is not
            // an unlikely fallback, it is a guaranteed failure, and a list
            // that ends in two of those is a list whose log line names a
            // fallback as though something had gone wrong.
            return {
                {SDL_GL_CONTEXT_PROFILE_ES, 3, 0, true, "OpenGL ES 3.0"},
                {SDL_GL_CONTEXT_PROFILE_ES, 2, 0, true, "OpenGL ES 2.0"},
                {},
            };
        case GlFamily::EitherKind:
            break;
    }
    // OpenGL ES 2.0 first: it is the common subset across every platform on
    // the roadmap, so the renderer is exercised against the same feature set
    // everywhere. Desktop GL is the fallback for drivers that will not hand
    // out an ES context.
    return {
        {SDL_GL_CONTEXT_PROFILE_ES, 2, 0, true, "OpenGL ES 2.0"},
        {SDL_GL_CONTEXT_PROFILE_ES, 3, 0, true, "OpenGL ES 3.0"},
        {SDL_GL_CONTEXT_PROFILE_COMPATIBILITY, 2, 1, false, "OpenGL 2.1 compatibility"},
        {SDL_GL_CONTEXT_PROFILE_CORE, 3, 3, false, "OpenGL 3.3 core"},
        {},
    };
}

bool create_gl_window(const GlWindowRequest& request, GlWindow& out, std::string* error) {  // NOLINT
    GlContextNeed need = request.need;
    SDL_Window*& window = out.window;
    SDL_GLContext& gl_context = out.context;

    // A way to exercise the 3.3 path on a machine whose driver would otherwise
    // hand over an ES context. switch_client_gl33_headless sets it and runs a
    // whole session through it, which is the only place the core-profile
    // shaders are actually drawn with on Linux -- and so the only advance
    // warning of a break on macOS, where a core context is not a fallback but
    // the only thing there is.
    const char* forced = std::getenv("DUALSCREEN_FORCE_GL33");
    if (forced != nullptr && forced[0] != '\0' && forced[0] != '0') {
        need = GlContextNeed::OpenGl33Core;
    }

    const std::vector<GlContextAttempt> attempts = gl_context_attempts(native_gl_family(), need);
    const std::size_t attempt_count = attempts.size();
    if (attempts.empty()) {
        if (error != nullptr) {
            *error = "this platform has no OpenGL context that can run a GPU-rendering core";
        }
        return false;
    }

    Uint32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
    if (!request.windowed) {
        // Borderless-desktop fullscreen is what gamescope expects from a
        // well-behaved client; it never asks to change the display mode.
        flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    }
    // A phone, a tablet and a television have no windows to be one of. SDL
    // gives an iOS or tvOS window the screen either way, but asking for
    // fullscreen as well is what makes it report the screen's real size --
    // and a request for a 1280x720 *window* on a device that has none is how
    // the picture ends up drawn at a size no display here has.
    if (native_gl_family() == GlFamily::EmbeddedOnly) {
        flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    }

    std::string last_error;
    for (std::size_t index = 0; index < attempt_count; ++index) {
        const GlContextAttempt& attempt = attempts[index];
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
#if defined(DS_UIKIT_SCENES)
        // A UIWindow that belongs to no scene is a window on no display, and
        // SDL makes its windows the pre-scene way. See
        // apple/ios_scene_lifecycle.m, which is where the rest of this lives.
        ds_attach_sdl_window_to_scene(window);
#endif
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
