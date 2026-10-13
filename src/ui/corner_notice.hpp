#pragma once

#include <algorithm>
#include <cmath>
#include <string>

#include "render/quad_renderer.hpp"
#include "render/text_renderer.hpp"
#include "ui/ui_theme.hpp"

namespace ds {

// A few words in the top right corner, for a moment, over whatever is there,
// with nothing to answer: what a Wii U shows when its HOME Menu cannot open
// right now, said in words. Drawn as a Switch draws its own system alerts --
// a screenshot that cannot be taken, a HOME Menu that cannot open -- a plain
// darkened rectangle, nothing blurred and no border, with the words in this
// app's font.
//
// Measured in a 720-line screen's pixels, the Switch's own, and scaled to the
// window, so it sits the same on a handheld panel and a 4K television.
inline void render_corner_notice(QuadRenderer& quads, TextRenderer& text, float output_width,
                                 float output_height, const std::string& message) {
    const float unit = output_height / 720.0f;
    // Text 20 pixels to the em.
    const float line = std::round(18.0f * unit);
    const float scale = line / TextRenderer::line_height(1.0f);
    const float pad_x = std::round(24.0f * unit);
    const float pad_y = std::round(16.0f * unit);
    const float margin = std::round(24.0f * unit);
    // Up, not to the nearest: a box a fraction of a pixel narrower than its
    // words would cut the last of them off.
    const float words = std::ceil(TextRenderer::measure_width(message, scale));
    const float width = std::min(words + pad_x * 2.0f, output_width - margin * 2.0f);
    const float height = line + pad_y * 2.0f;
    const PixelRect box{std::round(output_width - margin - width), margin, width, height};
    quads.draw_solid(box, Color{0.0f, 0.0f, 0.0f, 0.75f});
    const std::string shown = words + pad_x * 2.0f <= width
                                  ? message
                                  : TextRenderer::elide_right(message, box.w - pad_x * 2.0f, scale);
    text.draw(quads, shown, box.x + pad_x, box.y + pad_y, scale, ui_theme().text);
}

// What it says when the game will not pause for the quick menu.
inline constexpr const char* kMenuUnavailableNotice = "The menu can't be opened right now.";

}  // namespace ds
