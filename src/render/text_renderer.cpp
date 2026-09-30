#include "render/text_renderer.hpp"

#include <cmath>
#include <sstream>
#include <vector>

#include "render/font.hpp"

namespace ds {
namespace {

// How much em the font is given per scale step. A line box is kTextLineUnit
// tall, and setting the em to exactly that would leave the letters visibly
// smaller than the bitmap font they replace, whose glyphs filled almost the
// whole cell. This is the em that matches the old text's weight; the extra
// sits in the descender, which the space between rows absorbs.
constexpr float kEmPerLineUnit = 1.125f;

float em_pixels_for(float scale) { return kTextLineUnit * kEmPerLineUnit * scale; }

// The top of the em box inside a line box, so text sits centred in the space
// every screen already reserved for it.
float em_offset_for(float scale) {
    return (TextRenderer::line_height(scale) - em_pixels_for(scale)) * 0.5f;
}

float advance_of(const FontFace& face, const std::vector<uint32_t>& codepoints) {
    float width = 0.0f;
    for (std::size_t index = 0; index < codepoints.size(); ++index) {
        const Glyph* glyph = face.glyph(codepoints[index]);
        if (glyph == nullptr) continue;
        width += glyph->advance;
        if (index + 1 < codepoints.size()) {
            width += face.kerning(codepoints[index], codepoints[index + 1]);
        }
    }
    return width;
}

}  // namespace

bool TextRenderer::initialize(std::string* error) {
    const FontFace& face = font_face(em_pixels_for(1.0f));
    if (!face.valid()) {
        if (error != nullptr) *error = "the embedded font could not be rasterised";
        return false;
    }
    default_atlas_width_ = face.atlas_width();
    default_atlas_height_ = face.atlas_height();
    return true;
}

void TextRenderer::shutdown() {
    for (auto& entry : atlases_) {
        entry.second.destroy();
    }
    atlases_.clear();
}

const Texture* TextRenderer::atlas_for(float scale) {
    const int key = static_cast<int>(std::lround(em_pixels_for(scale)));
    const auto found = atlases_.find(key);
    if (found != atlases_.end()) {
        return found->second.valid() ? &found->second : nullptr;
    }

    const FontFace& face = font_face(em_pixels_for(scale));
    Texture& texture = atlases_[key];
    if (face.valid()) {
        texture.upload_alpha8(face.pixels().data(), face.atlas_width(), face.atlas_height());
    }
    return texture.valid() ? &texture : nullptr;
}

float TextRenderer::measure_width(const std::string& text, float scale) {
    if (text.empty() || scale <= 0.0f) {
        return 0.0f;
    }
    return advance_of(font_face(em_pixels_for(scale)), decode_utf8(text));
}

float TextRenderer::line_height(float scale) { return kTextLineUnit * scale; }

float TextRenderer::wrapped_line_step(float scale) { return line_height(scale) * 1.25f; }

void TextRenderer::draw(QuadRenderer& quads,
                        const std::string& text,
                        float x,
                        float y,
                        float scale,
                        const Color& color,
                        TextAlign align) {
    if (text.empty() || scale <= 0.0f) {
        return;
    }
    const Texture* atlas = atlas_for(scale);
    if (atlas == nullptr) {
        return;
    }
    const FontFace& face = font_face(em_pixels_for(scale));
    const std::vector<uint32_t> codepoints = decode_utf8(text);

    float pen_x = x;
    if (align == TextAlign::Center) {
        pen_x = x - advance_of(face, codepoints) * 0.5f;
    } else if (align == TextAlign::Right) {
        pen_x = x - advance_of(face, codepoints);
    }
    const float baseline = y + em_offset_for(scale) + face.baseline();

    const float atlas_width = static_cast<float>(atlas->width());
    const float atlas_height = static_cast<float>(atlas->height());

    for (std::size_t index = 0; index < codepoints.size(); ++index) {
        const Glyph* glyph = face.glyph(codepoints[index]);
        if (glyph == nullptr) continue;
        if (glyph->width > 0 && glyph->height > 0) {
            // Snapped to whole pixels: a glyph rasterised for this size and
            // then landed on a half pixel is blurred by the sampler for no
            // gain, since the next frame draws it in the same place anyway.
            const PixelRect rect{std::floor(pen_x + glyph->left + 0.5f),
                                 std::floor(baseline + glyph->top + 0.5f),
                                 static_cast<float>(glyph->width),
                                 static_cast<float>(glyph->height)};
            const float u0 = static_cast<float>(glyph->atlas_x) / atlas_width;
            const float v0 = static_cast<float>(glyph->atlas_y) / atlas_height;
            const float u1 = static_cast<float>(glyph->atlas_x + glyph->width) / atlas_width;
            const float v1 = static_cast<float>(glyph->atlas_y + glyph->height) / atlas_height;
            quads.draw_alpha_mask(rect, *atlas, color, u0, v0, u1, v1);
        }
        pen_x += glyph->advance;
        if (index + 1 < codepoints.size()) {
            pen_x += face.kerning(codepoints[index], codepoints[index + 1]);
        }
    }
}

std::string TextRenderer::elide_left(const std::string& text, float max_width, float scale) {
    if (measure_width(text, scale) <= max_width) {
        return text;
    }
    const std::string marker = "...";
    const float marker_width = measure_width(marker, scale);
    if (marker_width > max_width) {
        return std::string();
    }
    // Trimmed a character at a time from the left, keeping the tail: paths are
    // the common case and their last components are the informative part.
    std::size_t start = 0;
    while (start < text.size()) {
        // Never cut a UTF-8 sequence in half.
        do {
            ++start;
        } while (start < text.size() && (static_cast<unsigned char>(text[start]) & 0xC0u) == 0x80u);
        if (marker_width + measure_width(text.substr(start), scale) <= max_width) {
            return marker + text.substr(start);
        }
    }
    return marker;
}

std::string TextRenderer::elide_right(const std::string& text, float max_width, float scale) {
    if (measure_width(text, scale) <= max_width) return text;
    std::string cut = text;
    while (!cut.empty() && measure_width(cut + "...", scale) > max_width) {
        cut.pop_back();
        // Never half a character.
        while (!cut.empty() && (static_cast<unsigned char>(cut.back()) & 0xC0) == 0x80) cut.pop_back();
        if (!cut.empty() && (static_cast<unsigned char>(cut.back()) & 0xC0) == 0xC0) cut.pop_back();
    }
    while (!cut.empty() && cut.back() == ' ') cut.pop_back();
    return cut + "...";
}

std::vector<std::string> TextRenderer::wrap(const std::string& text, float max_width, float scale) {
    std::vector<std::string> lines;
    if (max_width <= 0.0f || scale <= 0.0f) {
        lines.push_back(text);
        return lines;
    }

    const auto fits = [&](const std::string& candidate) {
        return measure_width(candidate, scale) <= max_width;
    };

    std::istringstream paragraphs(text);
    std::string paragraph;
    while (std::getline(paragraphs, paragraph)) {
        std::istringstream words(paragraph);
        std::string word;
        std::string current;
        while (words >> word) {
            // A single word wider than the line gets hard-split rather than
            // running off the edge -- paths in error messages do this a lot.
            while (!fits(word)) {
                std::string head;
                for (std::size_t index = 0; index < word.size(); ++index) {
                    if (!fits(head + word[index]) && !head.empty()) break;
                    head += word[index];
                }
                if (head.size() == word.size()) break;
                if (!current.empty()) {
                    lines.push_back(current);
                    current.clear();
                }
                lines.push_back(head);
                word = word.substr(head.size());
            }
            if (current.empty()) {
                current = word;
            } else if (fits(current + " " + word)) {
                current += " " + word;
            } else {
                lines.push_back(current);
                current = word;
            }
        }
        lines.push_back(current);
    }
    return lines;
}

float TextRenderer::draw_wrapped(QuadRenderer& quads,
                                 const std::string& text,
                                 float x,
                                 float y,
                                 float max_width,
                                 float scale,
                                 const Color& color,
                                 TextAlign align) {
    const std::vector<std::string> lines = wrap(text, max_width, scale);
    const float step = wrapped_line_step(scale);
    float pen_y = y;
    for (const std::string& line : lines) {
        draw(quads, line, x, pen_y, scale, color, align);
        pen_y += step;
    }
    return pen_y - y;
}

}  // namespace ds
