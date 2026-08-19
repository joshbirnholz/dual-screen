#pragma once

#include <cstdint>
#include <vector>

namespace ds {

// A tiny built-in 5x7 bitmap font, laid out on 6x8 cells so glyphs have one
// column and one row of natural spacing. It is embedded rather than loaded from
// a system font because the app has to render readable errors (missing BIOS,
// missing ROM) even on a machine where nothing else is installed — and because
// the roadmap's Switch homebrew target has no system font to borrow.
constexpr int kFontGlyphWidth = 5;
constexpr int kFontGlyphHeight = 7;
constexpr int kFontCellWidth = 6;
constexpr int kFontCellHeight = 8;
constexpr int kFontFirstChar = 32;   // space
constexpr int kFontCharCount = 95;   // through '~'
constexpr int kFontAtlasColumns = 16;

// 8-bit coverage atlas: 0 = transparent, 255 = opaque.
struct FontAtlas {
    std::vector<uint8_t> pixels;
    int width = 0;
    int height = 0;
    int columns = 0;
    int rows = 0;
};

// Built once, on first use.
const FontAtlas& builtin_font_atlas();

// Where a character's cell sits in the atlas, in atlas pixels. Characters
// outside the covered range fall back to '?'.
void font_cell_for(char character, int& out_x, int& out_y);

}  // namespace ds
