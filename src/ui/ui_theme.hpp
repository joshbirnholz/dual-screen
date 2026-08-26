#pragma once

#include <algorithm>
#include <cmath>

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

inline const UiTheme& ui_theme() {
    static const UiTheme theme;
    return theme;
}

// One selectable row, tile or button, in the language every menu shares: an
// outline that whatever is behind the menu shows through, filled solid only
// where the cursor is sitting. Two pixels at the size the design was drawn at,
// scaled with everything else so a 4K panel does not get a hairline.
inline void draw_menu_row(QuadRenderer& quads, const PixelRect& rect, bool focused, float scale) {
    const UiTheme& theme = ui_theme();
    if (focused) {
        quads.draw_solid(rect, theme.text);
    } else {
        quads.draw_outline(rect, theme.text, std::max(2.0f, std::round(scale * 0.6f)));
    }
}

// What text on such a row has to be, which flips with the fill under it.
inline const Color& menu_row_text(bool focused) {
    const UiTheme& theme = ui_theme();
    return focused ? theme.background : theme.text;
}

}  // namespace ds
