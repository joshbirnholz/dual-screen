#include "ui/message_screen.hpp"

#include <algorithm>
#include <cmath>

#include "ui/control_guide.hpp"
#include "ui/page_frame.hpp"

namespace ds {

void MessageScreen::show(MessageKind kind,
                         const std::string& title,
                         const std::string& body,
                         const std::vector<std::string>& details) {
    visible_ = true;
    seconds_visible_ = 0.0;
    kind_ = kind;
    title_ = title;
    body_ = body;
    details_ = details;
}

void MessageScreen::clear() {
    visible_ = false;
    details_.clear();
}

bool MessageScreen::update(const std::vector<UiAction>& actions,
                           const std::vector<PointerEvent>& pointers,
                           double delta_seconds) {
    if (!visible_) {
        return false;
    }
    seconds_visible_ += delta_seconds;
    if (seconds_visible_ < kMessageSettleSeconds) {
        // Still settling: whatever arrived was not aimed at this screen.
        return false;
    }
    for (const UiAction action : actions) {
        if (action == UiAction::Confirm || action == UiAction::Cancel ||
            action == UiAction::ToggleMenu) {
            return true;
        }
    }
    for (const PointerEvent& pointer : pointers) {
        if (pointer.released) {
            return true;
        }
    }
    return false;
}

void MessageScreen::render(QuadRenderer& quads,
                           TextRenderer& text,
                           const ButtonGlyphSheet& glyphs,
                           float output_width,
                           float output_height) {
    if (!visible_) {
        return;
    }

    const UiTheme& theme = ui_theme();
    // The same frame as every other page: see ui/page_frame.hpp.
    const PageFrame frame = page_frame(output_width, output_height);
    const float scale = frame.scale;
    const float line = frame.line;

    // Over the paused game, like every page, when there is one; on the app's
    // own background when there is not.
    draw_page_background(quads, output_width, output_height, over_game_);

    // A bar in front of the title in the colour of what kind of message it is.
    const Color accent = (kind_ == MessageKind::Error) ? theme.danger : theme.accent;
    const float bar_w = std::max(4.0f, scale * 3.0f);
    const float bar_h = TextRenderer::line_height(frame.title_scale());
    quads.draw_solid(PixelRect{frame.panel.x,
                               std::round(frame.panel.y + (frame.header_bottom - frame.panel.y - bar_h) * 0.5f),
                               bar_w, bar_h},
                     accent);
    draw_page_header(quads, text, frame, title_, std::string(), bar_w + line * 0.6f);

    float pen_y = frame.content_top;
    pen_y += text.draw_wrapped(quads, body_, frame.panel.x, pen_y, frame.panel.w, scale, theme.text);
    pen_y += line;

    for (const std::string& detail : details_) {
        pen_y += text.draw_wrapped(quads, detail, frame.panel.x, pen_y, frame.panel.w, scale * 0.9f,
                                   theme.text_dim);
        pen_y += line * 0.4f;
    }

    // Any tap goes on -- see update() -- but the prompt is kept where it was
    // drawn all the same, so a finger held on it lights it up like any other.
    guide_hits_.clear();
    draw_page_guide(quads, text, glyphs, frame, {{ButtonGlyph::A, "Continue"}}, &guide_hits_);
}

void MessageScreen::describe_for_sound(UiSoundView& view) const {
    mix_sound_state(view.rest, visible_ ? 1u : 0u);
    mix_sound_state(view.rest, title_);
    view.error = view.error || (visible_ && kind_ == MessageKind::Error);
}

}  // namespace ds
