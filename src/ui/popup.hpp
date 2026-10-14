#pragma once

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "input/input_manager.hpp"
#include "render/quad_renderer.hpp"
#include "render/text_renderer.hpp"
#include "ui/control_guide.hpp"
#include "ui/ui_theme.hpp"

namespace ds {

// A popup, the way the Switch's own are drawn: a dark box over the dimmed
// screen, what it has to say inset at the top, and its answers as buttons
// along the bottom -- side by side, split by a rule, or, when an answer has a
// detail of its own (a save's date), one above the other. The button with the
// cursor is ringed in the accent colour. Every popup in the app is one of
// these: the quick menu's questions, Data Management's, and the notices.
//
// The words are the caller's to draw, inside `body`; this is the box, the
// buttons and how they are answered.

struct PopupButton {
    std::string label;
    // Drawn at the right of a stacked button, dimmer: a save's date.
    std::string detail;
};

struct PopupGeometry {
    PixelRect box;
    // Where the words go.
    PixelRect body;
    std::vector<PixelRect> buttons;
    bool stacked = false;
};

// The colours the Switch's popups have.
inline Color popup_fill() { return Color{0.145f, 0.149f, 0.157f, 1.0f}; }
inline Color popup_rule() { return Color{0.30f, 0.31f, 0.32f, 1.0f}; }
inline Color popup_focus_fill() { return Color{0.10f, 0.105f, 0.11f, 1.0f}; }

inline float popup_scale(float output_height) { return UiTheme::base_scale(output_height); }
inline float popup_radius(float scale) { return std::round(scale * 6.0f); }

// How wide the words can run, for wrapping them before the box is laid out.
inline float popup_body_width(float output_width, float output_height) {
    const float line = TextRenderer::line_height(popup_scale(output_height));
    const float width = std::round(std::min(output_width * 0.62f, line * 36.0f));
    return width - std::round(line * 2.6f) * 2.0f;
}

inline bool popup_stacks(const std::vector<PopupButton>& buttons) {
    if (buttons.size() > 3) return true;
    for (const PopupButton& button : buttons) {
        if (!button.detail.empty()) return true;
    }
    return false;
}

// Centred on the screen, as tall as the words need plus the row of buttons.
inline PopupGeometry popup_geometry(float output_width, float output_height, float body_height,
                                    const std::vector<PopupButton>& buttons) {
    const float line = TextRenderer::line_height(popup_scale(output_height));
    const float width = std::round(std::min(output_width * 0.62f, line * 36.0f));
    const float inset = std::round(line * 2.6f);
    const float pad_top = std::round(line * 2.2f);
    const float pad_bottom = std::round(line * 2.2f);
    const float button_h = std::round(line * 3.0f);
    PopupGeometry geometry;
    geometry.stacked = popup_stacks(buttons);
    const float buttons_h =
        buttons.empty() ? 0.0f
                        : button_h * (geometry.stacked ? static_cast<float>(buttons.size()) : 1.0f);
    const float height = std::round(pad_top + body_height + pad_bottom + buttons_h);
    geometry.box = PixelRect{std::round((output_width - width) * 0.5f),
                             std::round((output_height - height) * 0.5f), width, height};
    geometry.body = PixelRect{geometry.box.x + inset, geometry.box.y + pad_top, width - inset * 2.0f,
                              body_height};
    const float row_top = geometry.box.y + height - buttons_h;
    const int count = static_cast<int>(buttons.size());
    for (int index = 0; index < count; ++index) {
        if (geometry.stacked) {
            geometry.buttons.push_back(
                PixelRect{geometry.box.x, row_top + button_h * index, width, button_h});
        } else {
            const float left = std::round(geometry.box.x + width * index / count);
            const float right = std::round(geometry.box.x + width * (index + 1) / count);
            geometry.buttons.push_back(PixelRect{left, row_top, right - left, button_h});
        }
    }
    return geometry;
}

// Which button a finger is on, or -1.
inline int popup_button_at(const PopupGeometry& geometry, float x, float y) {
    for (std::size_t index = 0; index < geometry.buttons.size(); ++index) {
        if (geometry.buttons[index].contains(x, y)) return static_cast<int>(index);
    }
    return -1;
}

// The dimmed screen, the box and its buttons. `show_focus` false leaves the
// cursor off, as the Switch does until a button or the pad is used.
inline void draw_popup(QuadRenderer& quads, TextRenderer& text, float output_width,
                       float output_height, const PopupGeometry& geometry,
                       const std::vector<PopupButton>& buttons, int focus, bool show_focus) {
    const UiTheme& theme = ui_theme();
    const float scale = popup_scale(output_height);
    const float line = TextRenderer::line_height(scale);
    quads.draw_solid(PixelRect{0.0f, 0.0f, output_width, output_height},
                     Color{0.0f, 0.0f, 0.0f, 0.6f});
    // Well rounded, as a Switch's popups are.
    const float radius = popup_radius(scale);
    quads.draw_rounded_ring(geometry.box, radius, geometry.box.w + geometry.box.h, popup_fill(),
                            popup_fill());
    quads.draw_rounded_ring(geometry.box, radius, 1.0f, popup_rule(), popup_rule());

    const float rule = std::max(1.0f, std::round(scale * 0.5f));
    for (std::size_t index = 0; index < geometry.buttons.size(); ++index) {
        const PixelRect& button = geometry.buttons[index];
        // A rule along the top of the first row, and between the others.
        if (index == 0 || geometry.stacked) {
            quads.draw_solid(PixelRect{geometry.box.x, button.y, geometry.box.w, rule}, popup_rule());
        } else {
            quads.draw_solid(PixelRect{button.x, button.y, rule, button.h}, popup_rule());
        }
    }
    for (std::size_t index = 0; index < geometry.buttons.size() && index < buttons.size(); ++index) {
        const PixelRect& button = geometry.buttons[index];
        const bool focused = show_focus && static_cast<int>(index) == focus;
        if (focused) {
            // Ringed, the ring standing a little proud of the button.
            const float ring = std::max(3.0f, std::round(scale * 1.4f));
            const float out = std::round(ring * 0.6f);
            const PixelRect outer{button.x - out, button.y - out, button.w + out * 2.0f,
                                  button.h + out * 2.0f};
            // Barely rounded, but for a corner it shares with the box, which
            // it follows from just outside -- as a Switch's popups draw it.
            const float small = std::max(3.0f, std::round(scale * 2.0f));
            const float big = radius + out;
            const std::size_t last = geometry.buttons.size() - 1;
            const bool bottom = geometry.stacked ? index == last : true;
            const bool left_end = geometry.stacked || index == 0;
            const bool right_end = geometry.stacked || index == last;
            const float radii[4] = {small, small, bottom && right_end ? big : small,
                                    bottom && left_end ? big : small};
            quads.draw_rounded_ring(outer, radii, outer.w + outer.h, popup_focus_fill(),
                                    popup_focus_fill());
            quads.draw_rounded_ring(outer, radii, ring, menu_ring_left(), menu_ring_right());
        }
        const PopupButton& label = buttons[index];
        const float text_y = std::round(button.y + (button.h - line) * 0.5f);
        if (geometry.stacked) {
            const float pad = std::round(line * 1.2f);
            const float detail_scale = scale * 0.85f;
            float room = button.w - pad * 2.0f;
            if (!label.detail.empty()) {
                text.draw(quads, label.detail, button.x + button.w - pad,
                          std::round(button.y + (button.h - TextRenderer::line_height(detail_scale)) * 0.5f),
                          detail_scale, theme.text_dim, TextAlign::Right);
                room -= TextRenderer::measure_width(label.detail, detail_scale) + line;
            }
            text.draw(quads, TextRenderer::elide_right(label.label, room, scale), button.x + pad,
                      text_y, scale, theme.text);
        } else {
            text.draw(quads, TextRenderer::elide_right(label.label, button.w - line, scale),
                      button.x + button.w * 0.5f, text_y, scale, theme.text, TextAlign::Center);
        }
    }
}

// What a popup says: a message in the ordinary colour, then any further lines
// smaller and dimmer -- the first `plain` of them in the ordinary colour too.
struct PopupText {
    std::string message;
    std::vector<std::string> lines;
    std::size_t plain = 0;
};

inline float popup_text_height(const PopupText& words, float width, float output_height) {
    const float scale = popup_scale(output_height);
    const float line = TextRenderer::line_height(scale);
    const float message_scale = scale * 1.05f;
    const float body_scale = scale * 0.85f;
    float height = words.message.empty()
                       ? 0.0f
                       : TextRenderer::wrapped_line_step(message_scale) *
                             static_cast<float>(TextRenderer::wrap(words.message, width, message_scale).size());
    for (const std::string& entry : words.lines) {
        height += line * 0.5f + TextRenderer::wrapped_line_step(body_scale) *
                                    static_cast<float>(TextRenderer::wrap(entry, width, body_scale).size());
    }
    return height;
}

// Returns where it stopped.
inline float draw_popup_text(QuadRenderer& quads, TextRenderer& text, const PopupText& words,
                             float x, float y, float width, float output_height) {
    const UiTheme& theme = ui_theme();
    const float scale = popup_scale(output_height);
    const float line = TextRenderer::line_height(scale);
    if (!words.message.empty()) {
        y += text.draw_wrapped(quads, words.message, x, y, width, scale * 1.05f, theme.text);
    }
    for (std::size_t index = 0; index < words.lines.size(); ++index) {
        y += line * 0.5f;
        y += text.draw_wrapped(quads, words.lines[index], x, y, width, scale * 0.85f,
                               index < words.plain ? theme.text : theme.text_dim);
    }
    return y;
}

// The cursor over a popup's buttons, and how presses and taps answer it.
class PopupCursor {
public:
    static constexpr int kNone = -1;
    static constexpr int kCancelled = -2;

    // Up again with `count` buttons, the cursor on `default_focus`. With a
    // pad the cursor shows from the start; with a finger, from the first
    // tap -- the first tap on a button only puts the cursor there, and a
    // second presses it, so a finger that lands on the wrong one does
    // nothing it cannot take back.
    void reset(int count, int default_focus) {
        count_ = count;
        focus_ = std::clamp(default_focus, 0, std::max(0, count - 1));
        shown_ = prompt_style() != PromptStyle::Touch;
    }
    int focus() const { return focus_; }
    bool shown() const { return shown_; }
    int count() const { return count_; }
    // From the other device, which answers the same popup.
    void set_focus(int focus) {
        if (focus >= 0 && focus < count_) {
            focus_ = focus;
            shown_ = true;
        }
    }

    // The cursor off, until the next press or tap puts it back.
    void hide() { shown_ = false; }

    // The button pressed this pass, kCancelled for B, or kNone. Left and
    // Right (or Up and Down, where the buttons are stacked) move the cursor,
    // stopping at the ends.
    int update(const std::vector<UiAction>& actions, const std::vector<PointerEvent>& pointers,
               const PopupGeometry& geometry) {
        for (const UiAction action : actions) {
            const bool back = geometry.stacked ? action == UiAction::Up : action == UiAction::Left;
            const bool on = geometry.stacked ? action == UiAction::Down : action == UiAction::Right;
            if (back || on) {
                if (shown_) focus_ = std::clamp(focus_ + (on ? 1 : -1), 0, std::max(0, count_ - 1));
                shown_ = true;
            }
            if (action == UiAction::Confirm && count_ > 0) return focus_;
            if (action == UiAction::Cancel) return kCancelled;
        }
        for (const PointerEvent& pointer : pointers) {
            for (std::size_t index = 0; index < geometry.buttons.size(); ++index) {
                if (!tapped(pointer, geometry.buttons[index])) continue;
                const int answer = tap(static_cast<int>(index));
                if (answer != kNone) return answer;
            }
        }
        return kNone;
    }
    // A tap on a button, by its index: pressed, where it is the only one or
    // already under the cursor; otherwise the cursor goes to it. What is
    // pressed, or kNone.
    int tap(int index) {
        if (index < 0 || index >= count_) return kNone;
        if (count_ == 1 || (shown_ && focus_ == index)) return index;
        focus_ = index;
        shown_ = true;
        return kNone;
    }

private:
    int count_ = 0;
    int focus_ = 0;
    bool shown_ = true;
};

}  // namespace ds
