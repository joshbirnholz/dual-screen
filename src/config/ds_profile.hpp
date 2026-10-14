#pragma once

#include <array>
#include <cstddef>

namespace ds {

// A DS's favorite color -- the theme its owner picks in the console's own
// settings, by the index melonDS DS's "melonds_firmware_favorite_color" takes
// -- and its name and shade as the console shows them.
struct DsFavoriteColor {
    const char* name;
    float r, g, b;
};

inline constexpr std::array<DsFavoriteColor, 16> kDsFavoriteColors = {{
    {"Gray", 0x61 / 255.0f, 0x82 / 255.0f, 0x9A / 255.0f},
    {"Brown", 0xBA / 255.0f, 0x49 / 255.0f, 0x00 / 255.0f},
    {"Red", 0xFB / 255.0f, 0x00 / 255.0f, 0x18 / 255.0f},
    {"Pink", 0xFB / 255.0f, 0x8A / 255.0f, 0xFB / 255.0f},
    {"Orange", 0xFB / 255.0f, 0x92 / 255.0f, 0x00 / 255.0f},
    {"Yellow", 0xF3 / 255.0f, 0xE3 / 255.0f, 0x00 / 255.0f},
    {"Lime", 0xAA / 255.0f, 0xFB / 255.0f, 0x00 / 255.0f},
    {"Green", 0x00 / 255.0f, 0xFB / 255.0f, 0x00 / 255.0f},
    {"Dark Green", 0x00 / 255.0f, 0xA2 / 255.0f, 0x38 / 255.0f},
    {"Turquoise", 0x49 / 255.0f, 0xDB / 255.0f, 0x8A / 255.0f},
    {"Light Blue", 0x30 / 255.0f, 0xBA / 255.0f, 0xF3 / 255.0f},
    {"Blue", 0x00 / 255.0f, 0x59 / 255.0f, 0xF3 / 255.0f},
    {"Dark Blue", 0x00 / 255.0f, 0x00 / 255.0f, 0x92 / 255.0f},
    {"Purple", 0x8A / 255.0f, 0x00 / 255.0f, 0xD3 / 255.0f},
    {"Violet", 0xD3 / 255.0f, 0x00 / 255.0f, 0xEB / 255.0f},
    {"Magenta", 0xFB / 255.0f, 0x00 / 255.0f, 0x92 / 255.0f},
}};

// A DS's name is at most this many characters.
inline constexpr std::size_t kDsNicknameMax = 10;

}  // namespace ds
