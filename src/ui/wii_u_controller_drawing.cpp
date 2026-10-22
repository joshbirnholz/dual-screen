#include "ui/wii_u_controller_drawing.hpp"

#include <algorithm>
#include <cmath>

#include "ui/ui_theme.hpp"

namespace ds {

void draw_wii_u_controller(QuadRenderer& quads, const PixelRect& box, WiiUController controller,
                           WiiUAction picked) {
    const UiTheme& theme = ui_theme();
    // Drawn on a grid of 460 by 520, as the DS and 3DS are, scaled to fit.
    constexpr float kWidth = 460.0f;
    constexpr float kHeight = 520.0f;
    const float unit = std::min(box.w / kWidth, box.h / kHeight);
    const float left = box.x + (box.w - kWidth * unit) * 0.5f;
    const float top = box.y + (box.h - kHeight * unit) * 0.5f;
    const auto at = [&](float x, float y, float w, float h) {
        return PixelRect{std::round(left + x * unit), std::round(top + y * unit), std::round(w * unit),
                         std::round(h * unit)};
    };
    const float stroke = std::max(1.5f, std::round(unit * 3.0f));
    const float thin = std::max(1.0f, std::round(unit * 2.0f));
    const Color dim = theme.text_dim;

    const auto ring = [&](const PixelRect& rect, float radius, float width, const Color& color) {
        quads.draw_rounded_ring(rect, radius, width, color, color);
    };
    const auto shell = [&](float x, float y, float w, float h, float radius) {
        ring(at(x, y, w, h), std::round(radius * unit), stroke, theme.text);
    };
    // A button: filled in the accent colour when it is the one, in outline
    // otherwise.
    const auto part = [&](WiiUAction which, const PixelRect& rect, float radius) {
        if (which == picked) {
            quads.draw_rounded_rect(rect, radius, theme.accent);
        } else {
            ring(rect, radius, thin, theme.text);
        }
    };
    const auto circle = [&](WiiUAction which, float cx, float cy, float r) {
        part(which, at(cx - r, cy - r, r * 2.0f, r * 2.0f), std::round(r * unit));
    };
    // Something with no button of its own, such as a stick that does not
    // click.
    const auto faint_circle = [&](float cx, float cy, float r) {
        ring(at(cx - r, cy - r, r * 2.0f, r * 2.0f), std::round(r * unit), thin, dim);
    };
    const auto dpad = [&](float cx, float cy, float arm, WiiUAction up, WiiUAction down, WiiUAction left_arm,
                          WiiUAction right_arm) {
        const float half = arm * 0.45f;
        const float radius = std::round(2.0f * unit);
        part(up, at(cx - half, cy - arm - half, half * 2.0f, arm + half * 0.2f), radius);
        part(down, at(cx - half, cy + half * 0.8f, half * 2.0f, arm + half * 0.2f), radius);
        part(left_arm, at(cx - arm - half, cy - half, arm + half * 0.2f, half * 2.0f), radius);
        part(right_arm, at(cx + half * 0.8f, cy - half, arm + half * 0.2f, half * 2.0f), radius);
    };
    const auto face = [&](float cx, float cy, float spread, float r) {
        circle(WiiUAction::X, cx, cy - spread, r);
        circle(WiiUAction::Y, cx - spread, cy, r);
        circle(WiiUAction::A, cx + spread, cy, r);
        circle(WiiUAction::B, cx, cy + spread, r);
    };
    const auto shoulders = [&](float y, float l_x, float zl_x, float r_x, float zr_x, float width) {
        part(WiiUAction::Zl, at(zl_x, y - 16, width * 0.75f, 10), std::round(5 * unit));
        part(WiiUAction::L, at(l_x, y, width, 12), std::round(6 * unit));
        part(WiiUAction::Zr, at(zr_x, y - 16, width * 0.75f, 10), std::round(5 * unit));
        part(WiiUAction::R, at(r_x, y, width, 12), std::round(6 * unit));
    };
    // A Wii Remote, upright, `s` times as big as alone, its middle at `cx`.
    const auto remote = [&](float cx, float y, float s) {
        const auto x = [&](float dx) { return cx + dx * s; };
        const auto yy = [&](float dy) { return y + dy * s; };
        shell(x(-40), yy(0), 80 * s, 480 * s, 30 * s);
        faint_circle(x(-22), yy(30), 6 * s);
        dpad(x(0), yy(95), 22 * s, WiiUAction::RemoteUp, WiiUAction::RemoteDown, WiiUAction::RemoteLeft,
             WiiUAction::RemoteRight);
        circle(WiiUAction::RemoteA, x(0), yy(170), 17 * s);
        // B is a trigger on the back: drawn along the remote's side.
        part(WiiUAction::RemoteB, at(x(44), yy(140), 12 * s, 60 * s), std::round(6 * s * unit));
        circle(WiiUAction::RemoteMinus, x(-24), yy(250), 7 * s);
        circle(WiiUAction::RemoteHome, x(0), yy(250), 8 * s);
        circle(WiiUAction::RemotePlus, x(24), yy(250), 7 * s);
        circle(WiiUAction::One, x(0), yy(350), 13 * s);
        circle(WiiUAction::Two, x(0), yy(395), 13 * s);
        for (int light = 0; light < 4; ++light) {
            quads.draw_solid(at(x(-21 + light * 12.0f), yy(450), 6 * s, 6 * s), dim);
        }
    };

    switch (controller) {
        case WiiUController::GamePad: {
            shell(10, 150, 440, 220, 46);
            ring(at(115, 180, 230, 150), std::round(3 * unit), thin, dim);
            shoulders(136, 30, 40, 360, 375, 70);
            circle(WiiUAction::LeftStick, 62, 205, 20);
            dpad(62, 285, 16, WiiUAction::Up, WiiUAction::Down, WiiUAction::Left, WiiUAction::Right);
            circle(WiiUAction::RightStick, 398, 205, 20);
            face(398, 280, 20, 9);
            circle(WiiUAction::Minus, 382, 338, 5);
            circle(WiiUAction::Plus, 414, 338, 5);
            circle(WiiUAction::Home, 230, 350, 7);
            break;
        }
        case WiiUController::Pro: {
            shell(30, 170, 400, 200, 70);
            shoulders(156, 55, 65, 325, 340, 80);
            circle(WiiUAction::LeftStick, 120, 225, 22);
            dpad(120, 305, 18, WiiUAction::Up, WiiUAction::Down, WiiUAction::Left, WiiUAction::Right);
            face(340, 225, 22, 10);
            circle(WiiUAction::RightStick, 340, 305, 22);
            circle(WiiUAction::Minus, 200, 245, 6);
            circle(WiiUAction::Home, 230, 245, 7);
            circle(WiiUAction::Plus, 260, 245, 6);
            break;
        }
        case WiiUController::Remote: remote(230, 20, 1.0f); break;
        case WiiUController::RemoteNunchuk: {
            remote(330, 20, 1.0f);
            // The Nunchuk, its C and Z on the end towards the player's hand.
            shell(60, 130, 120, 270, 58);
            faint_circle(120, 200, 26);
            part(WiiUAction::NunchukZ, at(88, 92, 64, 18), std::round(8 * unit));
            part(WiiUAction::NunchukC, at(98, 116, 44, 12), std::round(6 * unit));
            break;
        }
        case WiiUController::RemoteClassic: {
            // The Classic Controller, and the Wii Remote it is plugged into
            // beside it, smaller.
            shell(10, 160, 340, 170, 56);
            shoulders(146, 30, 110, 260, 215, 70);
            dpad(80, 215, 15, WiiUAction::Up, WiiUAction::Down, WiiUAction::Left, WiiUAction::Right);
            face(282, 215, 19, 9);
            faint_circle(140, 280, 20);
            faint_circle(222, 280, 20);
            circle(WiiUAction::Minus, 158, 215, 5);
            circle(WiiUAction::Home, 181, 215, 6);
            circle(WiiUAction::Plus, 204, 215, 5);
            remote(410, 100, 0.68f);
            break;
        }
    }
}

}  // namespace ds
