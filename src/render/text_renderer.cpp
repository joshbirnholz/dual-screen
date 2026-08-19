#include "render/text_renderer.hpp"

#include <sstream>
#include <vector>

#include "render/font.hpp"

namespace ds {

bool TextRenderer::initialize(std::string* error) {
    const FontAtlas& atlas = builtin_font_atlas();
    if (atlas.pixels.empty()) {
        if (error != nullptr) *error = "the built-in font atlas is empty";
        return false;
    }
    atlas_.upload_alpha8(atlas.pixels.data(), atlas.width, atlas.height);
    if (!atlas_.valid()) {
        if (error != nullptr) *error = "could not upload the font atlas";
        return false;
    }
    return true;
}

void TextRenderer::shutdown() { atlas_.destroy(); }

float TextRenderer::measure_width(const std::string& text, float scale) {
    return static_cast<float>(text.size()) * kFontCellWidth * scale;
}

float TextRenderer::line_height(float scale) { return kFontCellHeight * scale; }

void TextRenderer::draw(QuadRenderer& quads,
                        const std::string& text,
                        float x,
                        float y,
                        float scale,
                        const Color& color,
                        TextAlign align) {
    if (text.empty() || !atlas_.valid() || scale <= 0.0f) {
        return;
    }

    const float width = measure_width(text, scale);
    float pen_x = x;
    if (align == TextAlign::Center) {
        pen_x = x - width * 0.5f;
    } else if (align == TextAlign::Right) {
        pen_x = x - width;
    }

    const float atlas_width = static_cast<float>(atlas_.width());
    const float atlas_height = static_cast<float>(atlas_.height());
    const float advance = kFontCellWidth * scale;

    for (const char character : text) {
        if (character != ' ') {
            int cell_x = 0;
            int cell_y = 0;
            font_cell_for(character, cell_x, cell_y);

            const float u0 = static_cast<float>(cell_x) / atlas_width;
            const float v0 = static_cast<float>(cell_y) / atlas_height;
            const float u1 = static_cast<float>(cell_x + kFontGlyphWidth) / atlas_width;
            const float v1 = static_cast<float>(cell_y + kFontGlyphHeight) / atlas_height;

            const PixelRect rect{pen_x, y, kFontGlyphWidth * scale, kFontGlyphHeight * scale};
            quads.draw_alpha_mask(rect, atlas_, color, u0, v0, u1, v1);
        }
        pen_x += advance;
    }
}

std::string TextRenderer::elide_left(const std::string& text, float max_width, float scale) {
    const float advance = kFontCellWidth * scale;
    if (advance <= 0.0f) {
        return text;
    }
    const std::size_t max_characters = static_cast<std::size_t>(max_width / advance);
    if (text.size() <= max_characters) {
        return text;
    }
    if (max_characters <= 3) {
        return std::string(max_characters, '.');
    }
    return "..." + text.substr(text.size() - (max_characters - 3));
}

std::vector<std::string> TextRenderer::wrap(const std::string& text, float max_width, float scale) {
    std::vector<std::string> lines;
    const float advance = kFontCellWidth * scale;
    const std::size_t max_characters =
        advance > 0.0f ? static_cast<std::size_t>(max_width / advance) : 0;
    if (max_characters == 0) {
        lines.push_back(text);
        return lines;
    }

    std::istringstream paragraphs(text);
    std::string paragraph;
    while (std::getline(paragraphs, paragraph)) {
        std::istringstream words(paragraph);
        std::string word;
        std::string current;
        while (words >> word) {
            // A single word longer than the line gets hard-split rather than
            // running off the edge — paths in error messages do this a lot.
            while (word.size() > max_characters) {
                if (!current.empty()) {
                    lines.push_back(current);
                    current.clear();
                }
                lines.push_back(word.substr(0, max_characters));
                word = word.substr(max_characters);
            }
            if (current.empty()) {
                current = word;
            } else if (current.size() + 1 + word.size() <= max_characters) {
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
    const float step = line_height(scale);
    float pen_y = y;
    for (const std::string& line : lines) {
        draw(quads, line, x, pen_y, scale, color, align);
        pen_y += step;
    }
    return pen_y - y;
}

}  // namespace ds
