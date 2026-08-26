#include "ui/control_guide.hpp"

#include <cmath>

namespace ds {
namespace {

// The glyph is drawn taller than the text beside it: the artwork is a disc with
// a letter inside, so matching their heights would leave the letter much
// smaller than the words it sits next to.
constexpr float kGlyphScale = 1.9f;
constexpr float kGapScale = 0.4f;
constexpr float kSpacingScale = 1.4f;

float glyph_height(float scale) { return TextRenderer::line_height(scale) * kGlyphScale; }

}  // namespace

float control_guide_height(float scale) { return glyph_height(scale); }

float draw_control_guide(QuadRenderer& quads,
                         TextRenderer& text,
                         const ButtonGlyphSheet& glyphs,
                         const std::vector<GuidePrompt>& prompts,
                         float centre_x,
                         float y,
                         float scale,
                         const Color& color) {
    const float height = glyph_height(scale);
    const float glyph_width = glyphs.width_for(height);
    const float gap = TextRenderer::line_height(scale) * kGapScale;
    const float spacing = TextRenderer::line_height(scale) * kSpacingScale;

    float total = 0.0f;
    for (std::size_t index = 0; index < prompts.size(); ++index) {
        if (index > 0) total += spacing;
        total += glyph_width + gap + TextRenderer::measure_width(prompts[index].action, scale);
    }

    float pen_x = std::round(centre_x - total * 0.5f);
    for (std::size_t index = 0; index < prompts.size(); ++index) {
        if (index > 0) pen_x += spacing;
        glyphs.draw(quads, prompts[index].glyph, pen_x, y, height);
        pen_x += glyph_width + gap;
        // Centred against the glyph rather than sharing its top edge, which the
        // artwork's own headroom would otherwise throw off.
        text.draw(quads, prompts[index].action, pen_x,
                  y + (height - TextRenderer::line_height(scale)) * 0.5f, scale, color);
        pen_x += TextRenderer::measure_width(prompts[index].action, scale);
    }
    return height;
}

}  // namespace ds
