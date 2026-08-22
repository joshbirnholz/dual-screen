#include "test_support.hpp"

#include <vector>

#include "core/system.hpp"
#include "layout/layout_registry.hpp"
#include "ui/layout_picker.hpp"

using namespace ds;

namespace {

constexpr float kWidth = 1280.0f;
constexpr float kHeight = 800.0f;

// A tap, the way the menu receives one: it acts on the release.
PointerEvent tap(float x, float y) {
    PointerEvent pointer;
    pointer.x = x;
    pointer.y = y;
    pointer.released = true;
    pointer.normalized = false;
    return pointer;
}

// Taps every point on a coarse grid over the menu and reports how many of them
// asked for a different render scale. The footer's rectangles are the picker's
// own business, so this finds the button rather than being told where it is --
// which also means it notices a button that stopped being reachable.
int taps_that_cycle_the_scale(const LayoutRegistry& registry, const LayoutPickerState& state) {
    int hits = 0;
    for (float y = 4.0f; y < kHeight; y += 8.0f) {
        for (float x = 4.0f; x < kWidth; x += 8.0f) {
            LayoutPicker picker(registry);
            picker.open(state);
            const LayoutPickerResult result =
                picker.update({}, {tap(x, y)}, kWidth, kHeight);
            if (result.render_scale_changed) ++hits;
        }
    }
    return hits;
}

// The render scale is offered wherever the console being played has one to
// choose. That is both devices: this is one menu that happens to be reachable
// from either end, and a companion asks the host rather than acting locally.
void test_the_render_button_appears_where_there_is_a_scale_to_choose() {
    LayoutRegistry registry;

    LayoutPickerState playing;
    playing.layout_id = registry.at(0).id;
    playing.render_scale = 1;
    playing.max_render_scale = system_profile(SystemId::ThreeDs).max_render_scale;
    CHECK(playing.max_render_scale > 1);
    CHECK(taps_that_cycle_the_scale(registry, playing) > 0);

    // The companion end of the same 3DS session: the same button, the same
    // size, because it is the same menu.
    LayoutPickerState companion = playing;
    companion.companion = true;
    companion.session_active = true;
    CHECK(taps_that_cycle_the_scale(registry, companion) > 0);

    // A DS: melonDS is pinned to its software renderer, so there is nothing to
    // choose and nothing to press -- on either device.
    LayoutPickerState ds = playing;
    ds.max_render_scale = system_profile(SystemId::Ds).max_render_scale;
    CHECK_EQ(ds.max_render_scale, 1);
    CHECK_EQ(taps_that_cycle_the_scale(registry, ds), 0);

    LayoutPickerState ds_companion = ds;
    ds_companion.companion = true;
    ds_companion.session_active = true;
    CHECK_EQ(taps_that_cycle_the_scale(registry, ds_companion), 0);
}

// The other footer buttons keep working with an eighth one among them: the row
// wraps rather than dropping anything, and each button still reports its own
// intent and no one else's.
void test_the_other_buttons_still_work() {
    LayoutRegistry registry;
    LayoutPickerState state;
    state.layout_id = registry.at(0).id;
    state.session_active = true;
    state.max_render_scale = 3;

    int filtering = 0;
    int sound = 0;
    int hands = 0;
    int cursor = 0;
    int reset = 0;
    int resume = 0;
    int disconnect = 0;
    int quit = 0;
    for (float y = 4.0f; y < kHeight; y += 8.0f) {
        for (float x = 4.0f; x < kWidth; x += 8.0f) {
            LayoutPicker picker(registry);
            picker.open(state);
            const LayoutPickerResult result = picker.update({}, {tap(x, y)}, kWidth, kHeight);
            if (result.filtering_toggled) ++filtering;
            if (result.audio_target_changed) ++sound;
            if (result.handedness_changed) ++hands;
            if (result.cursor_mode_changed) ++cursor;
            if (result.reset_requested) ++reset;
            if (result.close_requested) ++resume;
            if (result.disconnect_requested) ++disconnect;
            if (result.quit_requested) ++quit;
        }
    }
    CHECK(filtering > 0);
    CHECK(sound > 0);
    CHECK(hands > 0);
    CHECK(cursor > 0);
    CHECK(reset > 0);
    CHECK(resume > 0);
    CHECK(disconnect > 0);
    CHECK(quit > 0);
}

// Reset closes the menu the same way Resume does -- there is nothing left to
// look at once the game has restarted -- but is its own distinct result so the
// caller knows to actually restart the game rather than just dismiss the menu.
void test_reset_closes_the_menu() {
    LayoutRegistry registry;
    LayoutPickerState state;
    state.layout_id = registry.at(0).id;

    LayoutPicker picker(registry);
    picker.open(state);
    CHECK(picker.is_open());

    bool found = false;
    for (float y = 4.0f; y < kHeight && !found; y += 8.0f) {
        for (float x = 4.0f; x < kWidth && !found; x += 8.0f) {
            const LayoutPickerResult result = picker.update({}, {tap(x, y)}, kWidth, kHeight);
            if (result.reset_requested) {
                found = true;
                CHECK(!result.close_requested);
                CHECK_MSG(!picker.is_open(), "the menu stayed open after Reset Game");
            }
        }
    }
    CHECK_MSG(found, "never found the Reset Game button");
}

}  // namespace

int main() {
    test_the_render_button_appears_where_there_is_a_scale_to_choose();
    test_the_other_buttons_still_work();
    test_reset_closes_the_menu();
    return test::finish("quick-settings");
}
