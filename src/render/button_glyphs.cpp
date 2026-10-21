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
// and every other button as that pad prints it. Positional has no printing
// of its own past the face buttons: a Switch's, but for the shoulders,
// triggers, start and select, a Steam Controller's plain ones.
GlyphCell glyph_cell(PadGlyphStyle style, ButtonGlyph glyph) {
    using C = GlyphCell;
    constexpr int kGlyphs = static_cast<int>(ButtonGlyph::Count);
    // In ButtonGlyph order: A B X Y, the stick clicks, Plus Minus, L R ZL ZR,
    // the d-pad's four and itself, the sticks, Home, Capture.
    static constexpr GlyphCell kNintendo[kGlyphs] = {
        C::LetterA, C::LetterB, C::LetterX, C::LetterY, C::LeftStick, C::RightStick, C::Plus,
        C::Minus, C::SwitchL, C::SwitchR, C::SwitchZL, C::SwitchZR, C::SwitchDpadUp,
        C::SwitchDpadDown, C::SwitchDpadLeft, C::SwitchDpadRight, C::SwitchDpad, C::SwitchStickL,
        C::SwitchStickR, C::SwitchHome, C::SwitchCapture};
    static constexpr GlyphCell kPlayStation[kGlyphs] = {
        C::Circle, C::Cross, C::Triangle, C::Square, C::PlayStationL3, C::PlayStationR3,
        C::PlayStationOptions, C::PlayStationCreate, C::PlayStationL1, C::PlayStationR1,
        C::PlayStationL2, C::PlayStationR2, C::PlayStationDpadUp, C::PlayStationDpadDown,
        C::PlayStationDpadLeft, C::PlayStationDpadRight, C::PlayStationDpad, C::PlayStationStickL,
        // No PS button among Kenney's: Home is a house, as on a Switch.
        C::PlayStationStickR, C::SwitchHome, C::PlayStationMute};
    static constexpr GlyphCell kXbox[kGlyphs] = {
        C::XboxB, C::XboxA, C::XboxY, C::XboxX, C::XboxLS, C::XboxRS, C::XboxMenu, C::XboxView,
        C::XboxLB, C::XboxRB, C::XboxLT, C::XboxRT, C::XboxDpadUp, C::XboxDpadDown,
        C::XboxDpadLeft, C::XboxDpadRight, C::XboxDpad, C::XboxStickL, C::XboxStickR,
        C::XboxGuide, C::XboxShare};
    static constexpr GlyphCell kPositional[kGlyphs] = {
        C::DotsEast, C::DotsSouth, C::DotsNorth, C::DotsWest, C::LeftStick, C::RightStick,
        C::SteamOptions, C::SteamView, C::SteamL1, C::SteamR1, C::SteamL2, C::SteamR2, C::SwitchDpadUp,
        C::SwitchDpadDown, C::SwitchDpadLeft, C::SwitchDpadRight, C::SwitchDpad, C::SwitchStickL,
        C::SwitchStickR, C::SwitchHome, C::SwitchCapture};
    const int index = static_cast<int>(glyph);
    if (index < 0 || index >= kGlyphs) return C::LetterA;
    switch (style) {
        case PadGlyphStyle::PlayStation: return kPlayStation[index];
        case PadGlyphStyle::Xbox: return kXbox[index];
        case PadGlyphStyle::Positional: return kPositional[index];
        default: return kNintendo[index];
    }
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

    // Square cells, a row of them across the sheet.
    cell_width_ = image.width / kGlyphSheetColumns;
    cell_height_ = cell_width_;
    sheet_width_ = image.width;
    sheet_height_ = image.height;
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

float ButtonGlyphSheet::button_centre() const { return 0.5f; }

void ButtonGlyphSheet::draw(QuadRenderer& quads,
                            ButtonGlyph glyph,
                            float x,
                            float y,
                            float height,
                            const Color& color) const {
    if (!texture_.valid() || glyph == ButtonGlyph::Count) {
        return;
    }
    const int cell = static_cast<int>(glyph_cell(style_, glyph));
    const float left = static_cast<float>((cell % kGlyphSheetColumns) * cell_width_);
    const float top = static_cast<float>((cell / kGlyphSheetColumns) * cell_height_);
    const float width = static_cast<float>(sheet_width_);
    const float tall = static_cast<float>(sheet_height_);
    quads.draw_alpha_mask(PixelRect{x, y, width_for(height), height}, texture_, color, left / width,
                          top / tall, (left + static_cast<float>(cell_width_)) / width,
                          (top + static_cast<float>(cell_height_)) / tall);
}

}  // namespace ds
