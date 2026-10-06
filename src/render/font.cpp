#include "render/font.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>

#include "render/embedded_assets.hpp"
#include "util/log.hpp"

#ifdef __SWITCH__
#include <switch.h>
#endif

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb/stb_truetype.h"

namespace ds {
namespace {

// What the atlas starts with: ASCII, then the Latin-1 letters, so an accented
// name in a ROM filename draws as itself, and the punctuation below. The
// embedded font is subset to little more, which is why it is kilobytes rather
// than megabytes; anything else comes from a fallback font when first drawn.
constexpr uint32_t kAsciiFirst = 32;
constexpr uint32_t kAsciiLast = 126;
constexpr uint32_t kLatinFirst = 160;
constexpr uint32_t kLatinLast = 255;
// And the punctuation names are written with -- a game's name from SteamGridDB
// as much as a ROM's: dashes, curly quotes, an ellipsis, the trademark sign.
constexpr uint32_t kPunctuation[] = {0x2013, 0x2014, 0x2018, 0x2019, 0x201C, 0x201D, 0x2026, 0x2122};

// Sized by its capitals rather than by its em or its ascent and descent,
// which say as much about the room a font leaves for accents -- or for kanji,
// in a Japanese family -- as about how big its letters look. Every menu was
// laid out around capitals kCapHeight of an em tall, sitting on a baseline
// kBaseline of an em down; a font is scaled and set to match, so changing it
// changes how the letters look and nothing else. A fallback is scaled the same
// way, so its Latin letters match the embedded font's and its kanji sit with
// them as they were designed to.
constexpr float kCapHeight = 0.78f;
constexpr float kBaseline = 0.88f;

struct LoadedFont {
    std::vector<uint8_t> owned;  // empty when the bytes are someone else's
    stbtt_fontinfo info{};
    std::string name;
};

bool load_font(const unsigned char* data, std::size_t size, std::vector<uint8_t> owned, const std::string& name,
               std::vector<std::unique_ptr<LoadedFont>>& fonts) {
    auto font = std::make_unique<LoadedFont>();
    font->owned = std::move(owned);
    if (!font->owned.empty()) data = font->owned.data();
    if (data == nullptr || size < 12) return false;
    const int offset = stbtt_GetFontOffsetForIndex(data, 0);
    if (offset < 0 || !stbtt_InitFont(&font->info, data, offset)) return false;
    font->name = name;
    fonts.push_back(std::move(font));
    return true;
}

// A font file from the system, if it is there and draws Japanese, Chinese or
// Korean -- fontconfig answers with its nearest font even when it has none.
void load_font_file(const std::string& path, std::vector<std::unique_ptr<LoadedFont>>& fonts) {
    if (path.empty()) return;
    for (const auto& font : fonts) {
        if (font->name == path) return;
    }
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) return;
    std::vector<uint8_t> bytes;
    if (std::fseek(file, 0, SEEK_END) == 0) {
        const long size = std::ftell(file);
        if (size > 0 && size < (128L << 20) && std::fseek(file, 0, SEEK_SET) == 0) {
            bytes.resize(static_cast<std::size_t>(size));
            if (std::fread(bytes.data(), 1, bytes.size(), file) != bytes.size()) bytes.clear();
        }
    }
    std::fclose(file);
    if (bytes.empty()) return;
    const std::size_t size = bytes.size();
    std::vector<std::unique_ptr<LoadedFont>> candidate;
    if (!load_font(nullptr, size, std::move(bytes), path, candidate)) return;
    const stbtt_fontinfo& info = candidate.front()->info;
    // あ, 中, 한: a Japanese, a Chinese or a Korean font.
    if (stbtt_FindGlyphIndex(&info, 0x3042) == 0 && stbtt_FindGlyphIndex(&info, 0x4E2D) == 0 &&
        stbtt_FindGlyphIndex(&info, 0xD55C) == 0) {
        return;
    }
    fonts.push_back(std::move(candidate.front()));
}

#if !defined(__SWITCH__) && !defined(__APPLE__)
// What fontconfig would draw a language in, when it is installed.
std::string fontconfig_file(const char* language) {
    const std::string command = std::string("fc-match -f '%{file}' ':lang=") + language + "' 2>/dev/null";
    std::FILE* pipe = popen(command.c_str(), "r");
    if (pipe == nullptr) return std::string();
    std::string out;
    char buffer[512];
    while (std::fgets(buffer, sizeof(buffer), pipe) != nullptr) out += buffer;
    pclose(pipe);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
}
#endif

// The embedded font first, then the fallbacks, Japanese before Chinese before
// Korean: a name in kanji is drawn the way it would be in Japan, since most
// games here are Japanese ones.
const std::vector<std::unique_ptr<LoadedFont>>& fonts() {
    static const std::vector<std::unique_ptr<LoadedFont>> loaded = [] {
        std::vector<std::unique_ptr<LoadedFont>> fonts;
        if (!load_font(assets::kUiFont, assets::kUiFont_size, {}, "the embedded font", fonts)) return fonts;
#ifdef __SWITCH__
        // The console's own, in memory already: the same fonts its menus use.
        if (R_SUCCEEDED(plInitialize(PlServiceType_User))) {
            const PlSharedFontType types[] = {PlSharedFontType_Standard, PlSharedFontType_ChineseSimplified,
                                              PlSharedFontType_ExtChineseSimplified,
                                              PlSharedFontType_ChineseTraditional, PlSharedFontType_KO};
            for (const PlSharedFontType type : types) {
                PlFontData data{};
                if (R_SUCCEEDED(plGetSharedFontByType(&data, type))) {
                    load_font(static_cast<const unsigned char*>(data.address), data.size, {},
                              "the console's shared font " + std::to_string(static_cast<int>(type)), fonts);
                }
            }
        }
#else
#ifdef __APPLE__
        for (const char* path : {"/System/Library/Fonts/ヒラギノ角ゴシック W3.ttc",
                                 "/System/Library/Fonts/Hiragino Sans GB.ttc",
                                 "/System/Library/Fonts/AppleSDGothicNeo.ttc",
                                 "/System/Library/Fonts/Supplemental/Arial Unicode.ttf",
                                 "/Library/Fonts/Arial Unicode.ttf"}) {
            load_font_file(path, fonts);
        }
#else
        for (const char* language : {"ja", "zh-cn", "ko"}) load_font_file(fontconfig_file(language), fonts);
        for (const char* path : {"/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
                                 "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
                                 "/usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc",
                                 "/usr/share/fonts/noto-cjk/NotoSansCJKjp-Regular.otf",
                                 "/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf",
                                 "/usr/share/fonts/droid/DroidSansFallbackFull.ttf",
                                 "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc",
                                 "/usr/share/fonts/wenquanyi/wqy-zenhei/wqy-zenhei.ttc",
                                 "/system/fonts/NotoSansCJK-Regular.ttc"}) {
            load_font_file(path, fonts);
        }
#endif
#endif
        if (fonts.size() == 1) {
            log_info("text: no Japanese, Chinese or Korean font found here; names in those are drawn without "
                     "the characters the UI font does not have");
        } else {
            std::string names;
            for (std::size_t i = 1; i < fonts.size(); ++i) names += (i > 1 ? ", " : "") + fonts[i]->name;
            log_info("text: characters the UI font does not have come from " + names);
        }
        return fonts;
    }();
    return loaded;
}

float scale_for(const stbtt_fontinfo& info, float em_pixels) {
    int left = 0;
    int bottom = 0;
    int right = 0;
    int cap_height = 0;
    if (stbtt_FindGlyphIndex(&info, 'H') != 0) stbtt_GetCodepointBox(&info, 'H', &left, &bottom, &right, &cap_height);
    if (cap_height <= 0) {
        int ascent = 0;
        int descent = 0;
        int gap = 0;
        stbtt_GetFontVMetrics(&info, &ascent, &descent, &gap);
        cap_height = static_cast<int>((ascent - descent) * kCapHeight);
    }
    return cap_height > 0 ? em_pixels * kCapHeight / static_cast<float>(cap_height) : 0.0f;
}

}  // namespace

bool FontFace::grow() const {
    // Twice as wide and twice as tall, the glyphs already placed staying where
    // they are: their atlas positions are in pixels, and stay right.
    const int width = atlas_width_ * 2;
    const int height = atlas_height_ * 2;
    if (width > 4096 || height > 4096) return false;
    std::vector<uint8_t> bigger(static_cast<std::size_t>(width) * height, 0);
    for (int y = 0; y < atlas_height_; ++y) {
        std::copy_n(pixels_.data() + static_cast<std::size_t>(y) * atlas_width_, atlas_width_,
                    bigger.data() + static_cast<std::size_t>(y) * width);
    }
    pixels_ = std::move(bigger);
    atlas_width_ = width;
    atlas_height_ = height;
    return true;
}

// Rasterises one glyph into the atlas: left to right in rows, starting the
// next row at the tallest glyph seen so far, and growing the atlas when it is
// full.
bool FontFace::place(int font, int glyph_index, uint32_t codepoint) const {
    const stbtt_fontinfo& info = fonts()[static_cast<std::size_t>(font)]->info;
    if (scales_[static_cast<std::size_t>(font)] == 0.0f) {
        scales_[static_cast<std::size_t>(font)] = scale_for(info, em_pixels_);
    }
    const float scale = scales_[static_cast<std::size_t>(font)];
    int advance = 0;
    int bearing = 0;
    stbtt_GetGlyphHMetrics(&info, glyph_index, &advance, &bearing);
    int x0 = 0;
    int y0 = 0;
    int x1 = 0;
    int y1 = 0;
    stbtt_GetGlyphBitmapBox(&info, glyph_index, scale, scale, &x0, &y0, &x1, &y1);

    Glyph glyph;
    glyph.width = x1 - x0;
    glyph.height = y1 - y0;
    glyph.left = static_cast<float>(x0);
    glyph.top = static_cast<float>(y0);
    glyph.advance = advance * scale;
    glyph.font = font;

    if (glyph.width > 0 && glyph.height > 0) {
        while (true) {
            if (pen_x_ + glyph.width + 1 > atlas_width_) {
                pen_x_ = 1;
                pen_y_ += row_height_ + 1;
                row_height_ = 0;
            }
            if (pen_y_ + glyph.height + 1 <= atlas_height_ && glyph.width + 2 <= atlas_width_) break;
            if (!grow()) return false;
        }
        stbtt_MakeGlyphBitmap(&info, pixels_.data() + pen_y_ * atlas_width_ + pen_x_, glyph.width, glyph.height,
                              atlas_width_, scale, scale, glyph_index);
        glyph.atlas_x = pen_x_;
        glyph.atlas_y = pen_y_;
        pen_x_ += glyph.width + 1;
        row_height_ = std::max(row_height_, glyph.height);
    }
    glyphs_.emplace(codepoint, glyph);
    ++revision_;
    return true;
}

FontFace FontFace::bake(float em_pixels) {
    FontFace face;
    const auto& all = fonts();
    if (all.empty() || em_pixels <= 0.0f) {
        return face;
    }
    face.em_pixels_ = em_pixels;
    face.baseline_ = em_pixels * kBaseline;
    face.scales_.assign(all.size(), 0.0f);
    face.atlas_width_ = 128;
    face.atlas_height_ = 128;
    face.pixels_.assign(static_cast<std::size_t>(face.atlas_width_) * face.atlas_height_, 0);

    // The embedded font's own set, at once: every menu needs it.
    const stbtt_fontinfo& info = all.front()->info;
    const auto place_own = [&](uint32_t codepoint) {
        const int index = stbtt_FindGlyphIndex(&info, static_cast<int>(codepoint));
        return index == 0 || face.place(0, index, codepoint);
    };
    bool placed = true;
    for (uint32_t codepoint = kAsciiFirst; codepoint <= kAsciiLast && placed; ++codepoint) placed = place_own(codepoint);
    for (uint32_t codepoint = kLatinFirst; codepoint <= kLatinLast && placed; ++codepoint) placed = place_own(codepoint);
    for (const uint32_t codepoint : kPunctuation) placed = placed && place_own(codepoint);
    if (!placed) face.pixels_.clear();
    return face;
}

const Glyph* FontFace::glyph(uint32_t codepoint) const {
    const auto found = glyphs_.find(codepoint);
    if (found != glyphs_.end()) return &found->second;
    if (pixels_.empty() || codepoint < 32 || missing_.count(codepoint) != 0) return nullptr;
    // Not in the atlas yet: the first font that has it, drawn now.
    const auto& all = fonts();
    for (std::size_t font = 0; font < all.size(); ++font) {
        const int index = stbtt_FindGlyphIndex(&all[font]->info, static_cast<int>(codepoint));
        if (index == 0) continue;
        if (!place(static_cast<int>(font), index, codepoint)) break;
        return &glyphs_.find(codepoint)->second;
    }
    missing_.emplace(codepoint, true);
    return nullptr;
}

float FontFace::kerning(uint32_t left, uint32_t right) const {
    // Only between two of the embedded font's own glyphs: a fallback's pairs
    // are not worth the lookups, and across two fonts there are none.
    const Glyph* first = glyph(left);
    const Glyph* second = glyph(right);
    if (first == nullptr || second == nullptr || first->font != 0 || second->font != 0 || scales_.empty()) {
        return 0.0f;
    }
    return stbtt_GetCodepointKernAdvance(&fonts().front()->info, static_cast<int>(left), static_cast<int>(right)) *
           scales_.front();
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
