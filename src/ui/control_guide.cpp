#include "ui/control_guide.hpp"

#include <algorithm>
#include <cmath>

#include "ui/ui_theme.hpp"

namespace ds {
namespace {

// The glyph is drawn taller than the text beside it: the artwork is a disc with
// a letter inside, so matching their heights would leave the letter much
// smaller than the words it sits next to.
constexpr float kGlyphScale = 1.9f;
constexpr float kGapScale = 0.4f;
constexpr float kSpacingScale = 1.4f;

float glyph_height(float scale) { return TextRenderer::line_height(scale) * kGlyphScale; }

// A touch chip's padding either side of its label, and how tall it is. The
// height is deliberately not the whole row: the row is tall enough for the
// arrow that stands above a stick-click glyph, and a chip drawn that tall
// would look like a different component rather than the same prompt.
// Wider than a square box wanted. A capsule's rounded ends take room from
// the label without looking like they have, so the same padding reads tighter
// than it did with corners.
constexpr float kChipPadScale = 0.85f;
constexpr float kChipHeightScale = 1.7f;

float chip_height(float scale) { return TextRenderer::line_height(scale) * kChipHeightScale; }
float chip_pad(float scale) { return TextRenderer::line_height(scale) * kChipPadScale; }

bool g_touch_hints = false;

}  // namespace

void set_touch_hints(bool touch) { g_touch_hints = touch; }
bool touch_hints() { return g_touch_hints; }

ButtonGlyph guide_button_at(const std::vector<GuideHit>& hits, float x, float y) {
    for (const GuideHit& hit : hits) {
        if (hit.rect.contains(x, y)) {
            return hit.glyph;
        }
    }
    return ButtonGlyph::Count;
}

bool ui_action_for_glyph(ButtonGlyph glyph, UiAction& out) {
    switch (glyph) {
        case ButtonGlyph::A: out = UiAction::Confirm; return true;
        case ButtonGlyph::B: out = UiAction::Cancel; return true;
        case ButtonGlyph::Plus: out = UiAction::Quit; return true;
        case ButtonGlyph::Minus: out = UiAction::ToggleMenu; return true;
        // X, Y and the stick clicks only ever appear in the quick menu, where
        // a press is forwarded to the host rather than acted on here. See
        // Client::handle_menu().
        default: return false;
    }
}

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
    const bool touch = touch_hints();
    const float pad = chip_pad(scale);

    // One prompt's width: a glyph and its words, or a chip around the words.
    const auto prompt_width = [&](const GuidePrompt& prompt) {
        const float label = TextRenderer::measure_width(prompt.action, scale);
        return touch ? label + pad * 2.0f : glyph_width + gap + label;
    };

    float total = 0.0f;
    for (std::size_t index = 0; index < prompts.size(); ++index) {
        if (index > 0) total += spacing;
        total += prompt_width(prompts[index]);
    }

    if (out_hits != nullptr) out_hits->clear();
    float pen_x = std::round(align == TextAlign::Right ? x - total : x - total * 0.5f);
    // Centred on the button itself rather than on the box it is drawn into,
    // which includes headroom for whatever stands above a glyph.
    const float label_y =
        y + height * glyphs.button_centre() - TextRenderer::line_height(scale) * 0.5f;
    for (std::size_t index = 0; index < prompts.size(); ++index) {
        if (index > 0) pen_x += spacing;
        const float width = prompt_width(prompts[index]);
        if (touch) {
            // A chip: the words in a box that looks like it can be pressed,
            // because here it is the only thing that can be. Centred on the
            // same line the glyph's button sits on, so a row of these lands
            // exactly where a row of glyphs would have.
            const float box_height = chip_height(scale);
            const PixelRect box{pen_x, label_y + TextRenderer::line_height(scale) * 0.5f -
                                           box_height * 0.5f,
                                width, box_height};
            // The line and the fill are two capsules, the second drawn inside
            // the first -- there is no stroked capsule, and an outline built
            // from four straight quads would cut the corners off the thing it
            // is meant to be following.
            const float line = std::max(1.0f, std::round(menu_border(scale) * 0.5f));
            quads.draw_capsule(box, color);
            quads.draw_capsule(PixelRect{box.x + line, box.y + line, box.w - line * 2.0f,
                                         box.h - line * 2.0f},
                               ui_theme().panel);
            text.draw(quads, prompts[index].action, pen_x + width * 0.5f, label_y, scale, color,
                      TextAlign::Center);
            if (out_hits != nullptr) {
                // The chip itself, which is the whole of what is on screen.
                out_hits->push_back(GuideHit{prompts[index].glyph, box});
            }
        } else {
            glyphs.draw(quads, prompts[index].glyph, pen_x, y, height, color);
            text.draw(quads, prompts[index].action, pen_x + glyph_width + gap, label_y, scale,
                      color);
            if (out_hits != nullptr) {
                // The glyph and its words together: a finger aims at the
                // prompt, not at the picture inside it.
                out_hits->push_back(
                    GuideHit{prompts[index].glyph, PixelRect{pen_x, y, width, height}});
            }
        }
        pen_x += width;
    }
    return height;
}

}  // namespace ds
