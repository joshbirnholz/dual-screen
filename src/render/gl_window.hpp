#pragma once

#include <SDL.h>

#include <string>

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
