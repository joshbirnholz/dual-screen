#pragma once

#include "config/ds_profile.hpp"
#include "ui/ui_theme.hpp"

namespace ds {

// The menus' accent made the DS favorite color chosen in Settings -- Light
// Blue where none is -- see set_ui_accent().
inline void apply_favorite_color_accent(int favorite_color) {
    if (favorite_color < 0 || favorite_color >= static_cast<int>(kDsFavoriteColors.size())) {
        favorite_color = kDsDefaultFavoriteColor;
    }
    const DsFavoriteColor& color = kDsFavoriteColors[static_cast<std::size_t>(favorite_color)];
    set_ui_accent(Color{color.r, color.g, color.b, 1.0f});
}

// The same, every frame: the accent made the host's favorite color while this
// device is another's companion -- `host_color`, or -1 when there is none to
// follow -- and this device's own otherwise. Only changes it when it differs.
inline void follow_favorite_color_accent(int own_color, int host_color) {
    int favorite_color = host_color >= 0 ? host_color : own_color;
    if (favorite_color < 0 || favorite_color >= static_cast<int>(kDsFavoriteColors.size())) {
        favorite_color = kDsDefaultFavoriteColor;
    }
    const DsFavoriteColor& color = kDsFavoriteColors[static_cast<std::size_t>(favorite_color)];
    const Color& now = ui_theme().accent;
    if (now.r != color.r || now.g != color.g || now.b != color.b) apply_favorite_color_accent(favorite_color);
}

}  // namespace ds
