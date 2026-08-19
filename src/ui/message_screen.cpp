#include "ui/message_screen.hpp"

#include <algorithm>
#include <cmath>

#include "ui/prompt.hpp"

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
                           float output_width,
                           float output_height) {
    if (!visible_) {
        return;
    }

    const UiTheme& theme = ui_theme();
    const float scale = UiTheme::base_scale(output_height);
    const float line = TextRenderer::line_height(scale);

    // Drawn over whatever is already on screen: fatal messages sit on a cleared
    // frame, a warning sits over the running game.
    quads.draw_solid(PixelRect{0.0f, 0.0f, output_width, output_height}, theme.scrim);

    const float margin = std::round(output_width * 0.08f);
    const float width = output_width - margin * 2.0f;
    float pen_y = std::round(output_height * 0.14f);

    const Color accent = (kind_ == MessageKind::Error) ? theme.danger : theme.accent;
    quads.draw_solid(PixelRect{margin, pen_y - line, std::max(4.0f, scale * 3.0f), line * 2.2f}, accent);

    text.draw(quads, title_, margin + scale * 12.0f, pen_y, scale * 1.8f, theme.text);
    pen_y += line * 3.0f;

    pen_y += text.draw_wrapped(quads, body_, margin, pen_y, width, scale, theme.text);
    pen_y += line;

    for (const std::string& detail : details_) {
        pen_y += text.draw_wrapped(quads, detail, margin, pen_y, width, scale * 0.9f, theme.text_dim);
        pen_y += line * 0.4f;
    }

    text.draw(quads, format_prompts({{"A", "Continue"}, {"Tap", "Continue"}}), output_width * 0.5f,
              output_height - line * 3.0f, scale, theme.text_dim, TextAlign::Center);
}

}  // namespace ds
