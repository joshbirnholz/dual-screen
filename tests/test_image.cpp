// The console artwork and the code that reads it. The Standard layout puts its
// screens at coordinates measured from that image, so the two have to be checked
// against each other: a re-exported image or a decoder that quietly shifted a
// row would leave the screens sitting slightly off their own wells, which is the
// kind of thing that looks like a rendering bug rather than an asset problem.

#include "test_support.hpp"

#include "layout/layout.hpp"
#include "core/system.hpp"
#include "layout/layout_registry.hpp"
#include "render/embedded_assets.hpp"
#include "render/png_image.hpp"

using namespace ds;

namespace {

const SystemScreens& ds_screens() { return system_profile(SystemId::Ds).screens; }

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
    SystemScreens unpadded = ds_screens();
    unpadded.console.margin = 0.0f;  // the padding is around the console, not in it
    const std::vector<PlacedScreen> placed = place_screens(standard, window, unpadded);
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

    // The padding keeps the console clear of the window's edges, on a display
    // big enough for the artwork to be scaled up at all.
    const PixelRect padded = layout_frame(standard, 1920.0f, 1200.0f, ds_screens());
    CHECK(padded.y > 1.0f);
    CHECK(padded.x > 1.0f);
    CHECK(padded.y + padded.h < 1199.0f);
}

// On a display no bigger than a Deck's, Standard is drawn at the artwork's own
// size: the screens come out one screen pixel per display pixel, on whole
// pixels, and are marked as such so nothing tries to smooth them.
void test_standard_locks_to_native_on_small_displays() {
    const LayoutRegistry registry;
    const Layout& standard = *registry.find("standard");

    struct Case {
        float width;
        float height;
        bool locked;
    };
    const Case cases[] = {
        {1280.0f, 800.0f, true},    // a Deck, exactly on the threshold
        {1280.0f, 720.0f, true},    // and anything smaller
        {800.0f, 480.0f, true},     // small enough that the console overflows
        {1920.0f, 1080.0f, false},  // room to scale, so it is scaled
        {1366.0f, 768.0f, false},   // wider than the threshold
        {1280.0f, 1024.0f, false},  // taller than it
    };

    for (const Case& test : cases) {
        const PixelRect frame = layout_frame(standard, test.width, test.height, ds_screens());
        const std::string where =
            std::to_string(static_cast<int>(test.width)) + "x" +
            std::to_string(static_cast<int>(test.height));
        if (test.locked) {
            CHECK_MSG(std::abs(frame.w - 615.0f) < 0.01f && std::abs(frame.h - 630.0f) < 0.01f,
                      where + ": the console is " + std::to_string(frame.w) + "x" +
                          std::to_string(frame.h) + ", not its own size");
            // Centred, on whole pixels, and allowed over the edge.
            CHECK_NEAR(frame.x, std::round((test.width - 615.0f) * 0.5f));
            CHECK_NEAR(frame.y, std::round((test.height - 630.0f) * 0.5f));
        } else {
            CHECK_MSG(std::abs(frame.h - 630.0f) > 1.0f,
                      where + ": the console was locked to its own size with room to scale");
        }

        for (const PlacedScreen& placed : place_screens(standard, test.width, test.height, ds_screens())) {
            const std::string screen = where + " " + to_string(placed.screen) + " screen";
            if (test.locked) {
                CHECK_MSG(placed.native, screen + " is not one-to-one");
                CHECK_NEAR(placed.rect.w, static_cast<float>(kDsScreenWidth));
                CHECK_NEAR(placed.rect.h, static_cast<float>(kDsScreenHeight));
                // On the pixel grid, or the texels land between pixels and the
                // picture is resampled however it is filtered.
                CHECK_NEAR(placed.rect.x, std::round(placed.rect.x));
                CHECK_NEAR(placed.rect.y, std::round(placed.rect.y));
            } else {
                CHECK_MSG(!placed.native, screen + " claims to be one-to-one while scaled");
            }
        }
    }
}

// The rule is not Standard's alone: any screen that lands at its own size is
// left alone. The focus layouts' small screen does exactly that on a Deck.
void test_native_screens_are_spotted_anywhere() {
    const LayoutRegistry registry;
    for (const char* id : {"upper_focus", "touch_focus"}) {
        bool found_native = false;
        for (const PlacedScreen& placed : place_screens(*registry.find(id), 1280.0f, 800.0f, ds_screens())) {
            if (placed.native) {
                found_native = true;
                CHECK_NEAR(placed.rect.w, static_cast<float>(kDsScreenWidth));
                CHECK_NEAR(placed.rect.h, static_cast<float>(kDsScreenHeight));
            }
        }
        CHECK_MSG(found_native,
                  std::string(id) + "'s small screen is not one-to-one at 1280x800");
    }

    // A screen that is merely close is not called native, or it would be snapped
    // to a size it never had.
    const std::vector<PlacedScreen> stretched =
        place_screens(*registry.find("vertical"), 1280.0f, 800.0f, ds_screens());
    for (const PlacedScreen& placed : stretched) {
        CHECK(!placed.native);
    }
}

// A preview tile is not a display. Everything about a layout that depends on how
// big the display is has to be left behind when it is drawn into one, or the
// console is drawn at full size across the rest of the menu -- which is exactly
// what happened before as_preview() existed.
void test_previews_are_not_displays() {
    const LayoutRegistry registry;
    const Layout& standard = *registry.find("standard");

    // A tile is smaller than the threshold in both dimensions, so the console
    // would be drawn at its own size on it if nothing stripped that.
    const PixelRect tile{80.0f, 120.0f, 340.0f, 190.0f};
    const SystemScreens preview = as_preview(ds_screens());
    CHECK(ds_screens().console.native_width > 0.0f);
    CHECK_MSG(preview.console.native_width == 0.0f,
              "a preview kept the console's own pixel size");

    for (const PlacedScreen& placed : place_screens(standard, tile, preview)) {
        const std::string screen = std::string(to_string(placed.screen)) + " screen";
        CHECK_MSG(placed.rect.x >= tile.x - 0.5f && placed.rect.y >= tile.y - 0.5f &&
                      placed.rect.x + placed.rect.w <= tile.x + tile.w + 0.5f &&
                      placed.rect.y + placed.rect.h <= tile.y + tile.h + 0.5f,
                  "a preview's " + screen + " spilled outside its tile");
    }

    // And the system's own geometry is untouched: previewing must not change
    // what the display does with it.
    CHECK(ds_screens().console.native_width > 0.0f);
    const PixelRect display = layout_frame(standard, 1280.0f, 800.0f, ds_screens());
    CHECK_NEAR(display.w, 615.0f);
}

}  // namespace

int main() {
    test_bezel_decodes();
    test_bad_images_are_refused();
    test_standard_screens_sit_in_the_artwork();
    test_standard_locks_to_native_on_small_displays();
    test_native_screens_are_spotted_anywhere();
    test_previews_are_not_displays();
    return test::finish("image");
}
