#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>
#include <vector>

#include "layout/layout.hpp"

namespace ds {

// The render scale chosen for a game: how many pixels the core draws per
// console pixel, or Auto -- see auto_render_scale().
inline constexpr int kAutoRenderScale = 0;

// One screen as it is drawn somewhere -- on this device, or on the one
// connected to it -- in device pixels, in the screen's own unturned axes.
struct DrawnScreen {
    ScreenId screen = ScreenId::Top;
    int width = 0;
    int height = 0;
};

// Auto: the least scale at which no screen is drawn larger than the core
// draws it, wherever it is drawn. A screen a hair bigger than a whole scale
// -- a tenth or less -- is not worth the next one up: the step costs as much
// as any, and shows nothing.
inline int auto_render_scale(const SystemScreens& native, const std::vector<DrawnScreen>& drawn,
                             int maximum) {
    float ratio = 1.0f;
    for (const DrawnScreen& screen : drawn) {
        const ScreenSize& size = native.size_of(screen.screen);
        if (!size.valid() || screen.width <= 0 || screen.height <= 0) continue;
        ratio = std::max({ratio, static_cast<float>(screen.width) / static_cast<float>(size.width),
                          static_cast<float>(screen.height) / static_cast<float>(size.height)});
    }
    const int scale = static_cast<int>(std::ceil(ratio - 0.1f));
    return std::clamp(scale, 1, std::max(1, maximum));
}

// The largest scale a core's resolution option offers, from its values --
// "1x (Native 400x240)", "10x (4000x2400)", or a bare "10" -- or 0 when none
// says one.
inline int largest_offered_scale(const std::vector<std::string>& values) {
    int largest = 0;
    for (const std::string& value : values) {
        std::size_t at = 0;
        while (at < value.size() && std::isspace(static_cast<unsigned char>(value[at]))) ++at;
        int number = 0;
        std::size_t digits = 0;
        while (at < value.size() && std::isdigit(static_cast<unsigned char>(value[at])) && digits < 3) {
            number = number * 10 + (value[at] - '0');
            ++at;
            ++digits;
        }
        if (digits > 0) largest = std::max(largest, number);
    }
    return largest;
}

}  // namespace ds
