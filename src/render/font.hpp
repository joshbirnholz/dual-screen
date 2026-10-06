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
    // Which font drew it: 0 the embedded one, else a fallback (see below).
    int font = 0;
};

// Every glyph the UI can draw, rasterised at one em size into a single 8-bit
// coverage atlas. The embedded font's Latin set is baked at once; anything it
// does not have -- a Japanese game's name, Chinese, Korean -- is drawn from a
// fallback font the first time it is asked for and added to the atlas, which
// grows to fit. The fallbacks are the system's own: the console's shared fonts
// on a Switch, Hiragino on a Mac, whatever fontconfig offers for Japanese on
// Linux (Noto Sans CJK, Droid Sans Fallback and the like). A codepoint none of
// them has is drawn as nothing.
class FontFace {
public:
    const std::vector<uint8_t>& pixels() const { return pixels_; }
    int atlas_width() const { return atlas_width_; }
    int atlas_height() const { return atlas_height_; }
    bool valid() const { return !pixels_.empty(); }
    // Goes up each time a glyph is added, so an uploaded copy of the atlas
    // knows to upload it again.
    int revision() const { return revision_; }

    // Null for a codepoint no font covers. May add the glyph to the atlas.
    const Glyph* glyph(uint32_t codepoint) const;
    // How much closer this pair sits than their advances alone would put them.
    float kerning(uint32_t left, uint32_t right) const;
    // Where the baseline sits below the top of a line of this face.
    float baseline() const { return baseline_; }

    static FontFace bake(float em_pixels);

private:
    bool place(int font, int glyph_index, uint32_t codepoint) const;
    bool grow() const;

    // Filled in as text needs it, from behind a const face: drawing a string
    // is what discovers which glyphs it wants.
    mutable std::vector<uint8_t> pixels_;
    mutable int atlas_width_ = 0;
    mutable int atlas_height_ = 0;
    mutable int pen_x_ = 1;
    mutable int pen_y_ = 1;
    mutable int row_height_ = 0;
    mutable int revision_ = 0;
    mutable std::unordered_map<uint32_t, Glyph> glyphs_;
    mutable std::unordered_map<uint32_t, bool> missing_;
    mutable std::vector<float> scales_;  // per font, 0 until first used
    float em_pixels_ = 0.0f;
    float baseline_ = 0.0f;
};

// Baked on first use and kept: a UI asks for a handful of sizes and asks for
// each of them every frame.
const FontFace& font_face(float em_pixels);

// The codepoints of a UTF-8 string. Anything malformed becomes one replacement
// codepoint rather than derailing the rest of the line.
std::vector<uint32_t> decode_utf8(const std::string& text);

}  // namespace ds
