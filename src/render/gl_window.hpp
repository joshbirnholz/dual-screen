#pragma once

#include <SDL.h>

#include <string>
#include <vector>

#include "core/system.hpp"

namespace ds {

// A window with a GL context on it, and the entry points loaded.
//
// Getting one is fiddly enough to be worth having in a single place: which
// context a driver will actually hand over varies, what it hands over is not
// always what was asked for, and the shaders differ between desktop GL and ES.
// Both binaries that draw -- the app and the Switch companion -- want the same
// answer to all of that, and a second copy of this logic is how the two would
// quietly end up rendering differently.
struct GlWindow {
    SDL_Window* window = nullptr;
    SDL_GLContext context = nullptr;

    bool valid() const { return window != nullptr && context != nullptr; }
    void destroy();
};

// Which kinds of GL a platform actually has.
//
// A value rather than a set of conditionals, for the same reason
// PathConvention is one (see config/paths.hpp): the answer for a platform this
// is not being compiled for can then be asked for, and checked, from one that
// it is. Getting this wrong does not misbehave subtly -- it means no context
// at all and an app that will not start -- and it is exactly the sort of thing
// that cannot be noticed from the machine it is written on.
enum class GlFamily {
    // Both are on offer and ES is preferred, because ES 2.0 is the common
    // subset across every platform here and using it everywhere it exists is
    // what keeps the renderer honest. Linux, and the Switch and Android
    // builds, whose drivers hand out an ES context and would refuse a desktop
    // one anyway.
    EitherKind,
    // Desktop GL and no ES whatsoever: macOS, which offers a legacy profile
    // frozen at 2.1 and a core profile that goes to 4.1, and nothing else.
    DesktopOnly,
    // ES and no desktop GL whatsoever: iOS and tvOS. The mirror image of
    // macOS, on hardware from the same company -- which is why "Apple" is not
    // the question this enum asks.
    EmbeddedOnly,
};

// This build's own.
GlFamily native_gl_family();

// One context to ask SDL for. `profile` of zero asks for nothing at all and
// takes the driver's default.
struct GlContextAttempt {
    int profile = 0;
    int major = 0;
    int minor = 0;
    bool gles = false;
    const char* description = "the driver's default context";
};

// The contexts to try, in the order to try them, for a platform and a need.
//
// Always ends with the driver's default: under gamescope an explicitly
// profiled request can be refused where the default succeeds, and a working
// context of some kind always beats refusing to start.
std::vector<GlContextAttempt> gl_context_attempts(GlFamily family, GlContextNeed need);

struct GlWindowRequest {
    const char* title = "Dualscreen";
    int width = 1280;
    int height = 800;
    bool windowed = false;
    // A GPU-rendering core will not load without the context it asked for, so
    // asking for one narrows what is acceptable. A companion runs no core and
    // always takes whatever is on offer.
    GlContextNeed need = GlContextNeed::Any;
    bool vsync = true;
};

bool create_gl_window(const GlWindowRequest& request, GlWindow& out, std::string* error);

}  // namespace ds
