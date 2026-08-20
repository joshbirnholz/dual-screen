#include "test_support.hpp"

#include "core/libretro_screen_source.hpp"
#include <utility>

#include "layout/layout.hpp"
#include "layout/layout_registry.hpp"

using namespace ds;

namespace {

const ScreenPlacement* placement_for(const Layout& layout, ScreenId screen) {
    for (const ScreenPlacement& placement : layout.placements) {
        if (placement.screen == screen) {
            return &placement;
        }
    }
    return nullptr;
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
    // Ids are unique and non-empty; config files key off them.
    for (const Layout& layout : registry.layouts()) {
        CHECK(!layout.id.empty());
        CHECK(!layout.display_name.empty());
        CHECK(!layout.placements.empty());
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

// Standard is the console itself: the shell keeps a real DS's proportions
// whatever shape the window is, and it is the one layout that is never smoothed.
void test_standard_is_the_console() {
    const LayoutRegistry registry;
    const Layout& standard = *registry.find("standard");
    CHECK(standard.bezel);
    CHECK(standard.forbids_filtering);
    CHECK(standard.frame_aspect > 0.0f);

    // Nothing else claims either.
    for (const Layout& layout : registry.layouts()) {
        if (layout.id == "standard") continue;
        CHECK_MSG(!layout.bezel, layout.id + " draws a console shell");
        CHECK_MSG(!layout.forbids_filtering, layout.id + " refuses smoothing");
    }

    // The frame is letterboxed inside the window, so the shell is the same shape
    // on a 16:9 TV as on a 16:10 handheld.
    const PixelRect wide = layout_frame(standard, 1600.0f, 900.0f);
    const PixelRect tall = layout_frame(standard, 800.0f, 1200.0f);
    CHECK_NEAR(wide.w / wide.h, standard.frame_aspect);
    CHECK_NEAR(tall.w / tall.h, standard.frame_aspect);
    CHECK_MSG(wide.h <= 900.01f && wide.w <= 1600.01f, "the shell did not fit the window");
    // Centred, with the letterboxing split evenly.
    CHECK_NEAR(wide.x, (1600.0f - wide.w) * 0.5f);
    CHECK_NEAR(tall.y, (1200.0f - tall.h) * 0.5f);

    // Both screens sit inside the shell rather than over its edge.
    for (const ScreenPlacement& placement : standard.placements) {
        const PixelRect rect = placement_pixel_rect(placement, wide);
        CHECK(rect.x >= wide.x - 0.01f && rect.x + rect.w <= wide.x + wide.w + 0.01f);
        CHECK(rect.y >= wide.y - 0.01f && rect.y + rect.h <= wide.y + wide.h + 0.01f);
    }
}

// Handedness mirrors a layout rather than duplicating it. Everything that has a
// side swaps sides; anything centred comes back unchanged.
void test_handedness_mirrors_the_layout() {
    const LayoutRegistry registry;

    const Layout& focus = *registry.find("upper_focus");
    const Layout mirrored = resolve_for_handedness(focus, Handedness::Left);
    CHECK_EQ(mirrored.placements.size(), focus.placements.size());
    for (std::size_t i = 0; i < focus.placements.size(); ++i) {
        // Same size, opposite side.
        CHECK_NEAR(mirrored.placements[i].w, focus.placements[i].w);
        CHECK_NEAR(mirrored.placements[i].h, focus.placements[i].h);
        CHECK_NEAR(mirrored.placements[i].y, focus.placements[i].y);
        CHECK_NEAR(mirrored.placements[i].x,
                   1.0f - focus.placements[i].x - focus.placements[i].w);
    }

    // Right-handed is the layout as written, so it is left alone entirely.
    const Layout right = resolve_for_handedness(focus, Handedness::Right);
    CHECK_NEAR(right.placements[0].x, focus.placements[0].x);

    // Held sideways, the other hand turns the screens the other way.
    const Layout& sideways = *registry.find("side_by_side");
    CHECK(sideways.placements[0].rotation == ScreenRotation::CounterClockwise);
    const Layout sideways_left = resolve_for_handedness(sideways, Handedness::Left);
    CHECK(sideways_left.placements[0].rotation == ScreenRotation::Clockwise);

    // Mirroring twice is the identity, so the toggle cannot drift.
    const Layout back = resolve_for_handedness(sideways_left, Handedness::Left);
    CHECK_NEAR(back.placements[0].x, sideways.placements[0].x);
    CHECK(back.placements[0].rotation == sideways.placements[0].rotation);

    // A centred layout has no side to swap.
    const Layout& vertical = *registry.find("vertical");
    const Layout vertical_left = resolve_for_handedness(vertical, Handedness::Left);
    for (std::size_t i = 0; i < vertical.placements.size(); ++i) {
        CHECK_NEAR(vertical_left.placements[i].x, vertical.placements[i].x);
    }
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

    const ScreenPlacement* bottom = placement_for(sideways, ScreenId::Bottom);
    CHECK(bottom != nullptr);
    if (bottom == nullptr) return;
    const PixelRect rect = placement_pixel_rect(*bottom, width, height);

    DsTouchPoint point;
    // The centre is the centre whichever way the screen is turned.
    CHECK(map_output_point_to_ds(sideways, width, height, rect.x + rect.w * 0.5f,
                                 rect.y + rect.h * 0.5f, point));
    CHECK_EQ(point.x, 128);
    CHECK_EQ(point.y, 96);

    // Turned anticlockwise, the DS's top-left corner is drawn at the bottom
    // left of the rect, so that is where touching it has to read from.
    CHECK(map_output_point_to_ds(sideways, width, height, rect.x + 1.0f,
                                 rect.y + rect.h - 1.0f, point));
    CHECK_MSG(point.x <= 2 && point.y <= 2,
              "the bottom-left of a sideways screen was not the DS's top-left, it was " +
                  std::to_string(point.x) + "," + std::to_string(point.y));
    // ...and the DS's bottom-right is drawn at the top right.
    CHECK(map_output_point_to_ds(sideways, width, height, rect.x + rect.w - 1.0f, rect.y + 1.0f,
                                 point));
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

    const ScreenPlacement* bottom = placement_for(vertical, ScreenId::Bottom);
    CHECK(bottom != nullptr);
    const PixelRect rect = placement_pixel_rect(*bottom, width, height);

    DsTouchPoint point;
    // Centre of the bottom screen -> centre of the DS touchscreen.
    CHECK(map_output_point_to_ds(vertical, width, height, rect.x + rect.w * 0.5f,
                                 rect.y + rect.h * 0.5f, point));
    CHECK_EQ(point.x, 128);
    CHECK_EQ(point.y, 96);

    // Corners clamp inside the screen rather than off it.
    CHECK(map_output_point_to_ds(vertical, width, height, rect.x + 0.5f, rect.y + 0.5f, point));
    CHECK_EQ(point.x, 0);
    CHECK_EQ(point.y, 0);
    CHECK(map_output_point_to_ds(vertical, width, height, rect.x + rect.w - 0.5f,
                                 rect.y + rect.h - 0.5f, point));
    CHECK_EQ(point.x, kDsScreenWidth - 1);
    CHECK_EQ(point.y, kDsScreenHeight - 1);

    // A touch on the top screen is not a touch at all.
    CHECK(!map_output_point_to_ds(vertical, width, height, width * 0.5f, height * 0.25f, point));
    // Nor is one in the pillarbox beside the bottom screen.
    CHECK(!map_output_point_to_ds(vertical, width, height, rect.x - 4.0f,
                                  rect.y + rect.h * 0.5f, point));
}

void test_touch_round_trips_for_every_layout() {
    const LayoutRegistry registry;
    const float width = 1280.0f;
    const float height = 800.0f;

    for (const Layout& base : registry.layouts()) {
      for (const Handedness hand : {Handedness::Right, Handedness::Left}) {
        const Layout layout = resolve_for_handedness(base, hand);
        const ScreenPlacement* bottom = placement_for(layout, ScreenId::Bottom);
        if (bottom == nullptr || !layout.accepts_touch_on_bottom) {
            // A layout with no touchable bottom screen must reject touch
            // everywhere rather than mapping it somewhere arbitrary.
            DsTouchPoint ignored;
            CHECK_MSG(!map_output_point_to_ds(layout, width, height, width * 0.5f, height * 0.5f,
                                              ignored),
                      layout.id + " accepted touch with no bottom screen shown");
            continue;
        }

        const PixelRect frame = layout_frame(layout, width, height);
        const PixelRect rect = placement_pixel_rect(*bottom, frame);
        // Walk a grid of DS coordinates out to pixels and back again. Where the
        // screen was turned, the walk has to be turned with it -- which is the
        // point: the same rotation has to be applied and undone by two
        // different pieces of code, and this is what catches them disagreeing.
        for (int ds_y = 4; ds_y < kDsScreenHeight; ds_y += 37) {
            for (int ds_x = 4; ds_x < kDsScreenWidth; ds_x += 51) {
                float u = (ds_x + 0.5f) / kDsScreenWidth;
                float v = (ds_y + 0.5f) / kDsScreenHeight;
                switch (bottom->rotation) {
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
                const bool hit = map_output_point_to_ds(layout, width, height, px, py, point);
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
        const PixelRect frame = layout_frame(layout, 1000.0f, 1000.0f);
        for (std::size_t a = 0; a < layout.placements.size(); ++a) {
            const PixelRect ra = placement_pixel_rect(layout.placements[a], frame);
            CHECK_MSG(ra.x >= -0.01f && ra.y >= -0.01f && ra.x + ra.w <= 1000.01f &&
                          ra.y + ra.h <= 1000.01f,
                      layout.id + " places a screen outside the window");
            for (std::size_t b = a + 1; b < layout.placements.size(); ++b) {
                const PixelRect rb = placement_pixel_rect(layout.placements[b], frame);
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
    // The shape the core is pinned to: one screen wide, two tall.
    const ScreenSplit split = compute_top_bottom_split(256, 384, &reason);
    CHECK(split.valid);
    CHECK_EQ(split.screen_width, 256);
    CHECK_EQ(split.screen_height, 192);
    CHECK_EQ(split.top_y, 0);
    CHECK_EQ(split.bottom_y, 192);

    // Upscaled output stays splittable, so an OpenGL-renderer core would work
    // without changing the crop maths.
    const ScreenSplit upscaled = compute_top_bottom_split(512, 768, &reason);
    CHECK(upscaled.valid);
    CHECK_EQ(upscaled.screen_height, 384);

    // Anything that is not two screens stacked is rejected with a reason,
    // rather than silently cropping the wrong pixels.
    CHECK(!compute_top_bottom_split(512, 192, &reason).valid);
    CHECK(!reason.empty());
    CHECK(!compute_top_bottom_split(256, 400, &reason).valid);  // a screen gap
    CHECK(!compute_top_bottom_split(0, 0, &reason).valid);
    CHECK(!compute_top_bottom_split(256, 191, &reason).valid);  // odd height
}

}  // namespace

int main() {
    test_registry_contents();
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
    return test::finish("layout");
}
