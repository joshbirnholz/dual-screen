#include "test_support.hpp"

#include "core/libretro_screen_source.hpp"
#include <utility>

#include "layout/layout.hpp"
#include "render/compositor.hpp"
#include "core/system.hpp"
#include "layout/layout_registry.hpp"

using namespace ds;

namespace {

// Where a screen actually lands, which is all any caller really wants to know.
// Unless a test says otherwise, "the screens" means a DS's -- which is what
// most of this file is about.
const SystemScreens& ds_screens() { return system_profile(SystemId::Ds).screens; }

bool placed_screen(const Layout& layout,
                   ScreenId screen,
                   float width,
                   float height,
                   PlacedScreen& out,
                   const SystemScreens& screens = ds_screens()) {
    for (const PlacedScreen& placed : place_screens(layout, width, height, screens)) {
        if (placed.screen == screen) {
            out = placed;
            return true;
        }
    }
    return false;
}

void test_registry_contents() {
    const LayoutRegistry registry;
    // The Wii U Virtual Console's own Screen Settings, in its order. The order
    // is part of the point: it is the list this app is trying to be.
    const char* const expected[] = {"standard",  "upper_focus",  "touch_focus",
                                    "vertical",  "side_by_side", "upper_on_tv"};
    CHECK_EQ(registry.size(), sizeof(expected) / sizeof(expected[0]));
    for (std::size_t index = 0; index < registry.size(); ++index) {
        CHECK_STR_EQ(registry.at(index).id, expected[index]);
    }
    // Ids are unique and non-empty; config files key off them. Every layout has
    // to put its screens somewhere, however it describes them.
    for (const Layout& layout : registry.layouts()) {
        CHECK(!layout.id.empty());
        CHECK(!layout.display_name.empty());
        CHECK_MSG(place_screens(layout, 1280.0f, 800.0f, ds_screens()).size() == 2,
                  layout.id + " does not place both screens");
        CHECK_EQ(registry.index_of(layout.id) >= 0, true);
    }
    // An unknown id resolves rather than throwing, so a stale config still boots.
    CHECK_STR_EQ(registry.resolve("no_such_layout").id, LayoutRegistry::fallback_id());
    CHECK_STR_EQ(registry.resolve("").id, LayoutRegistry::fallback_id());

    // Ids from before the layout set was replaced land on the nearest new one
    // rather than dropping the player back to the default.
    CHECK_STR_EQ(registry.resolve("split_devices").id, "upper_on_tv");
    CHECK_STR_EQ(registry.resolve("horizontal").id, "side_by_side");
    CHECK_STR_EQ(registry.resolve("horizontal_swapped").id, "side_by_side");
    CHECK_STR_EQ(registry.resolve("vertical_swapped").id, "vertical");
    CHECK_STR_EQ(registry.resolve("top_only").id, "upper_focus");
    CHECK_STR_EQ(registry.resolve("bottom_only").id, "touch_focus");
}

// The arrangements are measured off the Wii U's own output: a 378 x 212 picture
// inside the TV frame in its Screen Settings shots. Checking against those
// numbers is the only way to know the layouts match rather than merely resemble.
// The window here is that same 378 x 212 so the numbers can be read directly.
void test_layouts_match_the_wii_u() {
    const LayoutRegistry registry;
    const float width = 378.0f;
    const float height = 212.0f;
    const float tolerance = 2.5f;  // a pixel or two off a measured screenshot

    struct Expected {
        const char* id;
        ScreenId screen;
        float x, y, w, h;
    };
    // Upper Screen Focus: 227x170 at (32,21) beside 76x57 at (270,134).
    // Touch Screen Focus: 76x57 at (32,21) beside 227x170 at (119,21).
    // Vertical:           126x94 at (126,3) above 126x94 at (126,115).
    // Side-by-Side:       154x204 at (19,4) beside 154x204 at (205,4).
    const Expected cases[] = {
        {"upper_focus", ScreenId::Top, 32, 21, 227, 170},
        {"upper_focus", ScreenId::Bottom, 270, 134, 76, 57},
        {"touch_focus", ScreenId::Top, 32, 21, 76, 57},
        {"touch_focus", ScreenId::Bottom, 119, 21, 227, 170},
        {"vertical", ScreenId::Top, 126, 3, 126, 94},
        {"vertical", ScreenId::Bottom, 126, 115, 126, 94},
        {"side_by_side", ScreenId::Top, 19, 4, 154, 204},
        {"side_by_side", ScreenId::Bottom, 205, 4, 154, 204},
    };

    for (const Expected& expected : cases) {
        PlacedScreen placed;
        const Layout& layout = *registry.find(expected.id);
        CHECK_MSG(placed_screen(layout, expected.screen, width, height, placed),
                  std::string(expected.id) + " did not place its " + to_string(expected.screen) +
                      " screen");
        const std::string where = std::string(expected.id) + "'s " + to_string(expected.screen);
        CHECK_MSG(std::abs(placed.rect.x - expected.x) < tolerance,
                  where + " x is " + std::to_string(placed.rect.x) + ", the Wii U's is " +
                      std::to_string(expected.x));
        CHECK_MSG(std::abs(placed.rect.y - expected.y) < tolerance,
                  where + " y is " + std::to_string(placed.rect.y) + ", the Wii U's is " +
                      std::to_string(expected.y));
        CHECK_MSG(std::abs(placed.rect.w - expected.w) < tolerance,
                  where + " width is " + std::to_string(placed.rect.w) + ", the Wii U's is " +
                      std::to_string(expected.w));
        CHECK_MSG(std::abs(placed.rect.h - expected.h) < tolerance,
                  where + " height is " + std::to_string(placed.rect.h) + ", the Wii U's is " +
                      std::to_string(expected.h));
    }

    // The alignment those numbers encode, stated outright so it survives a
    // change of window size: the small screen of each focus layout sits where
    // that screen sits on a real DS -- the touch screen low, the upper one high
    // -- and the big screen it stands beside shares that edge.
    for (const float w : {600.0f, 1280.0f, 1920.0f}) {
        for (const float h : {400.0f, 800.0f, 1200.0f}) {
            for (const Handedness hand : {Handedness::Right, Handedness::Left}) {
                const Layout upper =
                    resolve_for_handedness(*registry.find("upper_focus"), hand);
                const Layout touch =
                    resolve_for_handedness(*registry.find("touch_focus"), hand);
                PlacedScreen big;
                PlacedScreen small;
                CHECK(placed_screen(upper, ScreenId::Top, w, h, big));
                CHECK(placed_screen(upper, ScreenId::Bottom, w, h, small));
                CHECK_MSG(
                    std::abs((big.rect.y + big.rect.h) - (small.rect.y + small.rect.h)) < 1.0f,
                    "upper focus is not bottom-aligned");
                CHECK(small.rect.w < big.rect.w);

                CHECK(placed_screen(touch, ScreenId::Bottom, w, h, big));
                CHECK(placed_screen(touch, ScreenId::Top, w, h, small));
                CHECK_MSG(std::abs(big.rect.y - small.rect.y) < 1.0f,
                          "touch focus is not top-aligned");
                CHECK(small.rect.w < big.rect.w);
            }
        }
    }

    // The gap is a proportion, so it survives a change of window size too.
    for (const float w : {600.0f, 1280.0f, 1920.0f}) {
        PlacedScreen top;
        PlacedScreen bottom;
        CHECK(placed_screen(*registry.find("vertical"), ScreenId::Top, w, w * 0.625f, top));
        CHECK(placed_screen(*registry.find("vertical"), ScreenId::Bottom, w, w * 0.625f, bottom));
        const float gap = bottom.rect.y - (top.rect.y + top.rect.h);
        CHECK_MSG(std::abs(gap / top.rect.h - 18.0f / 94.0f) < 0.02f,
                  "the vertical gap is " + std::to_string(gap / top.rect.h) +
                      " of a screen, the Wii U's is 0.19");

        CHECK(placed_screen(*registry.find("side_by_side"), ScreenId::Top, w, w * 0.625f, top));
        CHECK(placed_screen(*registry.find("side_by_side"), ScreenId::Bottom, w, w * 0.625f,
                            bottom));
        const float side_gap = bottom.rect.x - (top.rect.x + top.rect.w);
        CHECK_MSG(std::abs(side_gap / top.rect.w - 32.0f / 154.0f) < 0.02f,
                  "the side-by-side gap is " + std::to_string(side_gap / top.rect.w) +
                      " of a screen, the Wii U's is 0.21");
    }
}

// Standard is the console itself: its shape comes from whichever system is
// running, and the screens sit exactly where that console's own screens are.
void test_standard_is_the_console() {
    const LayoutRegistry registry;
    const Layout& standard = *registry.find("standard");
    const SystemScreens screens = system_profile(SystemId::Ds).screens;
    CHECK(standard.uses_console_frame);
    CHECK(screens.console.exists());
    CHECK(screens.console.has_art);

    // Nothing else draws a console around itself.
    for (const Layout& layout : registry.layouts()) {
        if (layout.id == "standard") continue;
        CHECK_MSG(!layout.uses_console_frame, layout.id + " draws a console shell");
    }

    // The frame is letterboxed inside the window, so the shell is the same shape
    // on a 16:9 TV as on a 16:10 handheld.
    const PixelRect wide = layout_frame(standard, 1600.0f, 900.0f, screens);
    const PixelRect tall = layout_frame(standard, 800.0f, 1200.0f, screens);
    CHECK_NEAR(wide.w / wide.h, screens.console.aspect);
    CHECK_NEAR(tall.w / tall.h, screens.console.aspect);
    CHECK_MSG(wide.h <= 900.01f && wide.w <= 1600.01f, "the shell did not fit the window");
    // Centred, with the letterboxing split evenly.
    CHECK_NEAR(wide.x, (1600.0f - wide.w) * 0.5f);
    CHECK_NEAR(tall.y, (1200.0f - tall.h) * 0.5f);

    // Both screens sit inside the shell rather than over its edge.
    for (const PlacedScreen& placed : place_screens(standard, 1600.0f, 900.0f, screens)) {
        const PixelRect& rect = placed.rect;
        CHECK(rect.x >= wide.x - 0.01f && rect.x + rect.w <= wide.x + wide.w + 0.01f);
        CHECK(rect.y >= wide.y - 0.01f && rect.y + rect.h <= wide.y + wide.h + 0.01f);
    }
}

// Handedness mirrors a layout rather than duplicating it. Everything that has a
// side swaps sides; anything centred comes back unchanged.
void test_handedness_mirrors_the_layout() {
    const LayoutRegistry registry;

    const float width = 1280.0f;
    const float height = 800.0f;

    const Layout& focus = *registry.find("upper_focus");
    const Layout mirrored = resolve_for_handedness(focus, Handedness::Left);

    PlacedScreen right_big;
    PlacedScreen right_small;
    PlacedScreen left_big;
    PlacedScreen left_small;
    CHECK(placed_screen(focus, ScreenId::Top, width, height, right_big));
    CHECK(placed_screen(focus, ScreenId::Bottom, width, height, right_small));
    CHECK(placed_screen(mirrored, ScreenId::Top, width, height, left_big));
    CHECK(placed_screen(mirrored, ScreenId::Bottom, width, height, left_small));

    // Same sizes, and each screen's mirror image about the window's centre line.
    CHECK_NEAR(left_big.rect.w, right_big.rect.w);
    CHECK_NEAR(left_small.rect.w, right_small.rect.w);
    CHECK_NEAR(left_big.rect.y, right_big.rect.y);
    // Within a pixel: a screen that lands at its own size is snapped to whole
    // pixels, and the two hands round independently.
    CHECK(std::abs(left_big.rect.x - (width - right_big.rect.x - right_big.rect.w)) <= 1.0f);
    CHECK(std::abs(left_small.rect.x - (width - right_small.rect.x - right_small.rect.w)) <= 1.0f);
    // Right-handed puts the touch screen on the right, left-handed on the left.
    CHECK(right_small.rect.x > right_big.rect.x);
    CHECK(left_small.rect.x < left_big.rect.x);

    // Held sideways, the other hand turns the screens the other way.
    const Layout& sideways = *registry.find("side_by_side");
    PlacedScreen turned;
    CHECK(placed_screen(sideways, ScreenId::Top, width, height, turned));
    CHECK(turned.rotation == ScreenRotation::CounterClockwise);
    const Layout sideways_left = resolve_for_handedness(sideways, Handedness::Left);
    CHECK(placed_screen(sideways_left, ScreenId::Top, width, height, turned));
    CHECK(turned.rotation == ScreenRotation::Clockwise);

    // Mirroring twice is the identity, so the toggle cannot drift.
    const Layout back = resolve_for_handedness(sideways_left, Handedness::Left);
    PlacedScreen there;
    PlacedScreen and_back;
    CHECK(placed_screen(sideways, ScreenId::Bottom, width, height, there));
    CHECK(placed_screen(back, ScreenId::Bottom, width, height, and_back));
    CHECK_NEAR(and_back.rect.x, there.rect.x);
    CHECK(and_back.rotation == there.rotation);

    // A column has no left or right, so a stacked layout comes back unchanged.
    const Layout& vertical = *registry.find("vertical");
    const Layout vertical_left = resolve_for_handedness(vertical, Handedness::Left);
    PlacedScreen upright;
    PlacedScreen lefty;
    CHECK(placed_screen(vertical, ScreenId::Bottom, width, height, upright));
    CHECK(placed_screen(vertical_left, ScreenId::Bottom, width, height, lefty));
    CHECK_NEAR(lefty.rect.x, upright.rect.x);
    CHECK_NEAR(lefty.rect.y, upright.rect.y);
}

// A quarter-turned screen is 3:4 where an upright one is 4:3, and the letterbox
// has to be computed against the shape actually drawn.
void test_rotated_screens_letterbox_sideways() {
    ScreenPlacement placement;
    placement.w = 0.5f;
    placement.h = 1.0f;
    placement.fit = ScreenFit::Contain;
    placement.rotation = ScreenRotation::CounterClockwise;

    // Half of a 1000x1000 window is 500x1000, taller than 3:4, so the screen is
    // letterboxed to 500x666 rather than pillarboxed to 750x1000.
    const PixelRect rect = placement_pixel_rect(placement, 1000.0f, 1000.0f);
    CHECK_NEAR(rect.w, 500.0f);
    CHECK_NEAR(rect.h, 500.0f * 4.0f / 3.0f);
    // Taller than it is wide, which an upright screen never is.
    CHECK(rect.h > rect.w);
}

// Touching a sideways screen has to land where the finger is pointing, not
// ninety degrees off it.
void test_touch_follows_a_quarter_turn() {
    const LayoutRegistry registry;
    const Layout& sideways = *registry.find("side_by_side");
    const float width = 1280.0f;
    const float height = 800.0f;

    PlacedScreen bottom;
    CHECK(placed_screen(sideways, ScreenId::Bottom, width, height, bottom));
    const PixelRect rect = bottom.rect;

    DsTouchPoint point;
    // The centre is the centre whichever way the screen is turned, to within the
    // pixel the exact midpoint falls between.
    CHECK(map_output_point_to_ds(sideways, width, height, rect.x + rect.w * 0.5f,
                                 rect.y + rect.h * 0.5f, point, ds_screens()));
    CHECK(std::abs(point.x - 128) <= 1);
    CHECK(std::abs(point.y - 96) <= 1);

    // Turned anticlockwise, the DS's top-left corner is drawn at the bottom
    // left of the rect, so that is where touching it has to read from.
    CHECK(map_output_point_to_ds(sideways, width, height, rect.x + 1.0f,
                                 rect.y + rect.h - 1.0f, point, ds_screens()));
    CHECK_MSG(point.x <= 2 && point.y <= 2,
              "the bottom-left of a sideways screen was not the DS's top-left, it was " +
                  std::to_string(point.x) + "," + std::to_string(point.y));
    // ...and the DS's bottom-right is drawn at the top right.
    CHECK(map_output_point_to_ds(sideways, width, height, rect.x + rect.w - 1.0f, rect.y + 1.0f,
                                 point, ds_screens()));
    CHECK(point.x >= kDsScreenWidth - 3 && point.y >= kDsScreenHeight - 3);
}

void test_contain_preserves_aspect() {
    ScreenPlacement placement;
    placement.x = 0.0f;
    placement.y = 0.0f;
    placement.w = 1.0f;
    placement.h = 0.5f;
    placement.fit = ScreenFit::Contain;

    // A 16:9 window, vertical layout: each half is 1280x360, wider than 4:3, so
    // the screen is pillarboxed to 480x360 and centred.
    const PixelRect rect = placement_pixel_rect(placement, 1280.0f, 720.0f);
    CHECK_NEAR(rect.h, 360.0f);
    CHECK_NEAR(rect.w, 480.0f);
    CHECK_NEAR(rect.x, 400.0f);
    CHECK_NEAR(rect.y, 0.0f);
    CHECK_NEAR(rect.w / rect.h, 256.0f / 192.0f);
}

void test_stretch_fills_rect() {
    ScreenPlacement placement;
    placement.w = 1.0f;
    placement.h = 0.5f;
    placement.fit = ScreenFit::Stretch;
    const PixelRect rect = placement_pixel_rect(placement, 1280.0f, 720.0f);
    CHECK_NEAR(rect.w, 1280.0f);
    CHECK_NEAR(rect.h, 360.0f);
}

void test_touch_maps_through_bottom_placement() {
    const LayoutRegistry registry;
    const Layout& vertical = *registry.find("vertical");
    const float width = 1280.0f;
    const float height = 720.0f;

    PlacedScreen bottom;
    CHECK(placed_screen(vertical, ScreenId::Bottom, width, height, bottom));
    const PixelRect rect = bottom.rect;

    DsTouchPoint point;
    // Centre of the bottom screen -> centre of the DS touchscreen. Exactly on
    // the boundary between two DS pixels, so either of them is right.
    CHECK(map_output_point_to_ds(vertical, width, height, rect.x + rect.w * 0.5f,
                                 rect.y + rect.h * 0.5f, point, ds_screens()));
    CHECK(std::abs(point.x - 128) <= 1);
    CHECK(std::abs(point.y - 96) <= 1);

    // Corners clamp inside the screen rather than off it.
    CHECK(map_output_point_to_ds(vertical, width, height, rect.x + 0.5f, rect.y + 0.5f, point, ds_screens()));
    CHECK_EQ(point.x, 0);
    CHECK_EQ(point.y, 0);
    CHECK(map_output_point_to_ds(vertical, width, height, rect.x + rect.w - 0.5f,
                                 rect.y + rect.h - 0.5f, point, ds_screens()));
    CHECK_EQ(point.x, kDsScreenWidth - 1);
    CHECK_EQ(point.y, kDsScreenHeight - 1);

    // A touch on the top screen is not a touch at all.
    CHECK(!map_output_point_to_ds(vertical, width, height, width * 0.5f, height * 0.25f, point, ds_screens()));

    // One in the pillarbox beside the bottom screen clamps to the near edge
    // instead: a finger that lands just off the touch screen almost always
    // meant to land on it, and the nearest edge is where a stylus dragged past
    // the glass would actually have stopped.
    CHECK(map_output_point_to_ds(vertical, width, height, rect.x - 4.0f, rect.y + rect.h * 0.5f,
                                 point, ds_screens()));
    CHECK_EQ(point.x, 0);
    CHECK(std::abs(point.y - 96) <= 1);

    // Off past two edges at once -- squarely in a corner of the window that is
    // neither screen -- clamps to that corner of the touch screen.
    CHECK(map_output_point_to_ds(vertical, width, height, rect.x - 40.0f, rect.y - 40.0f, point,
                                 ds_screens()));
    CHECK_EQ(point.x, 0);
    CHECK_EQ(point.y, 0);
}

void test_touch_round_trips_for_every_layout() {
    const LayoutRegistry registry;
    const float width = 1280.0f;
    const float height = 800.0f;

    for (const Layout& base : registry.layouts()) {
      for (const Handedness hand : {Handedness::Right, Handedness::Left}) {
        const Layout layout = resolve_for_handedness(base, hand);
        PlacedScreen bottom_placed;
        const bool has_bottom = placed_screen(layout, ScreenId::Bottom, width, height, bottom_placed);
        if (!has_bottom || !layout.accepts_touch_on_bottom) {
            // A layout with no touchable bottom screen must reject touch
            // everywhere rather than mapping it somewhere arbitrary.
            DsTouchPoint ignored;
            CHECK_MSG(!map_output_point_to_ds(layout, width, height, width * 0.5f, height * 0.5f,
                                              ignored, ds_screens()),
                      layout.id + " accepted touch with no bottom screen shown");
            continue;
        }

        const PixelRect& rect = bottom_placed.rect;
        // Walk a grid of DS coordinates out to pixels and back again. Where the
        // screen was turned, the walk has to be turned with it -- which is the
        // point: the same rotation has to be applied and undone by two
        // different pieces of code, and this is what catches them disagreeing.
        for (int ds_y = 4; ds_y < kDsScreenHeight; ds_y += 37) {
            for (int ds_x = 4; ds_x < kDsScreenWidth; ds_x += 51) {
                float u = (ds_x + 0.5f) / kDsScreenWidth;
                float v = (ds_y + 0.5f) / kDsScreenHeight;
                switch (bottom_placed.rotation) {
                    case ScreenRotation::Clockwise: {
                        const float turned_u = 1.0f - v;
                        const float turned_v = u;
                        u = turned_u;
                        v = turned_v;
                        break;
                    }
                    case ScreenRotation::CounterClockwise: {
                        const float turned_u = v;
                        const float turned_v = 1.0f - u;
                        u = turned_u;
                        v = turned_v;
                        break;
                    }
                    case ScreenRotation::None: break;
                }
                const float px = rect.x + u * rect.w;
                const float py = rect.y + v * rect.h;
                DsTouchPoint point;
                const bool hit =
                    map_output_point_to_ds(layout, width, height, px, py, point, ds_screens());
                CHECK_MSG(hit, layout.id + " lost a touch inside its own bottom screen");
                if (!hit) continue;
                CHECK_MSG(std::abs(point.x - ds_x) <= 1,
                          layout.id + ": x round-tripped to " + std::to_string(point.x) + " from " +
                              std::to_string(ds_x));
                CHECK_MSG(std::abs(point.y - ds_y) <= 1,
                          layout.id + ": y round-tripped to " + std::to_string(point.y) + " from " +
                              std::to_string(ds_y));
            }
        }
      }
    }
}

void test_layouts_do_not_overlap() {
    const LayoutRegistry registry;
    for (const Layout& base : registry.layouts()) {
      for (const Handedness hand : {Handedness::Right, Handedness::Left}) {
        const Layout layout = resolve_for_handedness(base, hand);
        const std::vector<PlacedScreen> placed = place_screens(layout, 1000.0f, 1000.0f, ds_screens());
        for (std::size_t a = 0; a < placed.size(); ++a) {
            const PixelRect ra = placed[a].rect;
            CHECK_MSG(ra.x >= -0.01f && ra.y >= -0.01f && ra.x + ra.w <= 1000.01f &&
                          ra.y + ra.h <= 1000.01f,
                      layout.id + " places a screen outside the window");
            for (std::size_t b = a + 1; b < placed.size(); ++b) {
                const PixelRect rb = placed[b].rect;
                const bool disjoint = ra.x + ra.w <= rb.x + 0.01f || rb.x + rb.w <= ra.x + 0.01f ||
                                      ra.y + ra.h <= rb.y + 0.01f || rb.y + rb.h <= ra.y + 0.01f;
                CHECK_MSG(disjoint, layout.id + " overlaps its own screens");
            }
        }
      }
    }
}

void test_framebuffer_split() {
    std::string reason;
    // The shape the DS core is pinned to: one screen wide, two tall.
    const ScreenSplit split = compute_top_bottom_split(SystemId::Ds, 256, 384, &reason);
    CHECK(split.valid);
    CHECK_EQ(split.top_width, 256);
    CHECK_EQ(split.top_height, 192);
    CHECK_EQ(split.bottom_width, 256);
    CHECK_EQ(split.bottom_height, 192);
    CHECK_EQ(split.top_y, 0);
    CHECK_EQ(split.bottom_y, 192);

    // Upscaled output stays splittable, so an OpenGL-renderer core would work
    // without changing the crop maths.
    const ScreenSplit upscaled = compute_top_bottom_split(SystemId::Ds, 512, 768, &reason);
    CHECK(upscaled.valid);
    CHECK_EQ(upscaled.top_height, 384);

    // Anything that is not two screens stacked is rejected with a reason,
    // rather than silently cropping the wrong pixels.
    CHECK(!compute_top_bottom_split(SystemId::Ds, 512, 192, &reason).valid);
    CHECK(!reason.empty());
    CHECK(!compute_top_bottom_split(SystemId::Ds, 256, 400, &reason).valid);  // a screen gap
    CHECK(!compute_top_bottom_split(SystemId::Ds, 0, 0, &reason).valid);
    CHECK(!compute_top_bottom_split(SystemId::Ds, 256, 191, &reason).valid);  // odd height
}

// A 3DS packs two screens of different widths: 400x240 above 320x240, the
// narrower one centred under the wider. Cropping it as if the screens matched
// would take a strip of the upper screen's edge into the touch screen.
void test_3ds_framebuffer_split() {
    std::string reason;
    const ScreenSplit split = compute_top_bottom_split(SystemId::ThreeDs, 400, 480, &reason);
    CHECK_MSG(split.valid, reason);
    CHECK_EQ(split.top_width, 400);
    CHECK_EQ(split.top_height, 240);
    CHECK_EQ(split.top_x, 0);
    CHECK_EQ(split.top_y, 0);
    CHECK_EQ(split.bottom_width, 320);
    CHECK_EQ(split.bottom_height, 240);
    CHECK_EQ(split.bottom_x, 40);  // centred: (400 - 320) / 2
    CHECK_EQ(split.bottom_y, 240);

    // The core may render at a higher internal resolution; everything scales.
    const ScreenSplit upscaled = compute_top_bottom_split(SystemId::ThreeDs, 1200, 1440, &reason);
    CHECK_MSG(upscaled.valid, reason);
    CHECK_EQ(upscaled.top_width, 1200);
    CHECK_EQ(upscaled.bottom_width, 960);
    CHECK_EQ(upscaled.bottom_x, 120);
    CHECK_EQ(upscaled.bottom_y, 720);

    // A DS-shaped buffer from a 3DS core means the layout option did not take,
    // and is refused with an explanation rather than cropped into nonsense.
    CHECK(!compute_top_bottom_split(SystemId::ThreeDs, 256, 384, &reason).valid);
    CHECK(!reason.empty());
    // ...and the reverse, which is the same mistake the other way round.
    CHECK(!compute_top_bottom_split(SystemId::Ds, 400, 480, &reason).valid);
    // Not a whole multiple of the console's own size.
    CHECK(!compute_top_bottom_split(SystemId::ThreeDs, 600, 720, &reason).valid);
}

// Every layout works on a console whose screens are different shapes. This is
// the check that nothing is still assuming both screens match.
void test_layouts_handle_mismatched_screens() {
    const LayoutRegistry registry;
    const SystemScreens screens = system_profile(SystemId::ThreeDs).screens;
    CHECK(screens.top.width != screens.bottom.width);

    for (const Layout& base : registry.layouts()) {
        for (const Handedness hand : {Handedness::Right, Handedness::Left}) {
            const Layout layout = resolve_for_handedness(base, hand);
            const std::vector<PlacedScreen> placed =
                place_screens(layout, 1280.0f, 800.0f, screens);
            CHECK_MSG(placed.size() == 2, layout.id + " lost a screen on a 3DS");
            for (const PlacedScreen& entry : placed) {
                const ScreenSize size = screens.size_of(entry.screen);
                const float want = rotation_is_quarter_turn(entry.rotation)
                                       ? static_cast<float>(size.height) /
                                             static_cast<float>(size.width)
                                       : static_cast<float>(size.width) /
                                             static_cast<float>(size.height);
                const float got = entry.rect.w / entry.rect.h;
                CHECK_MSG(std::abs(got - want) < 0.02f,
                          layout.id + " drew the " + to_string(entry.screen) +
                              " screen at the wrong shape: " + std::to_string(got) +
                              " instead of " + std::to_string(want));
                CHECK(entry.rect.x >= -0.01f && entry.rect.y >= -0.01f &&
                      entry.rect.x + entry.rect.w <= 1280.01f &&
                      entry.rect.y + entry.rect.h <= 800.01f);
            }
        }
    }

    // A touch lands in the touch screen's own space, which is 320x240 here --
    // not a DS's 256x192, and not the upper screen's 400x240.
    const Layout& vertical = *registry.find("vertical");
    PlacedScreen bottom;
    CHECK(placed_screen(vertical, ScreenId::Bottom, 1280.0f, 800.0f, bottom, screens));
    DsTouchPoint point;
    CHECK(map_output_point_to_ds(vertical, 1280.0f, 800.0f,
                                 bottom.rect.x + bottom.rect.w - 1.0f,
                                 bottom.rect.y + bottom.rect.h - 1.0f, point, screens));
    CHECK_EQ(point.x, screens.bottom.width - 1);
    CHECK_EQ(point.y, screens.bottom.height - 1);
}

// Smoothing is a request, and this is what is done with it. The case that
// matters now is a core rendering above the size it is drawn at: point-sampling
// a reduction throws away the very pixels the higher resolution was paid for.
void test_smoothing_follows_the_texture_not_just_the_setting() {
    PlacedScreen placed;
    placed.screen = ScreenId::Top;
    placed.rect = PixelRect{0.0f, 0.0f, 400.0f, 240.0f};

    // One texel per pixel: never smoothed, whatever the setting says.
    CHECK_EQ(screen_needs_smoothing(placed, 400, 240, true), false);
    CHECK_EQ(screen_needs_smoothing(placed, 400, 240, false), false);

    // Magnified, which is the case the setting was added for.
    CHECK_EQ(screen_needs_smoothing(placed, 200, 120, true), true);
    CHECK_EQ(screen_needs_smoothing(placed, 200, 120, false), false);

    // Rendered at 2x and drawn at the console's own size: resampled either way,
    // because the alternative is dropping every other row.
    CHECK_EQ(screen_needs_smoothing(placed, 800, 480, false), true);
    CHECK_EQ(screen_needs_smoothing(placed, 800, 480, true), true);

    // A quarter turn swaps which way round the two meet, so a rotated screen
    // that lands one-to-one is still left alone.
    PlacedScreen turned = placed;
    turned.rotation = ScreenRotation::Clockwise;
    turned.rect = PixelRect{0.0f, 0.0f, 240.0f, 400.0f};
    CHECK_EQ(screen_needs_smoothing(turned, 400, 240, true), false);
    CHECK_EQ(screen_needs_smoothing(turned, 800, 480, false), true);
}

}  // namespace

int main() {
    test_registry_contents();
    test_layouts_match_the_wii_u();
    test_standard_is_the_console();
    test_handedness_mirrors_the_layout();
    test_rotated_screens_letterbox_sideways();
    test_touch_follows_a_quarter_turn();
    test_contain_preserves_aspect();
    test_stretch_fills_rect();
    test_touch_maps_through_bottom_placement();
    test_touch_round_trips_for_every_layout();
    test_layouts_do_not_overlap();
    test_framebuffer_split();
    test_3ds_framebuffer_split();
    test_layouts_handle_mismatched_screens();
    test_smoothing_follows_the_texture_not_just_the_setting();
    return test::finish("layout");
}
