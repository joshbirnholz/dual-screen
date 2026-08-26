#include "render/button_glyphs.hpp"

#include <vector>

#include "render/embedded_assets.hpp"
#include "render/png_image.hpp"

namespace ds {

bool ButtonGlyphSheet::initialize(std::string* error) {
    DecodedImage image;
    if (!decode_png(assets::kButtonIconsPng, assets::kButtonIconsPng_size, image, error)) {
        return false;
    }

    // Only the alpha matters: the artwork is black throughout, and drawing it
    // as coverage lets the same sheet be tinted for whatever it sits on.
    std::vector<uint8_t> coverage(image.pixels.size());
    for (std::size_t index = 0; index < image.pixels.size(); ++index) {
        coverage[index] = static_cast<uint8_t>(image.pixels[index] >> 24);
    }

    cell_width_ = image.width / static_cast<int>(ButtonGlyph::Count);
    cell_height_ = image.height;
    texture_.upload_alpha8(coverage.data(), image.width, image.height);
    texture_.set_linear_filtering(true);
    if (!texture_.valid()) {
        if (error != nullptr) *error = "could not upload the button artwork";
        return false;
    }
    return true;
}

float ButtonGlyphSheet::width_for(float height) const {
    if (cell_height_ <= 0) {
        return 0.0f;
    }
    return height * static_cast<float>(cell_width_) / static_cast<float>(cell_height_);
}

float ButtonGlyphSheet::button_centre() const {
    if (cell_height_ <= 0) {
        return 0.5f;
    }
    return (static_cast<float>(cell_height_) - static_cast<float>(cell_width_) * 0.5f) /
           static_cast<float>(cell_height_);
}

void ButtonGlyphSheet::draw(QuadRenderer& quads,
                            ButtonGlyph glyph,
                            float x,
                            float y,
                            float height,
                            const Color& color) const {
    if (!texture_.valid() || glyph == ButtonGlyph::Count) {
        return;
    }
    const float cells = static_cast<float>(static_cast<int>(ButtonGlyph::Count));
    const float u0 = static_cast<float>(static_cast<int>(glyph)) / cells;
    const float u1 = static_cast<float>(static_cast<int>(glyph) + 1) / cells;
    quads.draw_alpha_mask(PixelRect{x, y, width_for(height), height}, texture_, color, u0, 0.0f,
                          u1, 1.0f);
}

}  // namespace ds
