#pragma once

#include <algorithm>
#include <cstddef>
#include <string>

#include "input/touch_controller.hpp"
#include "render/quad_renderer.hpp"
#include "render/text_renderer.hpp"

namespace ds {

// The touch controller, drawn over the game -- see TouchController. Light
// enough to play through: each control a dark, see-through disc or capsule
// with a thin white rim and its letter, filled white while a finger is on it,
// the way a Switch's own on-screen buttons answer a touch.
inline void draw_touch_controls(QuadRenderer& quads, TextRenderer& text, const TouchController& touch) {
    if (!touch.shown()) return;
    const Color base{0.0f, 0.0f, 0.0f, 0.32f};
    const Color rim{1.0f, 1.0f, 1.0f, 0.55f};
    const Color held{1.0f, 1.0f, 1.0f, 0.55f};
    const Color label{1.0f, 1.0f, 1.0f, 0.85f};
    const Color label_held{0.1f, 0.1f, 0.1f, 0.9f};

    const auto disc = [&](const PixelRect& rect, const Color& fill) {
        const float radius = (rect.w < rect.h ? rect.w : rect.h) * 0.5f;
        quads.draw_rounded_rect(rect, radius, fill);
        const float thickness = std::max(1.5f, radius * 0.06f);
        quads.draw_rounded_ring(rect, radius, thickness, rim, rim);
    };
    const auto centred = [&](const std::string& words, float x, float y, float height, float width,
                             const Color& color) {
        // `height` is how tall the line is to be; the font's own line height
        // at scale 1 gives the scale for it. Narrowed to `width` if wider.
        float scale = height / TextRenderer::line_height(1.0f);
        // Start sized as Select is, so the two match.
        const float wide = TextRenderer::measure_width(words == "Start" ? std::string("Select") : words, scale);
        if (wide > width && wide > 0.0f) scale *= width / wide;
        text.draw(quads, words, x, y - TextRenderer::line_height(scale) * 0.5f, scale, color, TextAlign::Center);
    };

    const std::vector<TouchController::Control>& controls = touch.controls();
    for (std::size_t index = 0; index < controls.size(); ++index) {
        const TouchController::Control& control = controls[index];
        const bool pressed = touch.pressed(index);
        const float r = control.radius();
        const float cx = control.centre_x();
        const float cy = control.centre_y();
        switch (control.kind) {
            case TouchController::Kind::Button: {
                disc(control.rect, pressed ? held : base);
                const std::string words = control.label;
                // A GamePad's + and -, drawn as bars rather than letters.
                const bool plus = words == "+";
                const bool minus = words == "\xE2\x88\x92";
                if (plus || minus) {
                    const Color bar = pressed ? label_held : label;
                    const float w = r * 0.9f;
                    const float h = std::max(1.5f, r * 0.16f);
                    quads.draw_capsule(PixelRect{cx - w * 0.5f, cy - h * 0.5f, w, h}, bar);
                    if (plus) quads.draw_capsule(PixelRect{cx - h * 0.5f, cy - w * 0.5f, h, w}, bar);
                    break;
                }
                // A letter fills most of its disc; a word on a capsule, most
                // of the capsule's height.
                const bool round = control.rect.w - control.rect.h < 0.5f;
                const float line = round ? r * (words.size() > 1 ? 0.9f : 1.05f) : control.rect.h * 0.62f;
                const float room = round ? r * 1.3f : control.rect.w - control.rect.h * 0.7f;
                centred(words, cx, cy, line, room, pressed ? label_held : label);
                break;
            }
            case TouchController::Kind::DPad: {
                // A cross on a disc, the arm held lit.
                disc(control.rect, base);
                const float arm = r * 0.38f;
                const float reach = r * 0.8f;
                const Color cross{1.0f, 1.0f, 1.0f, 0.22f};
                quads.draw_rounded_rect(PixelRect{cx - reach, cy - arm * 0.5f, reach * 2.0f, arm}, arm * 0.2f, cross);
                quads.draw_rounded_rect(PixelRect{cx - arm * 0.5f, cy - reach, arm, reach * 2.0f}, arm * 0.2f, cross);
                break;
            }
            case TouchController::Kind::Stick: {
                disc(control.rect, base);
                float x = 0.0f;
                float y = 0.0f;
                touch.stick_push(index, x, y);
                const float knob = r * 0.55f;
                const float travel = r - knob;
                const PixelRect top{cx + x * travel - knob, cy + y * travel - knob, knob * 2.0f, knob * 2.0f};
                quads.draw_rounded_rect(top, knob, pressed ? held : Color{1.0f, 1.0f, 1.0f, 0.3f});
                quads.draw_rounded_ring(top, knob, std::max(1.5f, knob * 0.08f), rim, rim);
                break;
            }
            case TouchController::Kind::Menu: {
                // The quick menu: three short bars.
                disc(control.rect, pressed ? held : base);
                const Color bar = pressed ? label_held : label;
                const float w = r * 0.9f;
                const float h = std::max(1.5f, r * 0.13f);
                for (int line = -1; line <= 1; ++line) {
                    quads.draw_capsule(PixelRect{cx - w * 0.5f, cy + line * r * 0.3f - h * 0.5f, w, h}, bar);
                }
                break;
            }
        }
    }
    // The d-pad's arms lit where a finger is pushing, over the cross.
    for (std::size_t index = 0; index < controls.size(); ++index) {
        const TouchController::Control& control = controls[index];
        if (control.kind != TouchController::Kind::DPad) continue;
        const unsigned bits = touch.dpad_held(index);
        const float r = control.radius();
        const float cx = control.centre_x();
        const float cy = control.centre_y();
        const float arm = r * 0.38f;
        const float reach = r * 0.8f;
        const Color lit{1.0f, 1.0f, 1.0f, 0.6f};
        if (bits & 1u) quads.draw_rounded_rect(PixelRect{cx - arm * 0.5f, cy - reach, arm, reach - arm * 0.5f}, arm * 0.2f, lit);
        if (bits & 2u) quads.draw_rounded_rect(PixelRect{cx - arm * 0.5f, cy + arm * 0.5f, arm, reach - arm * 0.5f}, arm * 0.2f, lit);
        if (bits & 4u) quads.draw_rounded_rect(PixelRect{cx - reach, cy - arm * 0.5f, reach - arm * 0.5f, arm}, arm * 0.2f, lit);
        if (bits & 8u) quads.draw_rounded_rect(PixelRect{cx + arm * 0.5f, cy - arm * 0.5f, reach - arm * 0.5f, arm}, arm * 0.2f, lit);
    }
}

}  // namespace ds
