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
                         float x,
                         float y,
                         float scale,
                         const Color& color,
                         TextAlign align,
                         std::vector<GuideHit>* out_hits) {
    const float height = glyph_height(scale);
    const float glyph_width = glyphs.width_for(height);
    const float gap = TextRenderer::line_height(scale) * kGapScale;
    const float spacing = TextRenderer::line_height(scale) * kSpacingScale;

    float total = 0.0f;
    for (std::size_t index = 0; index < prompts.size(); ++index) {
        if (index > 0) total += spacing;
        total += glyph_width + gap + TextRenderer::measure_width(prompts[index].action, scale);
    }

    if (out_hits != nullptr) out_hits->clear();
    float pen_x = std::round(align == TextAlign::Right ? x - total : x - total * 0.5f);
    // Centred on the button itself rather than on the box it is drawn into,
    // which includes headroom for whatever stands above a glyph.
    const float label_y =
        y + height * glyphs.button_centre() - TextRenderer::line_height(scale) * 0.5f;
    for (std::size_t index = 0; index < prompts.size(); ++index) {
        if (index > 0) pen_x += spacing;
        glyphs.draw(quads, prompts[index].glyph, pen_x, y, height, color);
        pen_x += glyph_width + gap;
        const float label_width = TextRenderer::measure_width(prompts[index].action, scale);
        text.draw(quads, prompts[index].action, pen_x, label_y, scale, color);
        if (out_hits != nullptr) {
            // The glyph and its words together: a finger aims at the prompt,
            // not at the picture inside it.
            const float left = pen_x - glyph_width - gap;
            out_hits->push_back(
                GuideHit{prompts[index].glyph,
                         PixelRect{left, y, glyph_width + gap + label_width, height}});
        }
        pen_x += label_width;
    }
    return height;
}

}  // namespace ds
