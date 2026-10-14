#pragma once

#include <algorithm>
#include <cmath>
#include <string>

#include "render/quad_renderer.hpp"

namespace ds {

// One place for the colours and metrics the menus share, so screens stay
// visually consistent as more of them get added.
struct UiTheme {
    Color background{0.05f, 0.06f, 0.09f, 1.0f};
    Color scrim{0.0f, 0.0f, 0.0f, 0.72f};
    Color panel{0.10f, 0.12f, 0.17f, 1.0f};
    Color panel_focused{0.16f, 0.21f, 0.30f, 1.0f};
    Color accent{0.36f, 0.72f, 1.0f, 1.0f};
    Color text{0.92f, 0.94f, 0.97f, 1.0f};
    Color text_dim{0.60f, 0.65f, 0.74f, 1.0f};
    Color danger{1.0f, 0.45f, 0.40f, 1.0f};
    Color warning{1.0f, 0.78f, 0.35f, 1.0f};
    Color success{0.45f, 0.85f, 0.55f, 1.0f};

    // Screen swatches used in layout previews.
    Color top_screen{0.24f, 0.47f, 0.85f, 1.0f};
    Color bottom_screen{0.90f, 0.60f, 0.22f, 1.0f};

    // Text scales are derived from the output height so the UI is readable on a
    // 7" Deck panel and on a 4K TV without separate layouts.
    static float base_scale(float output_height) {
        const float scale = output_height / 320.0f;
        return scale < 1.0f ? 1.0f : scale;
    }
};

// How solid the layout in use is drawn when the cursor is somewhere else. Light
// enough for the black text it carries to stay readable over a blurred game,
// which is what rules out going much below this.
constexpr float kSelectedTileOpacity = 0.75f;

// The theme, its accent the player's own where they have chosen one -- see
// set_ui_accent().
inline UiTheme& mutable_ui_theme() {
    static UiTheme theme;
    return theme;
}
inline const UiTheme& ui_theme() { return mutable_ui_theme(); }

// The cursor ring's two ends, which follow the accent: see set_ui_accent().
struct AccentRing {
    Color left{0.36f, 0.72f, 1.0f, 1.0f};
    Color right{0.62f, 0.52f, 1.0f, 1.0f};
};
inline AccentRing& accent_ring() {
    static AccentRing ring;
    return ring;
}

// The accent everything is drawn in -- the cursor's ring, a chosen tab's
// words, a switch turned on -- made the player's favorite color, exactly as
// the DS shows it, solid throughout.
inline void set_ui_accent(const Color& color) {
    const Color solid{color.r, color.g, color.b, 1.0f};
    mutable_ui_theme().accent = solid;
    accent_ring().left = solid;
    accent_ring().right = solid;
}
// Back to the app's own blue, and its ring's run into violet.
inline void reset_ui_accent() {
    mutable_ui_theme().accent = UiTheme().accent;
    accent_ring() = AccentRing();
}

// How thick a menu's outlines are: two pixels at the size the design was drawn
// at, scaled with everything else so a 4K panel does not get a hairline.
inline float menu_border(float scale) { return std::max(2.0f, std::round(scale * 0.6f)); }

// How tall a line in a list is, given the height of a line of its text: room
// round the words, the way the menus were drawn in the design, rather than a
// box just big enough for them. Two-line rows -- a title with a line under it
// -- are taller.
inline float menu_row_height(float line) { return std::round(line * 2.9f); }
inline float menu_two_line_row_height(float line) { return std::round(line * 4.0f); }
// The space between two rows, and inside a row before its words start.
inline float menu_row_gap(float line) { return std::round(line * 0.3f); }
inline float menu_row_pad(float line) { return std::round(line * 0.9f); }

// The cursor's ring, as a Switch draws it: the accent running across the row
// into another shade -- violet for the app's own blue -- see set_ui_accent().
inline Color menu_ring_left() { return accent_ring().left; }
inline Color menu_ring_right() { return accent_ring().right; }

// The cursor's ring and the faint fill inside it, round `rect`.
inline void draw_menu_cursor(QuadRenderer& quads, const PixelRect& rect, float radius, float scale) {
    const float ring = std::max(2.0f, std::round(scale * 1.1f));
    quads.draw_rounded_ring(rect, radius, rect.w + rect.h, Color{1.0f, 1.0f, 1.0f, 0.07f},
                            Color{1.0f, 1.0f, 1.0f, 0.07f});
    quads.draw_rounded_ring(rect, radius, ring, menu_ring_left(), menu_ring_right());
}

// A line in a list -- the launcher, the settings, the file browser, Data
// Management. Only the one under the cursor is drawn at all: a box around
// every line turns a short list into a grid and makes the cursor harder to
// find, not easier. That one is ringed, the way a Switch's own menus ring it,
// with whatever is behind it still showing through.
inline void draw_menu_row(QuadRenderer& quads, const PixelRect& rect, bool focused, float scale) {
    if (!focused) return;
    draw_menu_cursor(quads, rect, std::round(std::min(rect.h * 0.22f, scale * 6.0f)), scale);
}

// An on/off value: a switch, on at the right in the accent colour, off at the
// left in grey -- ending at `right`, centred on `centre_y`. Returns its width.
inline float draw_toggle(QuadRenderer& quads, float right, float centre_y, bool on, float line) {
    const float height = std::round(line * 1.15f);
    const float width = std::round(height * 1.85f);
    const PixelRect track{std::round(right - width), std::round(centre_y - height * 0.5f), width, height};
    quads.draw_capsule(track, on ? ui_theme().accent : Color{0.36f, 0.38f, 0.43f, 1.0f});
    const float inset = std::max(2.0f, std::round(height * 0.12f));
    const float knob = height - inset * 2.0f;
    const float knob_x = on ? track.x + track.w - inset - knob : track.x + inset;
    quads.draw_capsule(PixelRect{knob_x, track.y + inset, knob, knob}, Color{0.97f, 0.97f, 0.98f, 1.0f});
    return width;
}

// Whether a value is a switch's: what an on/off row says.
inline bool is_toggle_value(const std::string& value) { return value == "On" || value == "Off"; }

// A page's left-hand list, where a page has two panes and the right shows
// the one chosen on the left -- the Graphics Packs page: the chosen one's
// words in the accent colour, a bar beside it, and the cursor's ring round it
// while the cursor is there.
inline void draw_side_tab(QuadRenderer& quads, const PixelRect& rect, bool chosen, bool focused,
                          float scale) {
    if (focused) draw_menu_row(quads, rect, true, scale);
    if (!chosen) return;
    const float bar = std::max(3.0f, std::round(scale * 1.3f));
    const float inset = std::round(rect.h * 0.2f);
    const PixelRect line{rect.x + bar * 1.5f, rect.y + inset, bar, rect.h - inset * 2.0f};
    quads.draw_rounded_ring(line, bar * 0.5f, line.w + line.h, ui_theme().accent, ui_theme().accent);
}

// A faint rule under a row, for lists whose rows run on from one another
// without gaps: Data Management's saves and actions.
inline void draw_menu_rule(QuadRenderer& quads, const PixelRect& rect) {
    Color rule = ui_theme().text_dim;
    rule.a = 0.25f;
    quads.draw_solid(PixelRect{rect.x, rect.y + rect.h - 1.0f, rect.w, 1.0f}, rule);
}


// How wide a scroll bar is, and the one every list that runs past its bottom
// draws at its right-hand edge: a dark track, and the part of the list in view
// in the accent colour. `first` and `shown` are fractions of the whole list.
inline float scroll_bar_width(float scale) { return std::max(3.0f, std::round(scale * 2.0f)); }
inline void draw_scroll_bar(QuadRenderer& quads, const PixelRect& list, float first, float shown,
                            float line, float scale) {
    if (shown >= 1.0f) return;
    const float width = scroll_bar_width(scale);
    const PixelRect track{list.x + list.w - width, list.y, width, list.h};
    quads.draw_solid(track, Color{0.10f, 0.12f, 0.16f, 1.0f});
    const float thumb = std::max(line, list.h * shown);
    const float travel = list.h - thumb;
    const float offset = shown < 1.0f ? std::clamp(first / (1.0f - shown), 0.0f, 1.0f) : 0.0f;
    quads.draw_solid(PixelRect{track.x, track.y + travel * offset, track.w, thumb},
                     ui_theme().accent);
}

// How round a tile's corners are -- a layout's, a save point's.
inline float menu_tile_radius(float scale) { return std::round(scale * 6.0f); }

// A tile in the quick menu's grid, which is a different job: the tiles are the
// choice itself rather than lines of a list, so each one is outlined, rounded
// as everything else is, and the cursor rings its own. The layout actually in
// use is washed faintly, so the menu still says which one the game is drawn in
// without competing with the cursor.
inline void draw_menu_tile(QuadRenderer& quads,
                           const PixelRect& rect,
                           bool focused,
                           bool selected,
                           float scale) {
    const UiTheme& theme = ui_theme();
    const float radius = menu_tile_radius(scale);
    if (selected && !focused) {
        Color faint = theme.text;
        faint.a = 0.14f;
        quads.draw_rounded_ring(rect, radius, rect.w + rect.h, faint, faint);
    }
    if (focused) {
        draw_menu_cursor(quads, rect, radius, scale);
        return;
    }
    Color outline = theme.text;
    outline.a = 0.45f;
    quads.draw_rounded_ring(rect, radius, menu_border(scale) * 0.5f, outline, outline);
}

// What text on a list's row is: the same with the cursor on it or not, the
// ring leaving what is under it showing.
inline const Color& menu_row_text(bool focused) {
    (void)focused;
    return ui_theme().text;
}


}  // namespace ds
