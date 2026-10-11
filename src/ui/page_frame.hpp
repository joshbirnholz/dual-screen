#pragma once

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "render/button_glyphs.hpp"
#include "render/quad_renderer.hpp"
#include "render/text_renderer.hpp"
#include "ui/control_guide.hpp"
#include "ui/ui_theme.hpp"

namespace ds {

// Where everything on a full-screen page goes -- the main menu, the file
// picker, Settings, Controllers, Data Management, the device list, the
// screens that only say something. One frame for all of them, so moving from
// one to the next never moves the title, the list or the row of prompts: the
// same margins, the title the same size in the same place with a rule under
// it, the content starting on the same line, and the prompts along the same
// row at the bottom, right-aligned, over a rule of their own.
struct PageFrame {
    float scale = 1.0f;
    float line = 0.0f;
    // The whole page inside its margins.
    PixelRect panel;
    // The heading sits above this, and its rule along it.
    float header_bottom = 0.0f;
    // Where the page's own content goes: lists start at the top of it.
    float content_top = 0.0f;
    float content_bottom = 0.0f;
    // The row of prompts, and the rule above it.
    float footer_rule_y = 0.0f;
    float guide_y = 0.0f;

    PixelRect content() const {
        return PixelRect{panel.x, content_top, panel.w, content_bottom - content_top};
    }
    float title_scale() const { return scale * 1.45f; }
    float guide_scale() const { return scale * 0.9f; }
};

inline PageFrame page_frame(float output_width, float output_height) {
    PageFrame frame;
    frame.scale = UiTheme::base_scale(output_height);
    frame.line = TextRenderer::line_height(frame.scale);
    const float margin_x = std::round(output_width * 0.06f);
    const float margin_y = std::round(output_height * 0.06f);
    frame.panel = PixelRect{margin_x, margin_y, output_width - margin_x * 2.0f,
                            output_height - margin_y * 2.0f};
    frame.header_bottom = std::round(frame.panel.y + frame.line * 2.6f);
    frame.content_top = std::round(frame.header_bottom + frame.line * 0.6f);
    frame.guide_y = frame.panel.y + frame.panel.h - control_guide_height(frame.guide_scale());
    frame.content_bottom = std::round(frame.guide_y - frame.line * 0.9f);
    frame.footer_rule_y = std::round(frame.content_bottom + frame.line * 0.45f);
    return frame;
}

inline Color page_rule_color() {
    Color rule = ui_theme().text_dim;
    rule.a = 0.3f;
    return rule;
}

// The page's ground: the app's background -- or, over a paused game, nothing,
// since the app has already put the game down behind the page blurred and
// darkened (see MenuBackdrop), exactly as it does behind the quick menu, and
// every page over a game looks the same as that.
inline void draw_page_background(QuadRenderer& quads, float output_width, float output_height,
                                 bool over_game = false) {
    if (over_game) return;
    quads.draw_solid(PixelRect{0.0f, 0.0f, output_width, output_height}, ui_theme().background);
}

// The heading: the title at the left -- after `title_indent`, where a page
// puts a picture of its own in front of it -- and, when there is one, a line
// of detail at the right in the dimmer colour, shortened to fit: a path keeps
// its end, anything else its start. Then the rules under the heading and over
// the prompts, unless `rules` is false -- the quick menu, which stands over
// the game rather than being a page of its own, goes without.
inline void draw_page_header(QuadRenderer& quads, TextRenderer& text, const PageFrame& frame,
                             const std::string& title, const std::string& detail = std::string(),
                             float title_indent = 0.0f, bool detail_is_path = false,
                             float right_reserved = 0.0f, bool rules = true) {
    const UiTheme& theme = ui_theme();
    const float title_scale = frame.title_scale();
    const float header_h = frame.header_bottom - frame.panel.y;
    const float title_y =
        std::round(frame.panel.y + (header_h - TextRenderer::line_height(title_scale)) * 0.5f);
    const float title_x = frame.panel.x + title_indent;
    const float right = frame.panel.x + frame.panel.w - right_reserved;
    float title_room = right - title_x;
    if (!detail.empty()) {
        const float detail_scale = frame.scale * 0.85f;
        const float title_width =
            std::min(TextRenderer::measure_width(title, title_scale), title_room * 0.6f);
        const float detail_room = right - title_x - title_width - frame.line * 1.5f;
        const std::string shown = detail_is_path
                                      ? TextRenderer::elide_left(detail, detail_room, detail_scale)
                                      : TextRenderer::elide_right(detail, detail_room, detail_scale);
        // On the title's line, sharing its baseline as near as two sizes can.
        const float detail_y = title_y + TextRenderer::line_height(title_scale) -
                               TextRenderer::line_height(detail_scale) - frame.line * 0.12f;
        text.draw(quads, shown, right, std::round(detail_y), detail_scale, theme.text_dim,
                  TextAlign::Right);
        title_room -= TextRenderer::measure_width(shown, detail_scale) + frame.line * 1.5f;
    }
    text.draw(quads, TextRenderer::elide_right(title, title_room, title_scale), title_x, title_y,
              title_scale, theme.text);
    if (!rules) return;
    const Color rule = page_rule_color();
    quads.draw_solid(PixelRect{frame.panel.x, frame.header_bottom, frame.panel.w, 1.0f}, rule);
    quads.draw_solid(PixelRect{frame.panel.x, frame.footer_rule_y, frame.panel.w, 1.0f}, rule);
}

// A line about the state of things -- "Looking for devices...", "No
// controllers found" -- at the left of the prompts' row, its words level with
// theirs.
inline void draw_page_status(QuadRenderer& quads, TextRenderer& text,
                             const ButtonGlyphSheet& glyphs, const PageFrame& frame,
                             const std::string& status, const Color& color) {
    if (status.empty()) return;
    const float status_scale = frame.guide_scale();
    const float y = frame.guide_y +
                    control_guide_height(frame.guide_scale()) * glyphs.button_centre() -
                    TextRenderer::line_height(status_scale) * 0.5f;
    text.draw(quads, status, frame.panel.x, std::round(y), status_scale, color);
}

// The prompts in the order every page shows them: the page's own first, in
// the order it gives them, then the cancel button, then the confirm button --
// so OK is always at the right-hand end with Back beside it, the way the
// Switch's own menus have them, whichever page or dialog this is. By the
// logical buttons: when confirm and cancel are swapped, the prompts are drawn
// swapped too, and keep their places.
inline std::vector<GuidePrompt> page_guide_order(const std::vector<GuidePrompt>& prompts) {
    const auto rank = [](const GuidePrompt& prompt) {
        if (prompt.glyph == ButtonGlyph::A) return 2;
        if (prompt.glyph == ButtonGlyph::B) return 1;
        return 0;
    };
    std::vector<GuidePrompt> ordered;
    for (int wanted = 0; wanted <= 2; ++wanted) {
        for (const GuidePrompt& prompt : prompts) {
            if (rank(prompt) == wanted) ordered.push_back(prompt);
        }
    }
    return ordered;
}

// The row of prompts, right-aligned along the bottom.
inline void draw_page_guide(QuadRenderer& quads, TextRenderer& text,
                            const ButtonGlyphSheet& glyphs, const PageFrame& frame,
                            const std::vector<GuidePrompt>& prompts,
                            std::vector<GuideHit>* out_hits = nullptr) {
    draw_control_guide(quads, text, glyphs, page_guide_order(prompts),
                       frame.panel.x + frame.panel.w, frame.guide_y, frame.guide_scale(),
                       ui_theme().text, TextAlign::Right, out_hits);
}

// A line of words in a list, drawn at the size every list uses, centred on
// its row.
inline float page_row_text_y(const PixelRect& row, float scale) {
    return std::round(row.y + (row.h - TextRenderer::line_height(scale)) * 0.5f);
}

}  // namespace ds
