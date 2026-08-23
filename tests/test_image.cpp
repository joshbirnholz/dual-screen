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

// Standard scales the console art to fit the window at every size, including
// a Deck's own resolution and below -- it used to lock to the artwork's own
// 615x630 pixel size there instead, leaving black bars around a picture much
// smaller than the window had room for. Above that size the art was already
// scaled to fit, and still is.
void test_standard_scales_to_fit_at_every_size() {
    const LayoutRegistry registry;
    const Layout& standard = *registry.find("standard");

    const std::pair<float, float> sizes[] = {
        {1280.0f, 800.0f},   // a Deck, where this used to lock to 615x630
        {1280.0f, 720.0f},
        {800.0f, 480.0f},    // smaller still
        {1920.0f, 1080.0f},
        {1366.0f, 768.0f},
        {1280.0f, 1024.0f},
    };

    for (const auto& size : sizes) {
        const float width = size.first;
        const float height = size.second;
        const PixelRect frame = layout_frame(standard, width, height, ds_screens());
        const std::string where =
            std::to_string(static_cast<int>(width)) + "x" + std::to_string(static_cast<int>(height));
        CHECK_NEAR(frame.w / frame.h, ds_screens().console.aspect);
        // Filling the window rather than pinned to the artwork's own 615x630:
        // the smaller side comes within a margin of the window's own size.
        const bool fills_width = frame.w > width * 0.9f;
        const bool fills_height = frame.h > height * 0.9f;
        CHECK_MSG(fills_width || fills_height,
                  where + ": the console did not scale to fill the window (" +
                      std::to_string(frame.w) + "x" + std::to_string(frame.h) + ")");
    }

    // On a display no bigger than a Deck's, the deliberate margin is gone: it
    // is a few percent of screen a small display cannot spare, unlike a
    // living-room TV. The frame fills the window's shorter side exactly. What
    // is left is not "padding" in that sense: it is whatever letterboxing the
    // console's own aspect ratio still needs against a window shaped
    // differently from it, the unavoidable price of not stretching or
    // cropping the artwork.
    const PixelRect padded = layout_frame(standard, 1280.0f, 800.0f, ds_screens());
    CHECK_NEAR(padded.y, 0.0f);
    CHECK_NEAR(padded.h, 800.0f);
    CHECK(padded.x > 1.0f);
    CHECK(padded.x + padded.w < 1279.0f);
}

// The exact window a composed layout's group lands at its own natural size in
// -- scale exactly 1, no scaling up or down -- computed from the layout's own
// fields rather than a resolution chosen by hand, so this stays correct
// however the focus layouts' proportions are tuned.
PixelRect natural_group_window(const LayoutComposition& composition, const SystemScreens& screens) {
    const bool horizontal = composition.axis == LayoutAxis::Horizontal;
    float along = 0.0f;
    float across = 0.0f;
    float largest_along = 0.0f;
    for (const ScreenItem& item : composition.items) {
        const ScreenSize size = screens.size_of(item.screen);
        const bool turned = rotation_is_quarter_turn(item.rotation);
        const float width = static_cast<float>(turned ? size.height : size.width) * item.scale;
        const float height = static_cast<float>(turned ? size.width : size.height) * item.scale;
        const float item_along = horizontal ? width : height;
        along += item_along;
        across = std::max(across, horizontal ? height : width);
        largest_along = std::max(largest_along, item_along);
    }
    along += composition.gap * largest_along * static_cast<float>(composition.items.size() - 1);
    // Margin left out of this formula on purpose: a group of real screens is
    // always well under the small-display threshold (1280x800) at its own
    // native size, where production code trims the margin to zero -- so the
    // window that lands the group at scale exactly 1 has to assume zero
    // margin too, or this would land at some other scale instead.
    return horizontal ? PixelRect{0.0f, 0.0f, along, across}
                      : PixelRect{0.0f, 0.0f, across, along};
}

// The rule is not Standard's alone: any screen that lands at its own size is
// left alone. The focus layouts' small screen does exactly that at the one
// window size where the whole group needs no scaling.
void test_native_screens_are_spotted_anywhere() {
    const LayoutRegistry registry;
    for (const char* id : {"upper_focus", "touch_focus"}) {
        const Layout& layout = *registry.find(id);
        const PixelRect window = natural_group_window(layout.composition, ds_screens());
        bool found_native = false;
        for (const PlacedScreen& placed : place_screens(layout, window, ds_screens())) {
            if (placed.native) {
                found_native = true;
                CHECK_NEAR(placed.rect.w, static_cast<float>(kDsScreenWidth));
                CHECK_NEAR(placed.rect.h, static_cast<float>(kDsScreenHeight));
            }
        }
        CHECK_MSG(found_native, std::string(id) + "'s small screen is not one-to-one at " +
                                     std::to_string(static_cast<int>(window.w)) + "x" +
                                     std::to_string(static_cast<int>(window.h)));
    }

    // A screen that is merely close is not called native, or it would be snapped
    // to a size it never had.
    const std::vector<PlacedScreen> stretched =
        place_screens(*registry.find("vertical"), 1280.0f, 800.0f, ds_screens());
    for (const PlacedScreen& placed : stretched) {
        CHECK(!placed.native);
    }
}

// A layout drawn into a preview tile fits inside it whatever the tile's size
// -- Standard always scales its console art to fit whatever rect it is given,
// so there is no fixed pixel size for it to fall back to and spill out of a
// tile smaller than that.
void test_previews_fit_inside_their_tile() {
    const LayoutRegistry registry;
    const Layout& standard = *registry.find("standard");

    const PixelRect tile{80.0f, 120.0f, 340.0f, 190.0f};
    for (const PlacedScreen& placed : place_screens(standard, tile, ds_screens())) {
        const std::string screen = std::string(to_string(placed.screen)) + " screen";
        CHECK_MSG(placed.rect.x >= tile.x - 0.5f && placed.rect.y >= tile.y - 0.5f &&
                      placed.rect.x + placed.rect.w <= tile.x + tile.w + 0.5f &&
                      placed.rect.y + placed.rect.h <= tile.y + tile.h + 0.5f,
                  "a preview's " + screen + " spilled outside its tile");
    }
}

}  // namespace

int main() {
    test_bezel_decodes();
    test_bad_images_are_refused();
    test_standard_screens_sit_in_the_artwork();
    test_standard_scales_to_fit_at_every_size();
    test_native_screens_are_spotted_anywhere();
    test_previews_fit_inside_their_tile();
    return test::finish("image");
}
