#pragma once

#include <algorithm>

#include "layout/layout.hpp"
#include "net/protocol.hpp"
#include "render/quad_renderer.hpp"

namespace ds {

// The stylus cursor drawn over the touch screen, wherever this device shows
// it: a ring with a dark edge, so it shows on a white screen and a black one,
// filled in while it is touching. Drawn by whichever device shows the touch
// screen, from the host's cursor -- see StylusCursor and StylusCursorMessage.
inline void draw_stylus_cursor(QuadRenderer& quads, const Layout& layout, float width, float height,
                               const SystemScreens& screens, float display_width, float display_height,
                               const net::StylusCursorMessage& cursor) {
    if (cursor.opacity <= 0.0f) return;
    float x = 0.0f;
    float y = 0.0f;
    if (!map_ds_point_to_output(layout, width, height, cursor.u, cursor.v, x, y, screens, display_width,
                                display_height)) {
        return;
    }
    // Big enough to find at a glance on a television, small enough not to
    // hide what it is pointing at on a handheld.
    const float size = std::clamp(std::min(width, height) * 0.04f, 18.0f, 44.0f);
    const float edge = std::max(2.0f, size * 0.12f);
    const float alpha = cursor.opacity;
    const PixelRect outer{x - size * 0.5f - edge, y - size * 0.5f - edge, size + edge * 2.0f, size + edge * 2.0f};
    const PixelRect ring{x - size * 0.5f, y - size * 0.5f, size, size};
    const Color dark{0.0f, 0.0f, 0.0f, 0.55f * alpha};
    const Color light{1.0f, 1.0f, 1.0f, 0.95f * alpha};
    quads.draw_rounded_ring(outer, outer.h * 0.5f, edge * 2.0f + size * 0.18f, dark, dark);
    quads.draw_rounded_ring(ring, ring.h * 0.5f, cursor.pressed ? ring.h : size * 0.18f, light, light);
    // Its point, exactly where the touch lands.
    const float dot = std::max(2.0f, size * 0.12f);
    quads.draw_capsule(PixelRect{x - dot * 0.5f, y - dot * 0.5f, dot, dot},
                       cursor.pressed ? Color{0.0f, 0.0f, 0.0f, 0.8f * alpha} : light);
}

}  // namespace ds
