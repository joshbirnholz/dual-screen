#pragma once

#include <algorithm>
#include <cmath>

#include "input/pad_routing.hpp"
#include "render/quad_renderer.hpp"
#include "render/text_renderer.hpp"
#include "render/texture.hpp"
#include "ui/ui_icons.hpp"

namespace ds {

// Which player a controller is, the way a Wii Remote's own lights say it:
// four squares in a row, the one for its number filled -- only that one, a
// Wii U's way, where a Switch fills every square up to it. A Wii U stops at
// four; past that, a Switch's patterns for five to eight. The GamePad is
// drawn as one, centred over the same column the lights fill; Not used is
// nothing. A player's lights can have what the game has it as over them,
// drawn as that controller. `line` is the text's line height, which sizes it
// all.
struct PadSlotView {
    float light = 0.0f;
    float light_gap = 0.0f;
    float lights_width = 0.0f;
    float gamepad_size = 0.0f;
    // A player's controller over its lights, and the gap between.
    float type_size = 0.0f;
    float type_gap = 0.0f;
    // How wide the column it is drawn in is.
    float width = 0.0f;

    explicit PadSlotView(float line) {
        light = std::round(line * 0.62f);
        light_gap = std::round(light * 0.42f);
        lights_width = light * 4.0f + light_gap * 3.0f;
        // Its icon sits in a band half as tall as the square it is drawn in,
        // nearly the square's whole width -- see ui/ui_icons.hpp.
        gamepad_size = std::round(line * 2.9f);
        type_size = std::round(line * 2.4f);
        type_gap = std::round(line * 0.15f);
        width = std::max(lights_width, gamepad_size * 0.96f);
    }

    // Centred on (`centre_x`, `cy`). `icons` is the menus' icon strip, or null
    // where it would not load, when the GamePad is its name instead. `type`
    // is what the game has a player as -- see wii_u_type_icon() -- drawn
    // over its lights, the two centred together; empty for none.
    void draw(QuadRenderer& quads, TextRenderer& text, const Texture* icons, PadSlot slot, float centre_x,
              float cy, float scale, const Color& color, const std::string& type = std::string()) const {
        if (slot.kind == PadSlot::Kind::GamePad) {
            if (icons != nullptr && icons->valid()) {
                draw_ui_icon(quads, *icons, UiIcon::WiiUGamePad,
                             PixelRect{std::round(centre_x - gamepad_size * 0.5f), std::round(cy - gamepad_size * 0.5f),
                                       gamepad_size, gamepad_size},
                             color);
            } else {
                text.draw(quads, "GamePad", centre_x, std::round(cy - TextRenderer::line_height(scale) * 0.5f), scale,
                          color, TextAlign::Center);
            }
            return;
        }
        if (slot.kind != PadSlot::Kind::Player) return;
        const UiIcon icon = wii_u_type_icon(type);
        if (icon != UiIcon::Count && icons != nullptr && icons->valid()) {
            const float top = std::round(cy - (type_size + type_gap + light) * 0.5f);
            draw_ui_icon(quads, *icons, icon,
                         PixelRect{std::round(centre_x - type_size * 0.5f), top, type_size, type_size}, color);
            cy = top + type_size + type_gap + light * 0.5f;
        }
        static constexpr unsigned kLit[8] = {0b0001, 0b0010, 0b0100, 0b1000, 0b1001, 0b0101, 0b1101, 0b0110};
        const unsigned lit = kLit[std::clamp(slot.player, 0, 7)];
        const float thickness = std::max(1.0f, std::round(light * 0.16f));
        for (int index = 0; index < 4; ++index) {
            const PixelRect square{std::round(centre_x - lights_width * 0.5f) +
                                       static_cast<float>(index) * (light + light_gap),
                                   std::round(cy - light * 0.5f), light, light};
            if (lit & (1u << index)) {
                quads.draw_solid(square, color);
            } else {
                quads.draw_outline(square, color, thickness);
            }
        }
    }
};

}  // namespace ds
