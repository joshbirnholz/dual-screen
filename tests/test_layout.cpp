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
    // The layouts the spec asks V1 to ship.
    for (const char* id : {"vertical", "vertical_swapped", "horizontal", "top_only", "bottom_only"}) {
        CHECK_MSG(registry.find(id) != nullptr, std::string("missing layout ") + id);
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

void test_swapped_layouts_mirror_their_counterparts() {
    const LayoutRegistry registry;
    // Screen swapping is expressed as layouts rather than as a flag, so each
    // swapped layout must place the same rects as its counterpart with the two
    // screens exchanged.
    const std::pair<const char*, const char*> pairs[] = {
        {"vertical", "vertical_swapped"},
        {"horizontal", "horizontal_swapped"},
    };
    for (const auto& pair : pairs) {
        const Layout* plain = registry.find(pair.first);
        const Layout* swapped = registry.find(pair.second);
        CHECK(plain != nullptr && swapped != nullptr);
        if (plain == nullptr || swapped == nullptr) continue;

        CHECK_EQ(swapped->placements.size(), plain->placements.size());
        for (std::size_t i = 0; i < swapped->placements.size(); ++i) {
            // Same geometry...
            CHECK_NEAR(swapped->placements[i].x, plain->placements[i].x);
            CHECK_NEAR(swapped->placements[i].y, plain->placements[i].y);
            CHECK_NEAR(swapped->placements[i].w, plain->placements[i].w);
            CHECK_NEAR(swapped->placements[i].h, plain->placements[i].h);
            // ...opposite screen.
            CHECK(swapped->placements[i].screen != plain->placements[i].screen);
        }
    }
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

void test_touch_follows_the_swapped_layout() {
    const LayoutRegistry registry;
    const Layout& swapped = *registry.find("vertical_swapped");
    const float width = 1280.0f;
    const float height = 720.0f;

    // The swapped layout draws the bottom screen in the upper half, and touch
    // has to follow it there.
    DsTouchPoint point;
    CHECK(map_output_point_to_ds(swapped, width, height, width * 0.5f, height * 0.25f, point));
    CHECK_EQ(point.x, 128);
    CHECK(!map_output_point_to_ds(swapped, width, height, width * 0.5f, height * 0.75f, point));
}

void test_touch_round_trips_for_every_layout() {
    const LayoutRegistry registry;
    const float width = 1280.0f;
    const float height = 800.0f;

    for (const Layout& layout : registry.layouts()) {
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

        const PixelRect rect = placement_pixel_rect(*bottom, width, height);
        // Walk a grid of DS coordinates out to pixels and back again.
        for (int ds_y = 4; ds_y < kDsScreenHeight; ds_y += 37) {
            for (int ds_x = 4; ds_x < kDsScreenWidth; ds_x += 51) {
                const float px = rect.x + (ds_x + 0.5f) / kDsScreenWidth * rect.w;
                const float py = rect.y + (ds_y + 0.5f) / kDsScreenHeight * rect.h;
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

void test_layouts_do_not_overlap() {
    const LayoutRegistry registry;
    for (const Layout& layout : registry.layouts()) {
        for (std::size_t a = 0; a < layout.placements.size(); ++a) {
            const PixelRect ra = placement_pixel_rect(layout.placements[a], 1000.0f, 1000.0f);
            CHECK_MSG(ra.x >= -0.01f && ra.y >= -0.01f && ra.x + ra.w <= 1000.01f &&
                          ra.y + ra.h <= 1000.01f,
                      layout.id + " places a screen outside the window");
            for (std::size_t b = a + 1; b < layout.placements.size(); ++b) {
                const PixelRect rb = placement_pixel_rect(layout.placements[b], 1000.0f, 1000.0f);
                const bool disjoint = ra.x + ra.w <= rb.x + 0.01f || rb.x + rb.w <= ra.x + 0.01f ||
                                      ra.y + ra.h <= rb.y + 0.01f || rb.y + rb.h <= ra.y + 0.01f;
                CHECK_MSG(disjoint, layout.id + " overlaps its own screens");
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
    test_contain_preserves_aspect();
    test_stretch_fills_rect();
    test_swapped_layouts_mirror_their_counterparts();
    test_touch_maps_through_bottom_placement();
    test_touch_follows_the_swapped_layout();
    test_touch_round_trips_for_every_layout();
    test_layouts_do_not_overlap();
    test_framebuffer_split();
    return test::finish("layout");
}
