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
    companion.session_active = true;
    CHECK(taps_that_cycle_the_scale(registry, companion) > 0);

    // A DS: melonDS is pinned to its software renderer, so there is nothing to
    // choose and nothing to press -- on either device.
    LayoutPickerState ds = playing;
    ds.max_render_scale = system_profile(SystemId::Ds).max_render_scale;
    CHECK_EQ(ds.max_render_scale, 1);
    CHECK_EQ(taps_that_cycle_the_scale(registry, ds), 0);

    LayoutPickerState ds_companion = ds;
    ds_companion.session_active = true;
    CHECK_EQ(taps_that_cycle_the_scale(registry, ds_companion), 0);
}

// Taps every point on a coarse grid over the menu and reports how many of them
// cycled the right stick's mapping.
int taps_that_cycle_the_right_stick(const LayoutRegistry& registry, const LayoutPickerState& state) {
    int hits = 0;
    for (float y = 4.0f; y < kHeight; y += 8.0f) {
        for (float x = 4.0f; x < kWidth; x += 8.0f) {
            LayoutPicker picker(registry);
            picker.open(state);
            const LayoutPickerResult result = picker.update({}, {tap(x, y)}, kWidth, kHeight);
            if (result.right_stick_mode_changed) ++hits;
        }
    }
    return hits;
}

// What the right stick does is only a question on a console that has a stick
// of its own. A 3DS gets the button on both devices -- one menu, reachable
// from either end -- and a DS gets nothing to press, because there its left
// stick is already standing in for the d-pad and the right one has nothing to
// be.
void test_the_right_stick_button_appears_only_where_there_is_a_stick() {
    LayoutRegistry registry;

    LayoutPickerState three_ds;
    three_ds.layout_id = registry.at(0).id;
    three_ds.right_stick_choosable = system_profile(SystemId::ThreeDs).has_analog_stick;
    CHECK(three_ds.right_stick_choosable);
    CHECK(taps_that_cycle_the_right_stick(registry, three_ds) > 0);

    LayoutPickerState companion = three_ds;
    companion.session_active = true;
    CHECK(taps_that_cycle_the_right_stick(registry, companion) > 0);

    LayoutPickerState ds = three_ds;
    ds.right_stick_choosable = system_profile(SystemId::Ds).has_analog_stick;
    CHECK(!ds.right_stick_choosable);
    CHECK_EQ(taps_that_cycle_the_right_stick(registry, ds), 0);
}

// Taps every point on a coarse grid over the menu and reports how many of them
// toggled the screen gap.
int taps_that_toggle_the_gap(const LayoutRegistry& registry, const LayoutPickerState& state) {
    int hits = 0;
    for (float y = 4.0f; y < kHeight; y += 8.0f) {
        for (float x = 4.0f; x < kWidth; x += 8.0f) {
            LayoutPicker picker(registry);
            picker.open(state);
            const LayoutPickerResult result = picker.update({}, {tap(x, y)}, kWidth, kHeight);
            if (result.screen_gap_toggled) ++hits;
        }
    }
    return hits;
}

// The gap button appears while a layout that offers the choice is selected
// -- Vertical Screens and the two focus layouts -- and nowhere else, on
// either device.
void test_the_gap_button_appears_only_where_the_layout_offers_it() {
    LayoutRegistry registry;

    for (const char* id : {"vertical", "upper_focus", "touch_focus"}) {
        LayoutPickerState state;
        state.layout_id = id;
        CHECK_MSG(taps_that_toggle_the_gap(registry, state) > 0,
                  std::string(id) + " should offer the gap button");

        // The companion end of the same session: the same button, because it
        // is the same menu.
        LayoutPickerState companion = state;
        companion.session_active = true;
        CHECK_MSG(taps_that_toggle_the_gap(registry, companion) > 0,
                  std::string(id) + " (companion) should offer the gap button");
    }

    // The layouts with no gap to choose: nothing to press. Side-by-Side's gap
    // is the DS's own hinge rather than a choice -- see
    // LayoutComposition::gap_toggleable -- and Standard has no gap at all.
    for (const char* id : {"side_by_side", "standard"}) {
        LayoutPickerState state;
        state.layout_id = id;
        CHECK_MSG(taps_that_toggle_the_gap(registry, state) == 0,
                  std::string(id) + " should not offer the gap button");
    }
}

// Picking a gap layout from inside the open menu has to bring the button with
// it. A game starts on Standard, which has no gap of its own, so a footer built
// once at open() and never rebuilt leaves the choice unreachable: pick Vertical
// Screens, look for Gap, and it is not there until the menu is closed and
// opened again.
void test_choosing_a_gap_layout_reveals_the_button() {
    LayoutRegistry registry;
    LayoutPickerState opened_on_standard;
    opened_on_standard.layout_id = "standard";

    // Where the Vertical Screens tile is, found the way a finger finds it
    // rather than by reaching into the picker's own geometry.
    PointerEvent vertical_tile;
    bool found = false;
    for (float y = 4.0f; y < kHeight && !found; y += 8.0f) {
        for (float x = 4.0f; x < kWidth && !found; x += 8.0f) {
            LayoutPicker picker(registry);
            picker.open(opened_on_standard);
            picker.update({}, {tap(x, y)}, kWidth, kHeight);
            if (picker.selected_layout_id() == "vertical") {
                vertical_tile = tap(x, y);
                found = true;
            }
        }
    }
    CHECK_MSG(found, "never found the Vertical Screens tile");

    int hits = 0;
    for (float y = 4.0f; y < kHeight; y += 8.0f) {
        for (float x = 4.0f; x < kWidth; x += 8.0f) {
            LayoutPicker picker(registry);
            picker.open(opened_on_standard);
            picker.update({}, {vertical_tile}, kWidth, kHeight);
            if (picker.update({}, {tap(x, y)}, kWidth, kHeight).screen_gap_toggled) ++hits;
        }
    }
    CHECK_MSG(hits > 0, "no Gap button after picking Vertical Screens from inside the menu");

    // And the other direction, which is how it arrives on a companion: the
    // host changes the layout under an open menu, and a button belonging to
    // the layout that just left must not linger.
    LayoutPickerState opened_on_vertical;
    opened_on_vertical.layout_id = "vertical";
    int lingering = 0;
    for (float y = 4.0f; y < kHeight; y += 8.0f) {
        for (float x = 4.0f; x < kWidth; x += 8.0f) {
            LayoutPicker picker(registry);
            picker.open(opened_on_vertical);
            picker.set_selected_layout("standard");
            if (picker.update({}, {tap(x, y)}, kWidth, kHeight).screen_gap_toggled) ++lingering;
        }
    }
    CHECK_EQ(lingering, 0);
}

// Toggling Gap has to change what the tiles themselves draw, not just what a
// resumed game looks like -- the whole point of a preview is showing the
// player the arrangement they are about to get. render() draws every tile
// through resolved_layout_for_preview(), so this checks that one function
// rather than needing a GL context to read pixels back out of a render.
void test_toggling_the_gap_updates_what_the_previews_would_draw() {
    LayoutRegistry registry;
    const Layout& upper_focus = *registry.find("upper_focus");

    LayoutPickerState state;
    state.layout_id = "vertical";
    state.screen_gap = true;

    LayoutPicker picker(registry);
    picker.open(state);
    const Layout with_gap = picker.resolved_layout_for_preview(upper_focus);
    CHECK_NEAR(with_gap.composition.gap, upper_focus.composition.gap);

    // Same picker, same open menu -- just the toggle changing, the way
    // set_screen_gap() does when the app applies a request. The next preview
    // drawn has to reflect it immediately, without reopening the menu.
    picker.set_screen_gap(false);
    const Layout without_gap = picker.resolved_layout_for_preview(upper_focus);
    CHECK_NEAR(without_gap.composition.gap, 0.0f);

    // Side-by-Side's gap is not a choice, so it must come back unchanged
    // whichever way the toggle is set.
    const Layout& side_by_side = *registry.find("side_by_side");
    const Layout side_by_side_preview = picker.resolved_layout_for_preview(side_by_side);
    CHECK_NEAR(side_by_side_preview.composition.gap, side_by_side.composition.gap);
}

// The other footer buttons keep working with an eighth one among them: the row
// wraps rather than dropping anything, and each button still reports its own
// intent and no one else's.
void test_the_other_buttons_still_work() {
    LayoutRegistry registry;
    LayoutPickerState state;
    state.layout_id = "vertical";
    state.session_active = true;
    state.max_render_scale = 3;
    state.right_stick_choosable = true;

    int filtering = 0;
    int sound = 0;
    int hands = 0;
    int cursor = 0;
    int gap = 0;
    int right_stick = 0;
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
            if (result.screen_gap_toggled) ++gap;
            if (result.right_stick_mode_changed) ++right_stick;
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
    CHECK(gap > 0);
    CHECK(right_stick > 0);
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
    test_the_right_stick_button_appears_only_where_there_is_a_stick();
    test_the_gap_button_appears_only_where_the_layout_offers_it();
    test_choosing_a_gap_layout_reveals_the_button();
    test_toggling_the_gap_updates_what_the_previews_would_draw();
    test_the_other_buttons_still_work();
    test_reset_closes_the_menu();
    return test::finish("quick-settings");
}
