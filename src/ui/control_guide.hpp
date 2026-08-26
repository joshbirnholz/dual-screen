#pragma once

#include <string>
#include <vector>

#include "render/button_glyphs.hpp"
#include "render/quad_renderer.hpp"
#include "render/text_renderer.hpp"

namespace ds {

// One "<button> <action>" pair in the row of prompts along the bottom of a
// screen. The button is drawn as the console's own symbol rather than spelled
// out, so a prompt says the same thing whatever letters the pad in the player's
// hands happens to have printed on it.
struct GuidePrompt {
    ButtonGlyph glyph = ButtonGlyph::A;
    std::string action;
};

// Draws the prompts as one row and returns how tall it was. `x` is the row's
// centre when centred and its right edge when right-aligned.
float draw_control_guide(QuadRenderer& quads,
                         TextRenderer& text,
                         const ButtonGlyphSheet& glyphs,
                         const std::vector<GuidePrompt>& prompts,
                         float x,
                         float y,
                         float scale,
                         const Color& color,
                         TextAlign align = TextAlign::Center);

// How tall that row will be, for a caller reserving space before drawing.
float control_guide_height(float scale);

}  // namespace ds
