#pragma once

#include <algorithm>
#include <cmath>
#include <string>

#include "render/quad_renderer.hpp"
#include "render/text_renderer.hpp"
#include "ui/popup.hpp"
#include "ui/ui_theme.hpp"

namespace ds {

// A few words in the top right corner, for a moment, over whatever is there,
// with nothing to answer: what a Wii U shows when its HOME Menu cannot open
// right now, said in words. In the popups' colours, so it reads as part of the
// same app.
inline void render_corner_notice(QuadRenderer& quads, TextRenderer& text, float output_width,
                                 float output_height, const std::string& message) {
    const UiTheme& theme = ui_theme();
    const float scale = popup_scale(output_height);
    const float line = TextRenderer::line_height(scale);
    const float pad = std::round(line * 1.0f);
    const float margin = std::round(line * 1.2f);
    const float width =
        std::min(std::round(TextRenderer::measure_width(message, scale) + pad * 2.0f),
                 output_width - margin * 2.0f);
    const float height = std::round(line + pad * 1.4f);
    const PixelRect box{std::round(output_width - margin - width), margin, width, height};
    const float radius = std::max(2.0f, std::round(scale * 1.5f));
    quads.draw_rounded_rect(box, radius, popup_rule());
    quads.draw_rounded_rect(PixelRect{box.x + 1.0f, box.y + 1.0f, box.w - 2.0f, box.h - 2.0f},
                            std::max(0.0f, radius - 1.0f), popup_fill());
    text.draw(quads, TextRenderer::elide_right(message, box.w - pad * 2.0f, scale), box.x + pad,
              std::round(box.y + (box.h - line) * 0.5f), scale, theme.text);
}

// What it says when the game will not pause for the quick menu.
inline constexpr const char* kMenuUnavailableNotice = "The menu can't be opened right now.";

}  // namespace ds
