#pragma once

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "input/pad_routing.hpp"
#include "render/quad_renderer.hpp"
#include "render/text_renderer.hpp"
#include "render/texture.hpp"
#include "ui/pad_slot_view.hpp"
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
    // Up, not to the nearest: a box a fraction of a pixel narrower than its
    // words would cut the last of them off.
    const float words = std::ceil(TextRenderer::measure_width(message, scale));
    // Right in the corner, against both edges, as a Switch's own notices sit.
    const float width = std::min(words + pad_x * 2.0f, output_width);
    const float height = line + pad_y * 2.0f;
    const PixelRect box{std::round(output_width - width), 0.0f, width, height};
    quads.draw_solid(box, Color{0.0f, 0.0f, 0.0f, 0.75f});
    const std::string shown = words + pad_x * 2.0f <= width
                                  ? message
                                  : TextRenderer::elide_right(message, box.w - pad_x * 2.0f, scale);
    text.draw(quads, shown, box.x + pad_x, box.y + pad_y, scale, ui_theme().text);
}

// Those, and one about a controller under each other, each against the right
// edge: the plain words at the top, then each controller's, oldest first,
// drawn as its lights or the GamePad -- with what the game has it as over
// them -- before its words. `icons` is the menus' strip, loaded the first
// time it is wanted -- see load_ui_icons().
inline void render_corner_notices(QuadRenderer& quads, TextRenderer& text, Texture& icons, bool& icons_tried,
                                  float output_width, float output_height, const std::string& message,
                                  const std::vector<ControllerNotice>& notices) {
    const float unit = output_height / 720.0f;
    if (!message.empty()) render_corner_notice(quads, text, output_width, output_height, message);
    const float line = std::round(18.0f * unit);
    const float scale = line / TextRenderer::line_height(1.0f);
    const float pad_x = std::round(24.0f * unit);
    const float pad_y = std::round(12.0f * unit);
    const float gap = std::round(4.0f * unit);
    const PadSlotView slot(std::round(13.0f * unit));
    const float icon_height = slot.type_size + slot.type_gap + slot.light;
    const float height = std::max(line, icon_height) + pad_y * 2.0f;
    float top = message.empty() ? 0.0f : std::round(line + 32.0f * unit) + gap;
    const bool have_icons = load_ui_icons(icons, icons_tried);
    for (const ControllerNotice& notice : notices) {
        if (top + height > output_height) break;
        const float words = std::ceil(TextRenderer::measure_width(notice.text, scale));
        const float icon_room = slot.width + std::round(14.0f * unit);
        const float width = std::min(words + icon_room + pad_x * 2.0f, output_width);
        const PixelRect box{std::round(output_width - width), top, width, height};
        quads.draw_solid(box, Color{0.0f, 0.0f, 0.0f, 0.75f});
        const float centre_y = std::round(box.y + box.h * 0.5f);
        slot.draw(quads, text, have_icons ? &icons : nullptr, notice.slot, box.x + pad_x + slot.width * 0.5f, centre_y,
                  scale, ui_theme().text, notice.type, notice.sideways);
        const float text_x = box.x + pad_x + icon_room;
        text.draw(quads, TextRenderer::elide_right(notice.text, box.x + box.w - pad_x - text_x, scale), text_x,
                  std::round(centre_y - line * 0.5f), scale, ui_theme().text);
        top += height + gap;
    }
}

// What it says when the game will not pause for the quick menu.
inline constexpr const char* kMenuUnavailableNotice = "The menu can't be opened right now.";

}  // namespace ds
