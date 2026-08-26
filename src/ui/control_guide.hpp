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

// Where one prompt ended up, so a finger can land on it. Filled in while the
// row is drawn and hit-tested on the next press: the row is laid out from the
// glyph artwork's own proportions, which only the drawing side knows.
struct GuideHit {
    ButtonGlyph glyph = ButtonGlyph::A;
    PixelRect rect;
};

// Draws the prompts as one row and returns how tall it was. `x` is the row's
// centre when centred and its right edge when right-aligned. `out_hits`, when
// given, receives where each prompt landed.
float draw_control_guide(QuadRenderer& quads,
                         TextRenderer& text,
                         const ButtonGlyphSheet& glyphs,
                         const std::vector<GuidePrompt>& prompts,
                         float x,
                         float y,
                         float scale,
                         const Color& color,
                         TextAlign align = TextAlign::Center,
                         std::vector<GuideHit>* out_hits = nullptr);

// How tall that row will be, for a caller reserving space before drawing.
float control_guide_height(float scale);

}  // namespace ds
