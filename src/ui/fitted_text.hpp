#pragma once

#include <algorithm>
#include <cmath>
#include <string>

#include "render/quad_renderer.hpp"
#include "render/text_renderer.hpp"

namespace ds {

// Text longer than the room it has. At rest it ends in "..."; on the row
// under the cursor it slides along to show the rest, as a Switch's menus do:
// still for a moment, then moving at a reading pace until its end is in view,
// still again, and back to its start.

// How long it stays still at each end, and how fast it moves, in lines of its
// text a second.
constexpr float kFittedTextHoldSeconds = 1.2f;
constexpr float kFittedTextLinesPerSecond = 2.5f;

// How far along it has slid, in pixels, `seconds` after the cursor came to
// it: 0 to `overflow`, the width that does not fit.
inline float fitted_text_offset(float overflow, float seconds, float speed) {
    if (overflow <= 0.0f || speed <= 0.0f || seconds <= 0.0f) return 0.0f;
    const float slide = overflow / speed;
    const float cycle = kFittedTextHoldSeconds * 2.0f + slide;
    float t = std::fmod(seconds, cycle);
    if (t < kFittedTextHoldSeconds) return 0.0f;
    t -= kFittedTextHoldSeconds;
    return t < slide ? t * speed : overflow;
}

// How long one round of sliding takes, still times and all; 0 for text that
// fits. A row with two that do not fit slides them in turn, one round each.
inline float fitted_text_cycle(float overflow, float speed) {
    if (overflow <= 0.0f || speed <= 0.0f) return 0.0f;
    return kFittedTextHoldSeconds * 2.0f + overflow / speed;
}

// When two texts on one row are too long, which slides, and how far into
// its round: the first for a round, then the second, and so on. False for
// the one whose turn it is not.
inline bool fitted_text_turn(float first_cycle, float second_cycle, float seconds, bool first,
                             float& turn_seconds) {
    turn_seconds = seconds;
    if (first_cycle <= 0.0f || second_cycle <= 0.0f) return true;
    const float t = std::fmod(std::max(0.0f, seconds), first_cycle + second_cycle);
    if (first) {
        turn_seconds = t;
        return t < first_cycle;
    }
    turn_seconds = t - first_cycle;
    return t >= first_cycle;
}

// A row's room shared between a name and its value, each as much as it
// needs: both whole where they fit; where they do not, the shorter keeps all
// it needs up to half the room and the longer has the rest.
inline void split_row_room(float room, float label_width, float value_width, float& label_room,
                           float& value_room) {
    room = std::max(0.0f, room);
    if (label_width + value_width <= room) {
        label_room = label_width;
        value_room = value_width;
        return;
    }
    const float half = room * 0.5f;
    if (value_width <= half) {
        value_room = value_width;
        label_room = room - value_width;
    } else if (label_width <= half) {
        label_room = label_width;
        value_room = room - label_width;
    } else {
        label_room = half;
        value_room = half;
    }
}

// Draws `text` in `room` pixels from `x` (its left edge, or its right with
// TextAlign::Right): whole if it fits, else shortened -- or, when `moving`,
// sliding, `seconds` since the cursor came to it.
inline void draw_fitted_text(QuadRenderer& quads, TextRenderer& text, const std::string& value, float x, float y,
                             float room, float scale, const Color& color, TextAlign align = TextAlign::Left,
                             bool moving = false, float seconds = 0.0f) {
    const float width = TextRenderer::measure_width(value, scale);
    if (width <= room || room <= 0.0f) {
        text.draw(quads, value, x, y, scale, color, align);
        return;
    }
    const float line = TextRenderer::line_height(scale);
    const float offset =
        moving ? fitted_text_offset(width - room, seconds, line * kFittedTextLinesPerSecond) : 0.0f;
    // Still at its start, it reads as any shortened text does.
    if (offset <= 0.0f) {
        text.draw(quads, TextRenderer::elide_right(value, room, scale), x, y, scale, color, align);
        return;
    }
    const float left = align == TextAlign::Right ? x - room : x;
    // Only within its room, and within whatever confinement it is already in.
    PixelRect box{left, y - line * 0.25f, room, line * 1.5f};
    const bool was_clipped = quads.clipped();
    const PixelRect was = quads.clip();
    if (was_clipped) {
        const float x0 = std::max(box.x, was.x);
        const float y0 = std::max(box.y, was.y);
        const float x1 = std::min(box.x + box.w, was.x + was.w);
        const float y1 = std::min(box.y + box.h, was.y + was.h);
        box = PixelRect{x0, y0, std::max(0.0f, x1 - x0), std::max(0.0f, y1 - y0)};
    }
    quads.set_clip(box);
    text.draw(quads, value, left - offset, y, scale, color, TextAlign::Left);
    if (was_clipped) {
        quads.set_clip(was);
    } else {
        quads.clear_clip();
    }
}

}  // namespace ds
