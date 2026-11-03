#pragma once

#include <algorithm>
#include <cmath>
#include <string>

#include "render/quad_renderer.hpp"
#include "render/text_renderer.hpp"

namespace ds {

// Text longer than the room it has. At rest it ends in "..."; on the row
// under the cursor it scrolls round as a Switch's menus do: still for a
// moment, then moving left at a reading pace all the way off, with a copy of
// it following a gap behind, until that copy is where the text started;
// still there for a moment, and round again.

// How long it stays still at its start, how fast it moves, in lines of its
// text a second, and the gap between it and the copy that follows, in lines.
constexpr float kFittedTextHoldSeconds = 1.2f;
constexpr float kFittedTextLinesPerSecond = 2.5f;
constexpr float kFittedTextGapLines = 1.6f;

// How far one round moves it, in pixels: its whole width and the gap after
// it; 0 for text that fits its room.
inline float fitted_text_distance(float width, float room, float line) {
    if (width <= room || room <= 0.0f) return 0.0f;
    return width + line * kFittedTextGapLines;
}

// How far along it has moved, in pixels, `seconds` after the cursor came to
// it: 0 to `distance`, where the copy has taken its place and it starts over.
inline float fitted_text_offset(float distance, float seconds, float speed) {
    if (distance <= 0.0f || speed <= 0.0f || seconds <= 0.0f) return 0.0f;
    const float cycle = kFittedTextHoldSeconds + distance / speed;
    const float t = std::fmod(seconds, cycle);
    if (t < kFittedTextHoldSeconds) return 0.0f;
    return std::min(distance, (t - kFittedTextHoldSeconds) * speed);
}

// How long one round takes, its still time and all; 0 for text that fits. A
// row with two that do not fit scrolls them in turn, one round each.
inline float fitted_text_cycle(float distance, float speed) {
    if (distance <= 0.0f || speed <= 0.0f) return 0.0f;
    return kFittedTextHoldSeconds + distance / speed;
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
// scrolling round, `seconds` since the cursor came to it.
inline void draw_fitted_text(QuadRenderer& quads, TextRenderer& text, const std::string& value, float x, float y,
                             float room, float scale, const Color& color, TextAlign align = TextAlign::Left,
                             bool moving = false, float seconds = 0.0f) {
    const float width = TextRenderer::measure_width(value, scale);
    if (width <= room || room <= 0.0f) {
        text.draw(quads, value, x, y, scale, color, align);
        return;
    }
    const float line = TextRenderer::line_height(scale);
    const float distance = fitted_text_distance(width, room, line);
    const float offset =
        moving ? fitted_text_offset(distance, seconds, line * kFittedTextLinesPerSecond) : 0.0f;
    // Before it first moves, it reads as any shortened text does; once it has
    // been round, it rests whole, cut off at its room's edge, where its copy
    // stopped.
    if (offset <= 0.0f && (!moving || seconds < kFittedTextHoldSeconds)) {
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
    // The copy that follows it round, a gap behind.
    if (offset > distance - room) {
        text.draw(quads, value, left - offset + distance, y, scale, color, TextAlign::Left);
    }
    if (was_clipped) {
        quads.set_clip(was);
    } else {
        quads.clear_clip();
    }
}

}  // namespace ds
