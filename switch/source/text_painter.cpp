#include "text_painter.hpp"

#include <vector>

#include "render/font.hpp"

namespace ds::sw {

TextPainter::~TextPainter() { destroy(); }

bool TextPainter::create(SDL_Renderer* renderer) {
    destroy();
    const FontAtlas& font = builtin_font_atlas();
    if (font.width <= 0 || font.height <= 0) {
        return false;
    }

    // The atlas is 8-bit coverage. It goes to the GPU as white with that
    // coverage as alpha, so a colour is a colour modulation rather than a
    // second atlas per colour.
    std::vector<uint32_t> pixels(static_cast<std::size_t>(font.width) * font.height);
    for (std::size_t index = 0; index < pixels.size(); ++index) {
        pixels[index] = (static_cast<uint32_t>(font.pixels[index]) << 24) | 0x00ffffffu;
    }

    atlas_ = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC,
                               font.width, font.height);
    if (atlas_ == nullptr) {
        return false;
    }
    SDL_UpdateTexture(atlas_, nullptr, pixels.data(), font.width * static_cast<int>(sizeof(uint32_t)));
    SDL_SetTextureBlendMode(atlas_, SDL_BLENDMODE_BLEND);
    return true;
}

void TextPainter::destroy() {
    if (atlas_ != nullptr) {
        SDL_DestroyTexture(atlas_);
        atlas_ = nullptr;
    }
}

void TextPainter::draw(SDL_Renderer* renderer,
                       const std::string& text,
                       float x,
                       float y,
                       int scale,
                       SDL_Color colour) const {
    if (atlas_ == nullptr || scale < 1) {
        return;
    }
    SDL_SetTextureColorMod(atlas_, colour.r, colour.g, colour.b);
    SDL_SetTextureAlphaMod(atlas_, colour.a);

    int pen = static_cast<int>(x);
    for (const char character : text) {
        if (character != ' ') {
            int cell_x = 0;
            int cell_y = 0;
            font_cell_for(character, cell_x, cell_y);
            const SDL_Rect source{cell_x, cell_y, kFontGlyphWidth, kFontGlyphHeight};
            const SDL_Rect destination{pen, static_cast<int>(y), kFontGlyphWidth * scale,
                                       kFontGlyphHeight * scale};
            SDL_RenderCopy(renderer, atlas_, &source, &destination);
        }
        pen += kFontCellWidth * scale;
    }
}

float TextPainter::width_of(const std::string& text, int scale) {
    return static_cast<float>(text.size() * static_cast<std::size_t>(kFontCellWidth * scale));
}

float TextPainter::height_of(int scale) { return static_cast<float>(kFontCellHeight * scale); }

}  // namespace ds::sw
