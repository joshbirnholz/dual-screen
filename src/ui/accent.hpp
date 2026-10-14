#pragma once

#include "config/ds_profile.hpp"
#include "ui/ui_theme.hpp"

namespace ds {

// The menus' accent made the DS favorite color chosen in Settings, or the
// app's own blue where none is -- see set_ui_accent().
inline void apply_favorite_color_accent(int favorite_color) {
    if (favorite_color >= 0 && favorite_color < static_cast<int>(kDsFavoriteColors.size())) {
        const DsFavoriteColor& color = kDsFavoriteColors[static_cast<std::size_t>(favorite_color)];
        set_ui_accent(Color{color.r, color.g, color.b, 1.0f});
    } else {
        reset_ui_accent();
    }
}

}  // namespace ds
