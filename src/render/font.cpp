#include "render/font.hpp"

#include <algorithm>
#include <cmath>
#include <map>

#include "render/embedded_assets.hpp"

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb/stb_truetype.h"

namespace ds {
namespace {

// What the atlas covers: ASCII, then the Latin-1 letters, so an accented name
// in a ROM filename draws as itself. The embedded font is subset to these plus
// a few punctuation marks, which is why it is kilobytes rather than megabytes.
constexpr uint32_t kAsciiFirst = 32;
constexpr uint32_t kAsciiLast = 126;
constexpr uint32_t kLatinFirst = 160;
constexpr uint32_t kLatinLast = 255;

const stbtt_fontinfo* embedded_font() {
    static stbtt_fontinfo info;
    static const bool loaded =
        stbtt_InitFont(&info, assets::kRodinOtf, stbtt_GetFontOffsetForIndex(assets::kRodinOtf, 0));
    return loaded ? &info : nullptr;
}

// Packs glyphs left to right in rows, starting the next row at the tallest
// glyph seen so far. Nothing cleverer is worth it for a couple of hundred
// glyphs that only ever get packed once per size.
bool pack_into(int atlas_size,
               const stbtt_fontinfo& info,
               float scale,
               std::vector<uint8_t>& pixels,
               std::unordered_map<uint32_t, Glyph>& glyphs) {
    pixels.assign(static_cast<std::size_t>(atlas_size) * atlas_size, 0);
    glyphs.clear();

    int pen_x = 1;
    int pen_y = 1;
    int row_height = 0;

    const auto place = [&](uint32_t codepoint) {
        if (stbtt_FindGlyphIndex(&info, static_cast<int>(codepoint)) == 0) {
            return true;
        }
        int advance = 0;
        int bearing = 0;
        stbtt_GetCodepointHMetrics(&info, static_cast<int>(codepoint), &advance, &bearing);

        int x0 = 0;
        int y0 = 0;
        int x1 = 0;
        int y1 = 0;
        stbtt_GetCodepointBitmapBox(&info, static_cast<int>(codepoint), scale, scale, &x0, &y0, &x1,
                                    &y1);

        Glyph glyph;
        glyph.width = x1 - x0;
        glyph.height = y1 - y0;
        glyph.left = static_cast<float>(x0);
        glyph.top = static_cast<float>(y0);
        glyph.advance = advance * scale;

        if (glyph.width > 0 && glyph.height > 0) {
            if (pen_x + glyph.width + 1 > atlas_size) {
                pen_x = 1;
                pen_y += row_height + 1;
                row_height = 0;
            }
            if (pen_y + glyph.height + 1 > atlas_size) {
                return false;
            }
            stbtt_MakeCodepointBitmap(&info, pixels.data() + pen_y * atlas_size + pen_x, glyph.width,
                                      glyph.height, atlas_size, scale, scale,
                                      static_cast<int>(codepoint));
            glyph.atlas_x = pen_x;
            glyph.atlas_y = pen_y;
            pen_x += glyph.width + 1;
            row_height = std::max(row_height, glyph.height);
        }
        glyphs.emplace(codepoint, glyph);
        return true;
    };

    for (uint32_t codepoint = kAsciiFirst; codepoint <= kAsciiLast; ++codepoint) {
        if (!place(codepoint)) return false;
    }
    for (uint32_t codepoint = kLatinFirst; codepoint <= kLatinLast; ++codepoint) {
        if (!place(codepoint)) return false;
    }
    return true;
}

}  // namespace

FontFace FontFace::bake(float em_pixels) {
    FontFace face;
    const stbtt_fontinfo* info = embedded_font();
    if (info == nullptr || em_pixels <= 0.0f) {
        return face;
    }

    // Sized by the em rather than by stbtt_ScaleForPixelHeight, which measures
    // the tallest glyph and so would shrink the text whenever the font happens
    // to carry a tall accented capital.
    int ascent = 0;
    int descent = 0;
    int gap = 0;
    stbtt_GetFontVMetrics(info, &ascent, &descent, &gap);
    const float units_per_em = static_cast<float>(ascent - descent);
    face.scale_ = em_pixels / units_per_em;
    face.baseline_ = ascent * face.scale_;

    for (int size = 128; size <= 4096; size *= 2) {
        if (pack_into(size, *info, face.scale_, face.pixels_, face.glyphs_)) {
            face.atlas_width_ = size;
            face.atlas_height_ = size;
            return face;
        }
    }
    face.pixels_.clear();
    return face;
}

const Glyph* FontFace::glyph(uint32_t codepoint) const {
    const auto found = glyphs_.find(codepoint);
    return found == glyphs_.end() ? nullptr : &found->second;
}

float FontFace::kerning(uint32_t left, uint32_t right) const {
    const stbtt_fontinfo* info = embedded_font();
    if (info == nullptr) {
        return 0.0f;
    }
    return stbtt_GetCodepointKernAdvance(info, static_cast<int>(left), static_cast<int>(right)) *
           scale_;
}

const FontFace& font_face(float em_pixels) {
    // Keyed by whole pixels: a face baked for 23.4px and one for 23.6px would
    // be the same picture and two atlases.
    const int key = std::max(6, static_cast<int>(std::lround(em_pixels)));
    static std::map<int, FontFace> faces;
    const auto found = faces.find(key);
    if (found != faces.end()) {
        return found->second;
    }
    return faces.emplace(key, FontFace::bake(static_cast<float>(key))).first->second;
}

std::vector<uint32_t> decode_utf8(const std::string& text) {
    std::vector<uint32_t> codepoints;
    codepoints.reserve(text.size());
    for (std::size_t index = 0; index < text.size();) {
        const unsigned char lead = static_cast<unsigned char>(text[index]);
        int length = 1;
        uint32_t codepoint = lead;
        if (lead >= 0xF0) {
            length = 4;
            codepoint = lead & 0x07u;
        } else if (lead >= 0xE0) {
            length = 3;
            codepoint = lead & 0x0Fu;
        } else if (lead >= 0xC0) {
            length = 2;
            codepoint = lead & 0x1Fu;
        } else if (lead >= 0x80) {
            codepoints.push_back(0xFFFD);
            ++index;
            continue;
        }
        if (index + length > text.size()) {
            codepoints.push_back(0xFFFD);
            break;
        }
        for (int step = 1; step < length; ++step) {
            const unsigned char continuation = static_cast<unsigned char>(text[index + step]);
            if ((continuation & 0xC0u) != 0x80u) {
                codepoint = 0xFFFD;
                length = step;
                break;
            }
            codepoint = (codepoint << 6) | (continuation & 0x3Fu);
        }
        codepoints.push_back(codepoint);
        index += length;
    }
    return codepoints;
}

}  // namespace ds
