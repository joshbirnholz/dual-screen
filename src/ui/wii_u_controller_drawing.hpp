#pragma once

#include "input/wii_u_mapping.hpp"
#include "render/quad_renderer.hpp"

namespace ds {

// A Wii U game's controller as Button Mapping draws it beside the list: the
// GamePad, a Pro Controller, a Wii Remote held upright -- with its Nunchuk
// beside it, or below a Classic Controller -- in outline, `picked` (one of the
// console's buttons) filled in the accent colour. Fitted into `box`.
void draw_wii_u_controller(QuadRenderer& quads, const PixelRect& box, WiiUController controller,
                           WiiUAction picked);

}  // namespace ds
