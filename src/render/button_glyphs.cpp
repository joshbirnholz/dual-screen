#include "render/button_glyphs.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

#include "render/embedded_assets.hpp"
#include "render/png_image.hpp"

namespace ds {

const char* to_string(PadGlyphStyle style) {
    switch (style) {
        case PadGlyphStyle::Nintendo: return "Nintendo";
        case PadGlyphStyle::PlayStation: return "PlayStation";
        case PadGlyphStyle::Xbox: return "Xbox";
        case PadGlyphStyle::Positional: return "Positional";
        case PadGlyphStyle::Count: break;
    }
    return "Nintendo";
}

bool pad_glyph_style_from_string(const std::string& text, PadGlyphStyle& out) {
    for (int index = 0; index < static_cast<int>(PadGlyphStyle::Count); ++index) {
        const PadGlyphStyle style = static_cast<PadGlyphStyle>(index);
        if (text == to_string(style)) {
            out = style;
            return true;
        }
    }
    return false;
}

// The same four positions, printed three ways.
//
//              east        south       north       west
//   Nintendo   A           B           X           Y
//   Xbox       B           A           Y           X
//   Sony       circle      cross       triangle    square
//   Positional four dots, the one in that corner filled in
//
// Xbox needs no new letters, only the two middle columns swapped over: a
// Nintendo pad and an Xbox pad print the same four letters in different
// corners, which is exactly the confusion this setting exists to end.
GlyphCell glyph_cell(PadGlyphStyle style, ButtonGlyph glyph) {
    switch (glyph) {
        case ButtonGlyph::LeftStick: return GlyphCell::LeftStick;
        case ButtonGlyph::RightStick: return GlyphCell::RightStick;
        case ButtonGlyph::A:  // east
            switch (style) {
                case PadGlyphStyle::PlayStation: return GlyphCell::Circle;
                case PadGlyphStyle::Xbox: return GlyphCell::LetterB;
                case PadGlyphStyle::Positional: return GlyphCell::DotsEast;
                default: return GlyphCell::LetterA;
            }
        case ButtonGlyph::B:  // south
            switch (style) {
                case PadGlyphStyle::PlayStation: return GlyphCell::Cross;
                case PadGlyphStyle::Xbox: return GlyphCell::LetterA;
                case PadGlyphStyle::Positional: return GlyphCell::DotsSouth;
                default: return GlyphCell::LetterB;
            }
        case ButtonGlyph::X:  // north
            switch (style) {
                case PadGlyphStyle::PlayStation: return GlyphCell::Triangle;
                case PadGlyphStyle::Xbox: return GlyphCell::LetterY;
                case PadGlyphStyle::Positional: return GlyphCell::DotsNorth;
                default: return GlyphCell::LetterX;
            }
        case ButtonGlyph::Y:  // west
            switch (style) {
                case PadGlyphStyle::PlayStation: return GlyphCell::Square;
                case PadGlyphStyle::Xbox: return GlyphCell::LetterX;
                case PadGlyphStyle::Positional: return GlyphCell::DotsWest;
                default: return GlyphCell::LetterY;
            }
        case ButtonGlyph::Plus:  // start
            switch (style) {
                case PadGlyphStyle::PlayStation:
                case PadGlyphStyle::Xbox: return GlyphCell::Bars;
                default: return GlyphCell::Plus;
            }
        case ButtonGlyph::Minus:  // back
            switch (style) {
                case PadGlyphStyle::PlayStation: return GlyphCell::Create;
                case PadGlyphStyle::Xbox: return GlyphCell::View;
                default: return GlyphCell::Minus;
            }
        case ButtonGlyph::L:
        case ButtonGlyph::R: {
            const bool right = glyph == ButtonGlyph::R;
            switch (style) {
                case PadGlyphStyle::Nintendo: return right ? GlyphCell::SwitchR : GlyphCell::SwitchL;
                case PadGlyphStyle::Xbox: return right ? GlyphCell::XboxRB : GlyphCell::XboxLB;
                default: return right ? GlyphCell::SteamR1 : GlyphCell::SteamL1;
            }
        }
        case ButtonGlyph::Count: break;
    }
    return GlyphCell::LetterA;
}

namespace {

// The swap symbol, drawn here rather than shipped as artwork: two strokes
// with round ends -- an arrow up on the left, an arrow down on the right --
// as coverage, smoothed by sampling each pixel sixteen times.
std::vector<uint8_t> swap_arrows_coverage(int size) {
    struct Stroke {
        float x0, y0, x1, y1;
    };
    const float up_x = 0.36f;
    const float down_x = 0.64f;
    const float top = 0.25f;
    const float bottom = 0.75f;
    const float head = 0.13f;
    const Stroke strokes[] = {
        {up_x, bottom, up_x, top},
        {up_x, top, up_x - head, top + head},
        {up_x, top, up_x + head, top + head},
        {down_x, top, down_x, bottom},
        {down_x, bottom, down_x - head, bottom - head},
        {down_x, bottom, down_x + head, bottom - head},
    };
    const float half_width = 0.035f;
    std::vector<uint8_t> coverage(static_cast<std::size_t>(size) * size, 0);
    constexpr int kSamples = 4;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            int inside = 0;
            for (int sy = 0; sy < kSamples; ++sy) {
                for (int sx = 0; sx < kSamples; ++sx) {
                    const float px = (x + (sx + 0.5f) / kSamples) / size;
                    const float py = (y + (sy + 0.5f) / kSamples) / size;
                    for (const Stroke& stroke : strokes) {
                        const float dx = stroke.x1 - stroke.x0;
                        const float dy = stroke.y1 - stroke.y0;
                        const float t = std::clamp(((px - stroke.x0) * dx + (py - stroke.y0) * dy) /
                                                       (dx * dx + dy * dy),
                                                   0.0f, 1.0f);
                        const float ex = px - (stroke.x0 + t * dx);
                        const float ey = py - (stroke.y0 + t * dy);
                        if (ex * ex + ey * ey <= half_width * half_width) {
                            ++inside;
                            break;
                        }
                    }
                }
            }
            coverage[static_cast<std::size_t>(y) * size + x] =
                static_cast<uint8_t>(inside * 255 / (kSamples * kSamples));
        }
    }
    return coverage;
}

}  // namespace

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

    // The stylus hand, the same way. Only a missing symbol on one button if
    // it cannot be read, so it does not fail the prompts with it.
    DecodedImage hand;
    std::string hand_error;
    if (decode_png(assets::kStylusHandPng, assets::kStylusHandPng_size, hand, &hand_error)) {
        std::vector<uint8_t> hand_coverage(hand.pixels.size());
        for (std::size_t index = 0; index < hand.pixels.size(); ++index) {
            hand_coverage[index] = static_cast<uint8_t>(hand.pixels[index] >> 24);
        }
        stylus_hand_.upload_alpha8(hand_coverage.data(), hand.width, hand.height);
        stylus_hand_.set_linear_filtering(true);
    }
    constexpr int kSwapSize = 128;
    const std::vector<uint8_t> arrows = swap_arrows_coverage(kSwapSize);
    swap_screens_.upload_alpha8(arrows.data(), kSwapSize, kSwapSize);
    swap_screens_.set_linear_filtering(true);

    cell_width_ = image.width / static_cast<int>(GlyphCell::Count);
    cell_height_ = image.height;
    texture_.upload_alpha8(coverage.data(), image.width, image.height);
    texture_.set_linear_filtering(true);
    if (!texture_.valid()) {
        if (error != nullptr) *error = "could not upload the button artwork";
        return false;
    }
    return true;
}

void ButtonGlyphSheet::draw_stylus_hand(QuadRenderer& quads, const PixelRect& rect,
                                        const Color& color, bool left_handed) const {
    if (!stylus_hand_.valid()) return;
    // As captured it is a left hand -- the Wii U shows it on its left-handed
    // layouts -- so a right hand reads the texture right to left.
    quads.draw_alpha_mask(rect, stylus_hand_, color, left_handed ? 0.0f : 1.0f, 0.0f,
                          left_handed ? 1.0f : 0.0f, 1.0f);
}

void ButtonGlyphSheet::draw_swap_screens(QuadRenderer& quads, const PixelRect& rect,
                                         const Color& color) const {
    if (swap_screens_.valid()) quads.draw_alpha_mask(rect, swap_screens_, color);
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
    if (by_letter_ && (style_ == PadGlyphStyle::Xbox || style_ == PadGlyphStyle::PlayStation)) {
        // The place a DS's A is in holds Steam's A on such a pad: drawn as
        // Xbox's A or PlayStation's cross, which sit south on their own pads.
        switch (glyph) {
            case ButtonGlyph::A: glyph = ButtonGlyph::B; break;
            case ButtonGlyph::B: glyph = ButtonGlyph::A; break;
            case ButtonGlyph::X: glyph = ButtonGlyph::Y; break;
            case ButtonGlyph::Y: glyph = ButtonGlyph::X; break;
            default: break;
        }
    }
    const int cell = static_cast<int>(glyph_cell(style_, glyph));
    const float cells = static_cast<float>(static_cast<int>(GlyphCell::Count));
    const float u0 = static_cast<float>(cell) / cells;
    const float u1 = static_cast<float>(cell + 1) / cells;
    quads.draw_alpha_mask(PixelRect{x, y, width_for(height), height}, texture_, color, u0, 0.0f,
                          u1, 1.0f);
}

}  // namespace ds
