#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace ds {

// The UI font, rasterised from an embedded OpenType file rather than drawn as
// bitmap art: the shell is meant to look like a console's, and a console's
// menus are not set in a 5x7 grid. It is embedded rather than borrowed from the
// system because the app has to render readable errors (missing BIOS, missing
// ROM) on a machine where nothing else is installed, and because the Switch
// homebrew build has no system font to ask for.

// Where one glyph sits in the atlas and how the pen moves across it. Everything
// is in pixels at the size the face was baked, and `left`/`top` are measured
// from the pen position on the baseline, so a proportional font places each
// glyph by its own shape rather than by a cell.
struct Glyph {
    int atlas_x = 0;
    int atlas_y = 0;
    int width = 0;
    int height = 0;
    float left = 0.0f;
    float top = 0.0f;
    float advance = 0.0f;
};

// Every glyph the UI can draw, rasterised at one em size into a single 8-bit
// coverage atlas.
class FontFace {
public:
    const std::vector<uint8_t>& pixels() const { return pixels_; }
    int atlas_width() const { return atlas_width_; }
    int atlas_height() const { return atlas_height_; }
    bool valid() const { return !pixels_.empty(); }

    // Null for a codepoint the font does not cover, which callers draw as
    // nothing rather than as a substitute box.
    const Glyph* glyph(uint32_t codepoint) const;
    // How much closer this pair sits than their advances alone would put them.
    float kerning(uint32_t left, uint32_t right) const;
    // Where the baseline sits below the top of a line of this face.
    float baseline() const { return baseline_; }

    static FontFace bake(float em_pixels);

private:
    std::vector<uint8_t> pixels_;
    int atlas_width_ = 0;
    int atlas_height_ = 0;
    float baseline_ = 0.0f;
    float scale_ = 0.0f;
    std::unordered_map<uint32_t, Glyph> glyphs_;
};

// Baked on first use and kept: a UI asks for a handful of sizes and asks for
// each of them every frame.
const FontFace& font_face(float em_pixels);

// The codepoints of a UTF-8 string. Anything malformed becomes one replacement
// codepoint rather than derailing the rest of the line.
std::vector<uint32_t> decode_utf8(const std::string& text);

}  // namespace ds
