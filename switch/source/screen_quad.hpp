#pragma once

#include "layout/layout.hpp"

namespace ds::sw {

// Where one screen goes on an SDL renderer.
//
// SDL turns a texture about the centre of the destination rect it is handed,
// so a quarter-turned screen cannot be drawn into the box the layout reports:
// that box is the screen's shape already turned. The picture goes into that
// box turned back the other way -- the same centre, width and height swapped
// -- and is then spun into place, which lands it exactly where the layout
// says. Getting this wrong is not subtle on a Side-by-Side layout, but it is
// invisible on every other one, which is why it is its own function with its
// own test rather than four lines inside the draw loop.
struct ScreenQuad {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;
    // Clockwise degrees, which is the direction SDL_RenderCopyEx measures in.
    double angle_degrees = 0.0;
};

ScreenQuad screen_quad_for(const PlacedScreen& placed);

}  // namespace ds::sw
