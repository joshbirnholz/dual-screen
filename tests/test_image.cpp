// The console artwork and the code that reads it. The Standard layout puts its
// screens at coordinates measured from that image, so the two have to be checked
// against each other: a re-exported image or a decoder that quietly shifted a
// row would leave the screens sitting slightly off their own wells, which is the
// kind of thing that looks like a rendering bug rather than an asset problem.

#include "test_support.hpp"

#include "layout/layout.hpp"
#include "layout/layout_registry.hpp"
#include "render/embedded_assets.hpp"
#include "render/png_image.hpp"

using namespace ds;

namespace {

DecodedImage decode_bezel() {
    DecodedImage image;
    std::string error;
    CHECK_MSG(decode_png(assets::kDsBezelPng, assets::kDsBezelPng_size, image, &error), error);
    return image;
}

void test_bezel_decodes() {
    const DecodedImage image = decode_bezel();
    CHECK(image.valid());
    CHECK_EQ(image.width, 615);
    CHECK_EQ(image.height, 630);

    // Known pixels, so a decoder that got the filters subtly wrong is caught
    // rather than producing a plausible-looking smear. The corners are the
    // artwork's own background, which is also what the layout paints around it.
    struct Probe {
        int x;
        int y;
        uint32_t rgb;
    };
    const Probe probes[] = {
        {0, 0, 0x252525},      {614, 0, 0x252525},   {0, 629, 0x252525},
        {614, 629, 0x252525},  {300, 5, 0xe6eff2},   {20, 300, 0xeff2f6},
        {600, 300, 0x414242},  {300, 625, 0x505254},
    };
    for (const Probe& probe : probes) {
        const uint32_t got =
            image.pixels[static_cast<std::size_t>(probe.y) * image.width + probe.x] & 0x00ffffffu;
        CHECK_MSG(got == probe.rgb, "pixel (" + std::to_string(probe.x) + "," +
                                        std::to_string(probe.y) + ") decoded as " +
                                        std::to_string(got) + ", expected " +
                                        std::to_string(probe.rgb));
    }
}

// Anything the decoder cannot honestly handle is refused rather than guessed at.
void test_bad_images_are_refused() {
    DecodedImage image;
    std::string error;
    const uint8_t rubbish[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    CHECK(!decode_png(rubbish, sizeof(rubbish), image, &error));
    CHECK(!error.empty());
    CHECK(!decode_png(nullptr, 0, image, &error));

    // A real PNG with its data cut short must fail rather than half-decode.
    std::vector<uint8_t> truncated(assets::kDsBezelPng,
                                   assets::kDsBezelPng + assets::kDsBezelPng_size / 2);
    CHECK(!decode_png(truncated.data(), truncated.size(), image, &error));
}

// The Standard layout's screens land on the artwork's own screens.
void test_standard_screens_sit_in_the_artwork() {
    const DecodedImage image = decode_bezel();
    const LayoutRegistry registry;
    const Layout& standard = *registry.find("standard");

    // Draw the layout at exactly the artwork's size, so a placement's pixels are
    // the image's pixels and the two can be compared directly.
    const PixelRect window{0.0f, 0.0f, static_cast<float>(image.width),
                           static_cast<float>(image.height)};
    Layout unpadded = standard;
    unpadded.frame_margin = 0.0f;  // the padding is around the console, not in it
    const std::vector<PlacedScreen> placed = place_screens(unpadded, window);
    CHECK_EQ(placed.size(), 2u);

    // The overlay that came with the artwork puts the screens here, one image
    // pixel per DS pixel.
    struct Expected {
        ScreenId screen;
        float x, y, w, h;
    };
    const Expected expected[] = {
        {ScreenId::Top, 179, 60, 256, 192},
        {ScreenId::Bottom, 179, 337, 256, 192},
    };
    for (const Expected& want : expected) {
        bool found = false;
        for (const PlacedScreen& screen : placed) {
            if (screen.screen != want.screen) continue;
            found = true;
            const std::string where = std::string(to_string(want.screen)) + " screen";
            CHECK_MSG(std::abs(screen.rect.x - want.x) < 1.0f,
                      where + " x is " + std::to_string(screen.rect.x));
            CHECK_MSG(std::abs(screen.rect.y - want.y) < 1.0f,
                      where + " y is " + std::to_string(screen.rect.y));
            CHECK_MSG(std::abs(screen.rect.w - want.w) < 1.0f,
                      where + " width is " + std::to_string(screen.rect.w));
            CHECK_MSG(std::abs(screen.rect.h - want.h) < 1.0f,
                      where + " height is " + std::to_string(screen.rect.h));

            // And the artwork really is dark there -- a screen well rather than
            // a patch of plastic the placement happened to land on.
            const int cx = static_cast<int>(screen.rect.x + screen.rect.w * 0.5f);
            const int cy = static_cast<int>(screen.rect.y + screen.rect.h * 0.5f);
            const uint32_t pixel =
                image.pixels[static_cast<std::size_t>(cy) * image.width + cx] & 0x00ffffffu;
            const uint32_t red = (pixel >> 16) & 0xffu;
            const uint32_t green = (pixel >> 8) & 0xffu;
            const uint32_t blue = pixel & 0xffu;
            CHECK_MSG(red < 60 && green < 60 && blue < 60,
                      where + " covers plastic rather than a screen well");
        }
        CHECK(found);
    }

    // The padding keeps the console clear of the window's edges.
    const PixelRect padded = layout_frame(standard, 1280.0f, 800.0f);
    CHECK(padded.y > 1.0f);
    CHECK(padded.x > 1.0f);
    CHECK(padded.y + padded.h < 799.0f);
}

}  // namespace

int main() {
    test_bezel_decodes();
    test_bad_images_are_refused();
    test_standard_screens_sit_in_the_artwork();
    return test::finish("image");
}
