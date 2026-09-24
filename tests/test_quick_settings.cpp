#include "test_support.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "core/system.hpp"
#include "layout/layout_registry.hpp"
#include "render/button_glyphs.hpp"
#include "ui/layout_picker.hpp"

using namespace ds;

namespace {

constexpr float kWidth = 1280.0f;
constexpr float kHeight = 800.0f;

// A tap, the way the menu receives one: it acts on the release.
// A finger that came down and lifted in the same place, which is what a tap is.
PointerEvent tap(float x, float y) {
    PointerEvent pointer;
    pointer.x = x;
    pointer.y = y;
    pointer.press_x = x;
    pointer.press_y = y;
    pointer.released = true;
    pointer.normalized = false;
    return pointer;
}

// One that came down somewhere else and lifted here.
PointerEvent slide_onto(float from_x, float from_y, float to_x, float to_y) {
    PointerEvent pointer = tap(to_x, to_y);
    pointer.press_x = from_x;
    pointer.press_y = from_y;
    return pointer;
}

// The menu, opened and with the cursor on one of the sections down its left,
// so what that section shows on the right is there to be tapped. A menu opens
// on Continue; the layouts are under Screen Layout and everything that is a
// setting under Options.
void open_on(LayoutPicker& picker, const LayoutPickerState& state, MenuSection section) {
    picker.open(state);
    picker.set_focus(LayoutPicker::focus_of(section));
}

// Every layout a tap can actually reach on this console's menu. The tiles are
// the picker's own business, so this finds them by touching the whole menu
// rather than by being told where they are -- which is also how it notices a
// tile that stopped being reachable.
std::vector<std::string> layouts_offered_for(const LayoutRegistry& registry,
                                             SystemId system,
                                             const SystemScreens& screens) {
    std::vector<std::string> found;
    // Twice, from two different starting layouts: a tap on the tile already in
    // use changes nothing, so one sweep can never see the layout it began on.
    // Neither has to actually be offered on this console -- opening on one
    // that is not just means focus lands on the first tile instead.
    for (const char* opened_on : {"standard", "vertical"}) {
        LayoutPickerState state;
        state.layout_id = opened_on;
        for (float y = 4.0f; y < kHeight; y += 8.0f) {
            for (float x = 4.0f; x < kWidth; x += 8.0f) {
                LayoutPicker picker(registry);
                picker.set_screens(screens, system);
                open_on(picker, state, MenuSection::Layout);
                const LayoutPickerResult result = picker.update({}, {tap(x, y)}, kWidth, kHeight);
                if (!result.selection_changed) continue;
                const std::string& id = picker.selected_layout_id();
                if (std::find(found.begin(), found.end(), id) == found.end()) {
                    found.push_back(id);
                }
            }
        }
    }
    return found;
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
            open_on(picker, state, MenuSection::Options);
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
            open_on(picker, state, MenuSection::Options);
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
// opened the reset confirmation: Reset is one of the sections down the left,
// and choosing it asks first. Drawn once before being tapped, the same as
// test_tapping_a_control_prompt_presses_its_button, so that nothing along the
// bottom is missed either.
int taps_that_open_reset(const LayoutRegistry& registry, const LayoutPickerState& state) {
    QuadRenderer quads;
    TextRenderer text;
    ButtonGlyphSheet glyphs;
    int hits = 0;
    for (float y = 4.0f; y < kHeight; y += 8.0f) {
        for (float x = 4.0f; x < kWidth; x += 8.0f) {
            LayoutPicker picker(registry);
            picker.open(state);
            picker.render(quads, text, glyphs, kWidth, kHeight, "Game");
            picker.update({}, {tap(x, y)}, kWidth, kHeight);
            // Not awaiting_confirmation(): Quit is a separate, still-offered
            // section, and removing Reset moves it up into Reset's place.
            if (picker.confirmation() == MenuConfirm::Reset) ++hits;
        }
    }
    return hits;
}

// Taps every point on a coarse grid over the menu's Options and reports how
// many of them toggled screen smoothing. See taps_that_open_reset for why each
// picker is rendered before being tapped.
int taps_that_toggle_smoothing(const LayoutRegistry& registry, const LayoutPickerState& state) {
    QuadRenderer quads;
    TextRenderer text;
    ButtonGlyphSheet glyphs;
    int hits = 0;
    for (float y = 4.0f; y < kHeight; y += 8.0f) {
        for (float x = 4.0f; x < kWidth; x += 8.0f) {
            LayoutPicker picker(registry);
            open_on(picker, state, MenuSection::Options);
            picker.render(quads, text, glyphs, kWidth, kHeight, "Game");
            const LayoutPickerResult result = picker.update({}, {tap(x, y)}, kWidth, kHeight);
            if (result.filtering_toggled) ++hits;
        }
    }
    return hits;
}

// Reset and Screen Smoothing are Cemu's to own for a Wii U -- see
// SystemProfile::offers_reset and offers_smoothing -- so neither is offered
// there, on a tap or on the button itself. Every other console keeps both,
// unaffected.
void test_reset_and_smoothing_are_hidden_where_the_console_owns_them() {
    LayoutRegistry registry;

    LayoutPickerState wii_u;
    wii_u.layout_id = registry.at(0).id;
    wii_u.reset_available = system_profile(SystemId::WiiU).offers_reset;
    wii_u.smoothing_available = system_profile(SystemId::WiiU).offers_smoothing;
    CHECK(!wii_u.reset_available);
    CHECK(!wii_u.smoothing_available);
    CHECK_EQ(taps_that_open_reset(registry, wii_u), 0);
    CHECK_EQ(taps_that_toggle_smoothing(registry, wii_u), 0);

    LayoutPicker pressing_y(registry);
    pressing_y.open(wii_u);
    const LayoutPickerResult pressed =
        pressing_y.update({UiAction::MenuReset}, {}, kWidth, kHeight);
    CHECK_MSG(!pressed.reset_requested && !pressing_y.awaiting_confirmation(),
              "Y still opened the reset confirmation on a console that owns reset itself");

    LayoutPickerState ds;
    ds.layout_id = registry.at(0).id;
    ds.reset_available = system_profile(SystemId::Ds).offers_reset;
    ds.smoothing_available = system_profile(SystemId::Ds).offers_smoothing;
    CHECK(ds.reset_available);
    CHECK(ds.smoothing_available);
    CHECK(taps_that_open_reset(registry, ds) > 0);
    CHECK(taps_that_toggle_smoothing(registry, ds) > 0);

    LayoutPickerState three_ds;
    three_ds.layout_id = registry.at(0).id;
    three_ds.reset_available = system_profile(SystemId::ThreeDs).offers_reset;
    three_ds.smoothing_available = system_profile(SystemId::ThreeDs).offers_smoothing;
    CHECK(three_ds.reset_available);
    CHECK(three_ds.smoothing_available);
    CHECK(taps_that_open_reset(registry, three_ds) > 0);
    CHECK(taps_that_toggle_smoothing(registry, three_ds) > 0);
}

// Taps every point on a coarse grid over the menu and reports how many of them
// toggled the screen gap.
int taps_that_toggle_the_gap(const LayoutRegistry& registry, const LayoutPickerState& state) {
    int hits = 0;
    for (float y = 4.0f; y < kHeight; y += 8.0f) {
        for (float x = 4.0f; x < kWidth; x += 8.0f) {
            LayoutPicker picker(registry);
            open_on(picker, state, MenuSection::Options);
            const LayoutPickerResult result = picker.update({}, {tap(x, y)}, kWidth, kHeight);
            if (result.screen_gap_toggled) ++hits;
        }
    }
    return hits;
}

// Screen Gap is one choice for every layout that has a gap -- Vertical
// Screens and the two focus layouts -- so it is under Options whichever layout
// is in use, on either device: set it from Standard and it waits there for
// the layouts it applies to.
void test_the_gap_option_is_always_offered() {
    LayoutRegistry registry;
    for (const char* id : {"vertical", "upper_focus", "touch_focus", "side_by_side", "standard",
                           "upper_on_tv"}) {
        LayoutPickerState state;
        state.layout_id = id;
        CHECK_MSG(taps_that_toggle_the_gap(registry, state) > 0,
                  std::string(id) + " should offer Screen Gap");
        LayoutPickerState companion = state;
        companion.session_active = true;
        CHECK_MSG(taps_that_toggle_the_gap(registry, companion) > 0,
                  std::string(id) + " (companion) should offer Screen Gap");
    }
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

// A device with no pointer does not get the Pointer entry: it is the one thing
// on this menu that belongs to the device rather than the host, and a console
// has no arrow to show or hide. Everything else stays exactly where it was.
void test_the_pointer_button_appears_only_where_there_is_a_pointer() {
    LayoutRegistry registry;
    LayoutPickerState state;
    state.layout_id = registry.at(0).id;
    state.session_active = true;

    int with_pointer = 0;
    int without_pointer = 0;
    for (float y = 4.0f; y < kHeight; y += 8.0f) {
        for (float x = 4.0f; x < kWidth; x += 8.0f) {
            LayoutPicker offered(registry);
            open_on(offered, state, MenuSection::Options);
            if (offered.update({}, {tap(x, y)}, kWidth, kHeight).cursor_mode_changed) {
                ++with_pointer;
            }

            LayoutPickerState console = state;
            console.cursor_choosable = false;
            LayoutPicker absent(registry);
            open_on(absent, console, MenuSection::Options);
            const LayoutPickerResult result = absent.update({}, {tap(x, y)}, kWidth, kHeight);
            if (result.cursor_mode_changed) ++without_pointer;
        }
    }
    CHECK(with_pointer > 0);
    CHECK_EQ(without_pointer, 0);
}

// Every option reports its own intent and no one else's, and one tap is never
// two things at once.
void test_the_other_buttons_still_work() {
    LayoutRegistry registry;
    LayoutPickerState state;
    state.layout_id = "vertical";
    state.session_active = true;
    state.max_render_scale = 3;
    state.right_stick_choosable = true;

    int sound = 0;
    int cursor = 0;
    int gap = 0;
    int right_stick = 0;
    int render_scale = 0;
    int smoothing = 0;
    int doubled = 0;
    for (float y = 4.0f; y < kHeight; y += 8.0f) {
        for (float x = 4.0f; x < kWidth; x += 8.0f) {
            LayoutPicker picker(registry);
            open_on(picker, state, MenuSection::Options);
            const LayoutPickerResult result = picker.update({}, {tap(x, y)}, kWidth, kHeight);
            const int intents = result.audio_target_changed + result.handedness_changed +
                                result.cursor_mode_changed + result.screen_gap_toggled +
                                result.right_stick_mode_changed + result.render_scale_changed +
                                result.filtering_toggled + result.disconnect_requested +
                                result.close_requested + result.quit_requested +
                                result.reset_requested + result.selection_changed;
            if (intents > 1) ++doubled;
            if (result.audio_target_changed) ++sound;
            if (result.cursor_mode_changed) ++cursor;
            if (result.screen_gap_toggled) ++gap;
            if (result.right_stick_mode_changed) ++right_stick;
            if (result.render_scale_changed) ++render_scale;
            if (result.filtering_toggled) ++smoothing;
        }
    }
    CHECK(sound > 0);
    CHECK(cursor > 0);
    CHECK(gap > 0);
    CHECK(right_stick > 0);
    CHECK(render_scale > 0);
    CHECK(smoothing > 0);
    CHECK_MSG(doubled == 0, "a single tap on the menu did two things at once");
}

// The sections down the left, in the order they are listed: what the game
// looks like, saving and loading, the options, and last the three that end
// something. Each appears only where it means something.
void test_the_sections_come_in_order() {
    LayoutRegistry registry;
    LayoutPickerState state;
    state.layout_id = "standard";
    state.session_active = true;
    state.save_points_available = true;

    LayoutPicker picker(registry);
    picker.open(state);
    // The list, at its top, the way NSO's menu opens.
    CHECK(picker.section() == MenuSection::Continue);
    CHECK(!picker.on_content());

    const MenuSection expected[] = {MenuSection::Continue,      MenuSection::Layout,
                                    MenuSection::CreateSavePoint, MenuSection::LoadSavePoint,
                                    MenuSection::Options,       MenuSection::Disconnect,
                                    MenuSection::Reset,         MenuSection::Quit};
    for (const MenuSection section : expected) {
        CHECK_MSG(picker.section() == section, "the sections are not in the order they should be");
        picker.update({UiAction::Down}, {}, kWidth, kHeight);
    }
    // And round again from the bottom.
    CHECK(picker.section() == MenuSection::Continue);

    // Without save points, a second device or a reset of our own, those go.
    LayoutPickerState bare = state;
    bare.session_active = false;
    bare.save_points_available = false;
    bare.reset_available = false;
    LayoutPicker lean(registry);
    lean.open(bare);
    for (const MenuSection gone : {MenuSection::CreateSavePoint, MenuSection::LoadSavePoint,
                                   MenuSection::Disconnect, MenuSection::Reset}) {
        lean.set_focus(LayoutPicker::focus_of(gone));
        CHECK_MSG(lean.section() == MenuSection::Continue,
                  "a section that should not be there could still be reached");
    }
    lean.update({UiAction::Down}, {}, kWidth, kHeight);
    CHECK(lean.section() == MenuSection::Layout);
    lean.update({UiAction::Down}, {}, kWidth, kHeight);
    CHECK(lean.section() == MenuSection::Options);
    lean.update({UiAction::Down}, {}, kWidth, kHeight);
    CHECK(lean.section() == MenuSection::Quit);
}

// Moving between the list and what it shows: right (or A) goes across, onto
// the layout in use; left from the grid's edge, or B, comes back; B on the
// list closes the menu.
void test_moving_between_the_list_and_what_it_shows() {
    LayoutRegistry registry;
    LayoutPickerState state;
    state.layout_id = "vertical";

    LayoutPicker picker(registry);
    open_on(picker, state, MenuSection::Layout);
    picker.update({UiAction::Right}, {}, kWidth, kHeight);
    CHECK(picker.on_content());
    CHECK(picker.section() == MenuSection::Layout);

    // Onto the layout in use, and back off the grid's left-hand edge.
    const int landed = picker.focus();
    for (int step = 0; step < 4; ++step) picker.update({UiAction::Left}, {}, kWidth, kHeight);
    CHECK(!picker.on_content());

    // A goes across too, to the same place; A there picks a layout without
    // leaving the menu.
    picker.update({UiAction::Confirm}, {}, kWidth, kHeight);
    CHECK(picker.on_content());
    CHECK_EQ(picker.focus(), landed);
    for (const UiAction move : {UiAction::Right, UiAction::Down, UiAction::Left}) {
        picker.update({move}, {}, kWidth, kHeight);
        if (picker.focus() != landed) break;
    }
    CHECK(picker.focus() != landed);
    const LayoutPickerResult picked = picker.update({UiAction::Confirm}, {}, kWidth, kHeight);
    CHECK(picked.selection_changed);
    CHECK(picker.is_open());

    // B backs out one step at a time.
    CHECK(!picker.update({UiAction::Cancel}, {}, kWidth, kHeight).close_requested);
    CHECK(!picker.on_content());
    CHECK(picker.update({UiAction::Cancel}, {}, kWidth, kHeight).close_requested);

    // Options: A on a row cycles it and stays there.
    LayoutPicker options(registry);
    open_on(options, state, MenuSection::Options);
    options.update({UiAction::Confirm}, {}, kWidth, kHeight);
    CHECK(options.on_content());
    const LayoutPickerResult cycled = options.update({UiAction::Confirm}, {}, kWidth, kHeight);
    CHECK(cycled.cursor_mode_changed);
    CHECK(options.on_content());
}

// Each layout is its own way round: flipping one leaves every other preview
// as it was.
void test_hands_are_remembered_per_layout() {
    LayoutRegistry registry;
    LayoutPickerState state;
    state.layout_id = "upper_focus";
    state.left_handed_layouts = {"upper_focus"};
    LayoutPicker picker(registry);
    picker.open(state);

    const Layout& upper = *registry.find("upper_focus");
    const Layout& touch = *registry.find("touch_focus");
    // Both put their two screens side by side, so mirroring one is running
    // them the other way along the row.
    const Layout flipped = picker.resolved_layout_for_preview(upper);
    const Layout as_defined = picker.resolved_layout_for_preview(touch);
    CHECK(upper.composition.items.size() == 2 && touch.composition.items.size() == 2);
    CHECK_MSG(flipped.composition.items[0].screen == upper.composition.items[1].screen,
              "the left-handed layout's preview was not mirrored");
    CHECK_MSG(as_defined.composition.items[0].screen == touch.composition.items[0].screen,
              "a layout nobody flipped was mirrored along with the one that was");
}

// Slots 1 and 3 filled, 3 the newer.
std::vector<SavePointInfo> some_slots() {
    std::vector<SavePointInfo> slots(kSavePointSlots);
    slots[0].exists = true;
    slots[0].saved_at = 100;
    slots[2].exists = true;
    slots[2].saved_at = 200;
    return slots;
}

LayoutPickerState with_save_points() {
    LayoutPickerState state;
    state.layout_id = "standard";
    state.save_points_available = true;
    return state;
}

LayoutPickerResult press(LayoutPicker& picker, UiAction action) {
    return picker.update({action}, {}, kWidth, kHeight);
}

// Which hand holds the stylus is a round button under the layouts, not an
// option: down from the grid's bottom row, A swaps hands and the cursor stays
// there. It is passed over, and a tap on it does nothing, while the layout in
// use has no hand to choose.
void test_the_stylus_button() {
    LayoutRegistry registry;
    for (const char* id : {"upper_focus", "touch_focus", "side_by_side", "vertical", "standard"}) {
        const bool handed = std::string(id) != "vertical" && std::string(id) != "standard";
        LayoutPickerState state;
        state.layout_id = id;

        int swapped = 0;
        for (float y = 4.0f; y < kHeight; y += 8.0f) {
            for (float x = 4.0f; x < kWidth; x += 8.0f) {
                LayoutPicker picker(registry);
                open_on(picker, state, MenuSection::Layout);
                if (picker.update({}, {tap(x, y)}, kWidth, kHeight).handedness_changed) ++swapped;
            }
        }
        CHECK_MSG((swapped > 0) == handed,
                  std::string(id) + (handed ? " offered no stylus button"
                                            : " offered a stylus button with no hand to choose"));

        // With the pad: across onto the layouts, then down as far as it goes.
        LayoutPicker picker(registry);
        open_on(picker, state, MenuSection::Layout);
        press(picker, UiAction::Right);
        for (int step = 0; step < 6; ++step) press(picker, UiAction::Down);
        const int bottom = picker.focus();
        const LayoutPickerResult pressed = press(picker, UiAction::Confirm);
        CHECK_EQ(pressed.handedness_changed, handed);
        CHECK(!pressed.selection_changed || !handed);
        if (handed) {
            CHECK_EQ(picker.focus(), bottom);
            // And up again lands back on a layout.
            press(picker, UiAction::Up);
            CHECK(!press(picker, UiAction::Confirm).handedness_changed);
        }
    }
}

// "Load this save state?" -- asked every time, answered no by Back, and not
// asked at all of an empty slot, which has nothing to load.
void test_loading_a_save_point_asks_first() {
    LayoutRegistry registry;
    LayoutPicker picker(registry);
    picker.set_save_slots(some_slots());
    open_on(picker, with_save_points(), MenuSection::LoadSavePoint);

    // Across onto the newest save.
    press(picker, UiAction::Confirm);
    CHECK_EQ(picker.focus(), LayoutPicker::focus_of(MenuSection::LoadSavePoint, 2));

    CHECK_EQ(press(picker, UiAction::Confirm).load_slot, -1);
    CHECK(picker.confirmation() == MenuConfirm::LoadSavePoint);
    CHECK_EQ(press(picker, UiAction::Cancel).load_slot, -1);
    CHECK(!picker.awaiting_confirmation());
    CHECK(picker.is_open());

    press(picker, UiAction::Confirm);
    CHECK_EQ(press(picker, UiAction::Confirm).load_slot, 2);

    press(picker, UiAction::Down);
    CHECK_EQ(picker.focus(), LayoutPicker::focus_of(MenuSection::LoadSavePoint, 3));
    CHECK_EQ(press(picker, UiAction::Confirm).load_slot, -1);
    CHECK(!picker.awaiting_confirmation());
}

// Saving into an empty slot loses nothing and is not asked about; saving over
// one asks "Overwrite this save point?" first.
void test_saving_asks_only_before_overwriting() {
    LayoutRegistry registry;
    LayoutPicker picker(registry);
    picker.set_save_slots(some_slots());
    open_on(picker, with_save_points(), MenuSection::CreateSavePoint);

    // Across onto the first empty slot.
    press(picker, UiAction::Right);
    CHECK_EQ(picker.focus(), LayoutPicker::focus_of(MenuSection::CreateSavePoint, 1));
    CHECK_EQ(press(picker, UiAction::Confirm).save_slot, 1);
    CHECK(!picker.awaiting_confirmation());
    // Still there afterwards, to see it saved.
    CHECK(picker.is_open());
    CHECK(picker.on_content());

    press(picker, UiAction::Up);
    CHECK_EQ(press(picker, UiAction::Confirm).save_slot, -1);
    CHECK(picker.confirmation() == MenuConfirm::OverwriteSavePoint);
    CHECK_EQ(press(picker, UiAction::Confirm).save_slot, 0);
    CHECK(!picker.awaiting_confirmation());

    // Every slot full: the oldest is where the cursor lands.
    std::vector<SavePointInfo> full = some_slots();
    full[1].exists = true;
    full[1].saved_at = 50;
    full[3].exists = true;
    full[3].saved_at = 300;
    LayoutPicker filled(registry);
    filled.set_save_slots(full);
    open_on(filled, with_save_points(), MenuSection::CreateSavePoint);
    press(filled, UiAction::Right);
    CHECK_EQ(filled.focus(), LayoutPicker::focus_of(MenuSection::CreateSavePoint, 1));

    // And a finger: a tap on a filled slot asks too.
    int saved_at_once = 0;
    int asked = 0;
    for (float y = 4.0f; y < kHeight; y += 8.0f) {
        for (float x = 4.0f; x < kWidth; x += 8.0f) {
            LayoutPicker tapped(registry);
            tapped.set_save_slots(full);
            open_on(tapped, with_save_points(), MenuSection::CreateSavePoint);
            if (tapped.update({}, {tap(x, y)}, kWidth, kHeight).save_slot >= 0) ++saved_at_once;
            if (tapped.confirmation() == MenuConfirm::OverwriteSavePoint) ++asked;
        }
    }
    CHECK_EQ(saved_at_once, 0);
    CHECK(asked > 0);
}

// No save points where the core cannot make them: neither section, on a tap
// or a press.
void test_save_points_only_where_the_core_can_make_them() {
    LayoutRegistry registry;
    LayoutPickerState state;
    state.layout_id = "standard";
    int asked = 0;
    for (float y = 4.0f; y < kHeight; y += 8.0f) {
        for (float x = 4.0f; x < kWidth; x += 8.0f) {
            for (const MenuSection section :
                 {MenuSection::CreateSavePoint, MenuSection::LoadSavePoint}) {
                LayoutPicker picker(registry);
                picker.set_save_slots(some_slots());
                open_on(picker, state, section);
                const LayoutPickerResult result = picker.update({}, {tap(x, y)}, kWidth, kHeight);
                if (result.save_slot >= 0 || result.load_slot >= 0 ||
                    picker.confirmation() == MenuConfirm::LoadSavePoint ||
                    picker.confirmation() == MenuConfirm::OverwriteSavePoint) {
                    ++asked;
                }
            }
        }
    }
    CHECK_EQ(asked, 0);
}

// Continue, Disconnect, Reset Game and Quit are sections of their own, and
// choosing one is the same as the button that used to be it.
void test_the_sections_that_end_something() {
    LayoutRegistry registry;
    LayoutPickerState state;
    state.layout_id = "standard";
    state.session_active = true;

    // Continue is the way back into the game, the same as B on the list.
    LayoutPicker resume(registry);
    resume.open(state);
    CHECK(resume.section() == MenuSection::Continue);
    CHECK(press(resume, UiAction::Confirm).close_requested);

    LayoutPicker disconnect(registry);
    open_on(disconnect, state, MenuSection::Disconnect);
    CHECK(press(disconnect, UiAction::Confirm).disconnect_requested);

    LayoutPicker reset(registry);
    open_on(reset, state, MenuSection::Reset);
    CHECK(!press(reset, UiAction::Confirm).reset_requested);
    CHECK(reset.confirmation() == MenuConfirm::Reset);
    CHECK(press(reset, UiAction::Confirm).reset_requested);

    LayoutPicker quit(registry);
    open_on(quit, state, MenuSection::Quit);
    CHECK(!press(quit, UiAction::Confirm).quit_requested);
    CHECK(quit.confirmation() == MenuConfirm::Quit);
    CHECK(press(quit, UiAction::Confirm).quit_requested);

    // None of them has anything to go across to.
    LayoutPicker across(registry);
    open_on(across, state, MenuSection::Quit);
    press(across, UiAction::Right);
    CHECK(!across.on_content());
}

// A Wii U's quick menu is a plain list -- Controllers, Settings, Disconnect
// (only with a companion attached), Quit -- and nothing else, not even Resume,
// which Back already is: no layout tiles (see
// test_a_console_with_no_shape_of_its_own_is_not_offered_its_own_shape), no
// audio, hands, pointer, right-stick or render-scale buttons, and no
// face-button shortcuts standing in for any of them. Which screen is shown
// was decided once, at connect; everything else here now lives in Settings.
void test_wii_u_menu_is_a_plain_list() {
    LayoutRegistry registry;
    LayoutPickerState state;
    state.layout_id = registry.at(0).id;

    for (const bool session_active : {false, true}) {
        state.session_active = session_active;

        int controllers = 0, closed = 0, settings = 0, disconnect = 0, quit = 0;
        int anything_else = 0;
        for (float y = 4.0f; y < kHeight; y += 8.0f) {
            for (float x = 4.0f; x < kWidth; x += 8.0f) {
                LayoutPicker picker(registry);
                picker.set_screens(system_profile(SystemId::WiiU).screens, SystemId::WiiU);
                picker.open(state);
                const LayoutPickerResult result = picker.update({}, {tap(x, y)}, kWidth, kHeight);
                if (result.close_requested) ++closed;
                if (result.controllers_requested) ++controllers;
                if (result.settings_requested) ++settings;
                if (result.disconnect_requested) ++disconnect;
                if (picker.awaiting_confirmation()) ++quit;
                if (result.selection_changed || result.audio_target_changed ||
                    result.handedness_changed || result.cursor_mode_changed ||
                    result.render_scale_changed || result.right_stick_mode_changed ||
                    result.screen_gap_toggled || result.filtering_toggled ||
                    result.reset_requested) {
                    ++anything_else;
                }
            }
        }
        CHECK_MSG(controllers > 0, "no tap reached Controllers on a Wii U's menu");
        CHECK_MSG(closed == 0, "a Wii U's menu still has a row that only closes it");
        CHECK_MSG(settings > 0, "no tap reached Settings on a Wii U's menu");
        CHECK_MSG(quit > 0, "no tap reached Quit on a Wii U's menu");
        CHECK_MSG(session_active == (disconnect > 0),
                  "Disconnect did not follow whether a companion is attached");
        CHECK_MSG(anything_else == 0,
                  "a Wii U's menu still offered something outside its plain list");
    }

    // The shortcuts every other console's menu answers to are not offered
    // here either -- see update()'s Wii U branch -- so a press that arrives
    // anyway (a shared keybinding, a macro) does nothing rather than reaching
    // past the list.
    state.session_active = true;
    LayoutPicker shortcuts(registry);
    shortcuts.set_screens(system_profile(SystemId::WiiU).screens, SystemId::WiiU);
    shortcuts.open(state);
    for (const UiAction action :
         {UiAction::MenuQuit, UiAction::MenuDisconnect, UiAction::MenuSettings}) {
        const LayoutPickerResult result = shortcuts.update({action}, {}, kWidth, kHeight);
        CHECK_MSG(!result.settings_requested && !result.disconnect_requested &&
                      !shortcuts.awaiting_confirmation(),
                  "a face-button shortcut still worked on a Wii U's plain-list menu");
    }

    // And the reverse: a DS keeps every one of its own buttons and never
    // shrinks to the Wii U's plain list.
    int sound = 0;
    for (float y = 4.0f; y < kHeight; y += 8.0f) {
        for (float x = 4.0f; x < kWidth; x += 8.0f) {
            LayoutPicker picker(registry);
            picker.set_screens(system_profile(SystemId::Ds).screens, SystemId::Ds);
            open_on(picker, state, MenuSection::Options);
            if (picker.update({}, {tap(x, y)}, kWidth, kHeight).audio_target_changed) ++sound;
        }
    }
    CHECK(sound > 0);

    // Shown in place of the waiting screen rather than over a paused game,
    // Back has nothing to close.
    state.embedded = true;
    LayoutPicker embedded(registry);
    embedded.set_screens(system_profile(SystemId::WiiU).screens, SystemId::WiiU);
    embedded.open(state);
    CHECK(embedded.embedded());
    for (const UiAction action : {UiAction::Cancel, UiAction::ToggleMenu}) {
        CHECK_MSG(!embedded.update({action}, {}, kWidth, kHeight).close_requested,
                  "Back closed a menu that is the screen itself");
    }
    CHECK(embedded.update({UiAction::Confirm}, {}, kWidth, kHeight).controllers_requested);
}

// The control guide is what actually tells a player what a press does, so the
// list has to shrink there too: A and B only, nothing named for a control
// that no longer does anything on this menu.
void test_wii_u_menu_guide_is_just_ok_and_resume() {
    LayoutRegistry registry;
    LayoutPickerState state;
    state.layout_id = registry.at(0).id;
    state.session_active = true;

    LayoutPicker picker(registry);
    picker.set_screens(system_profile(SystemId::WiiU).screens, SystemId::WiiU);
    picker.open(state);

    QuadRenderer quads;
    TextRenderer text;
    ButtonGlyphSheet glyphs;
    picker.render(quads, text, glyphs, kWidth, kHeight, "Game");

    bool found_a = false;
    bool found_b = false;
    bool found_other = false;
    for (float y = 2.0f; y < kHeight; y += 4.0f) {
        for (float x = 2.0f; x < kWidth; x += 4.0f) {
            switch (picker.guide_button_at(x, y)) {
                case ButtonGlyph::A: found_a = true; break;
                case ButtonGlyph::B: found_b = true; break;
                case ButtonGlyph::Count: break;
                default: found_other = true; break;
            }
        }
    }
    CHECK_MSG(found_a, "a Wii U's menu guide is missing A");
    CHECK_MSG(found_b, "a Wii U's menu guide is missing B");
    CHECK_MSG(!found_other, "a Wii U's menu guide still names a control beyond A and B");
}

// Reset and Quit throw away something the player cannot get back, so neither
// happens on the press that asked for it.
void test_reset_and_quit_are_asked_about_first() {
    LayoutRegistry registry;
    LayoutPickerState state;
    state.layout_id = registry.at(0).id;

    for (const UiAction asked : {UiAction::MenuReset, UiAction::MenuQuit}) {
        LayoutPicker picker(registry);
        picker.open(state);

        const LayoutPickerResult raised = picker.update({asked}, {}, kWidth, kHeight);
        CHECK_MSG(!raised.reset_requested && !raised.quit_requested,
                  "a single press did the thing it was only supposed to ask about");
        CHECK_MSG(picker.awaiting_confirmation(), "nothing was asked before acting");
        CHECK_MSG(picker.is_open(), "the menu closed while it was still asking a question");

        const LayoutPickerResult answered =
            picker.update({UiAction::Confirm}, {}, kWidth, kHeight);
        const bool acted = asked == UiAction::MenuReset ? answered.reset_requested
                                                        : answered.quit_requested;
        CHECK_MSG(acted, "confirming did not carry out what was confirmed");
        CHECK_MSG(!picker.awaiting_confirmation(), "the question stayed up after being answered");
    }
}

void test_a_confirmation_can_be_backed_out_of() {
    LayoutRegistry registry;
    LayoutPickerState state;
    state.layout_id = registry.at(0).id;

    LayoutPicker picker(registry);
    picker.open(state);
    picker.update({UiAction::MenuQuit}, {}, kWidth, kHeight);
    CHECK(picker.awaiting_confirmation());

    const LayoutPickerResult backed = picker.update({UiAction::Cancel}, {}, kWidth, kHeight);
    CHECK_MSG(!backed.quit_requested, "backing out of the question quit anyway");
    CHECK_MSG(!backed.close_requested, "backing out of the question also closed the menu");
    CHECK_MSG(!picker.awaiting_confirmation(), "the question stayed up after being dismissed");
    CHECK_MSG(picker.is_open(), "the menu did not survive a dismissed question");
}

// While a question is up it is the only thing on screen, so nothing behind it
// moves, cycles or opens -- including a finger landing on a button.
void test_nothing_else_answers_while_a_confirmation_is_up() {
    LayoutRegistry registry;
    LayoutPickerState state;
    state.layout_id = registry.at(0).id;
    state.session_active = true;
    state.max_render_scale = 3;

    for (float y = 4.0f; y < kHeight; y += 8.0f) {
        for (float x = 4.0f; x < kWidth; x += 8.0f) {
            LayoutPicker picker(registry);
            picker.open(state);
            picker.update({UiAction::MenuReset}, {}, kWidth, kHeight);
            const LayoutPickerResult result =
                picker.update({UiAction::Right, UiAction::Down, UiAction::MenuDisconnect},
                              {tap(x, y)}, kWidth, kHeight);
            CHECK_MSG(!result.selection_changed && !result.audio_target_changed &&
                          !result.handedness_changed && !result.cursor_mode_changed &&
                          !result.screen_gap_toggled && !result.disconnect_requested &&
                          !result.render_scale_changed,
                      "the menu under a confirmation still answered a press");
        }
    }
}

void test_disconnect_is_the_plus_button_and_only_in_a_session() {
    LayoutRegistry registry;
    LayoutPickerState alone;
    alone.layout_id = registry.at(0).id;

    LayoutPicker solo(registry);
    solo.open(alone);
    CHECK_MSG(!solo.update({UiAction::MenuDisconnect}, {}, kWidth, kHeight).disconnect_requested,
              "a device with nothing connected to it offered to disconnect");

    LayoutPickerState paired = alone;
    paired.session_active = true;
    LayoutPicker picker(registry);
    picker.open(paired);
    CHECK_MSG(picker.update({UiAction::MenuDisconnect}, {}, kWidth, kHeight).disconnect_requested,
              "plus did not end the session");

    // And the menu is still standing afterwards. Whether it goes away is the
    // app's call -- it stays up when the other device was the one that left --
    // so a menu that put itself away here would leave that decision with
    // nothing behind it: a blurred game, no menu on top of it, and no press
    // left that anything would answer.
    CHECK_MSG(picker.is_open(),
              "the menu closed itself on the way out of a session, leaving nothing to draw and "
              "nothing to press");
    CHECK_MSG(picker.update({UiAction::Cancel}, {}, kWidth, kHeight).close_requested,
              "the menu had stopped answering presses after the session ended");
}

// The menu belongs to whichever device runs the game. The other one holds no
// copy of it: it is handed the whole thing every frame and shows that, which is
// the only arrangement where the two cannot end up disagreeing.
void test_a_shown_menu_takes_everything_it_is_given() {
    LayoutRegistry registry;
    LayoutPicker picker(registry);

    LayoutPickerState opened;
    opened.layout_id = registry.at(0).id;
    opened.session_active = true;
    picker.open(opened);

    LayoutPickerState from_host = opened;
    from_host.layout_id = "vertical";
    from_host.audio_target = AudioTarget::BottomScreen;
    from_host.left_handed_layouts = {"upper_focus"};
    from_host.bilinear_filtering = true;
    const int focus = LayoutPicker::focus_of(MenuSection::Options, 1);
    picker.show(from_host, focus, MenuConfirm::None);

    CHECK_STR_EQ(picker.selected_layout_id(), "vertical");
    CHECK(picker.audio_target() == AudioTarget::BottomScreen);
    CHECK(picker.handedness_of("upper_focus") == Handedness::Left);
    CHECK(picker.handedness_of("touch_focus") == Handedness::Right);
    CHECK_EQ(picker.filtering(), true);
    CHECK_EQ(picker.focus(), focus);

    // Including a question the other device put up, and taking it away again.
    picker.show(from_host, focus, MenuConfirm::Quit);
    CHECK(picker.awaiting_confirmation());
    picker.show(from_host, focus, MenuConfirm::None);
    CHECK(!picker.awaiting_confirmation());
}

// The options have to be rebuilt before the cursor is placed: a focus names
// a row, so a cursor sent for the longer list would be refused if the list it
// landed against were still the old one.
void test_entries_are_rebuilt_before_the_cursor_lands() {
    LayoutRegistry registry;
    LayoutPicker picker(registry);

    LayoutPickerState few;
    few.layout_id = registry.at(0).id;
    few.session_active = true;
    picker.open(few);

    // The first row this menu's Options has no entry for, found by asking for
    // each in turn: one it does not have is ignored, so the cursor stops.
    int beyond = 0;
    while (beyond < LayoutPicker::kSectionStride - 1) {
        const int focus = LayoutPicker::focus_of(MenuSection::Options, beyond);
        picker.show(few, focus, MenuConfirm::None);
        if (picker.focus() != focus) break;
        ++beyond;
    }
    CHECK_MSG(beyond > 0 && beyond < LayoutPicker::kSectionStride - 1,
              "could not find the end of the menu's options");

    LayoutPickerState more = few;
    more.max_render_scale = 3;          // adds the render scale option
    more.right_stick_choosable = true;  // and the right stick one
    const int focus = LayoutPicker::focus_of(MenuSection::Options, beyond);
    picker.show(more, focus, MenuConfirm::None);
    CHECK_MSG(picker.focus() == focus,
              "a cursor sent for a menu with more options landed against the old list");
}

// A tap is a press and a release in the same place. A finger that landed
// somewhere else and slid onto a button was not aiming at it, and one that
// landed on a button and slid off was taking it back.
void test_a_button_only_answers_a_tap_aimed_at_it() {
    LayoutRegistry registry;
    LayoutPickerState state;
    state.layout_id = registry.at(0).id;
    state.session_active = true;

    int aimed = 0;
    int slid_on = 0;
    int slid_off = 0;
    for (float y = 4.0f; y < kHeight; y += 8.0f) {
        for (float x = 4.0f; x < kWidth; x += 8.0f) {
            LayoutPicker direct(registry);
            open_on(direct, state, MenuSection::Layout);
            if (direct.update({}, {tap(x, y)}, kWidth, kHeight).selection_changed) ++aimed;

            // Down in the far corner, up here.
            LayoutPicker onto(registry);
            open_on(onto, state, MenuSection::Layout);
            if (onto.update({}, {slide_onto(kWidth - 2.0f, kHeight - 2.0f, x, y)}, kWidth, kHeight)
                    .selection_changed) {
                ++slid_on;
            }

            // Down here, up in the far corner.
            LayoutPicker away(registry);
            open_on(away, state, MenuSection::Layout);
            if (away.update({}, {slide_onto(x, y, kWidth - 2.0f, kHeight - 2.0f)}, kWidth, kHeight)
                    .selection_changed) {
                ++slid_off;
            }
        }
    }
    CHECK_MSG(aimed > 0, "no tap anywhere in the menu picked a layout");
    CHECK_MSG(slid_on == 0,
              std::to_string(slid_on) + " taps that began somewhere else still picked a layout");
    CHECK_MSG(slid_off == 0,
              std::to_string(slid_off) + " taps that were slid away from still picked a layout");
}

// The control prompts along the bottom are buttons too: a finger on one means
// the same as pressing what it shows.
//
// Where they are is decided while the row is drawn, from the glyph artwork's
// own proportions, so the menu is drawn once first. Nothing here has a GL
// context and nothing needs one: every draw is a no-op without a program, and
// the row writes down where its prompts went either way.
void test_tapping_a_control_prompt_presses_its_button() {
    LayoutRegistry registry;
    LayoutPickerState state;
    state.layout_id = registry.at(0).id;
    state.session_active = true;

    LayoutPicker picker(registry);
    picker.open(state);
    CHECK_MSG(picker.guide_button_at(kWidth * 0.5f, kHeight * 0.5f) == ButtonGlyph::Count,
              "a menu that has never been drawn claimed a prompt was under the finger");

    QuadRenderer quads;
    TextRenderer text;
    ButtonGlyphSheet glyphs;
    picker.render(quads, text, glyphs, kWidth, kHeight, "Game");

    // Find each prompt the way a finger does, and check it does what it says.
    // Disconnect, Reset and Quit are in the list now, so the row is only
    // Settings, Back and OK.
    bool pressed_resume = false;
    bool asked_for_settings = false;
    bool found_other = false;
    for (float y = 2.0f; y < kHeight; y += 4.0f) {
        for (float x = 2.0f; x < kWidth; x += 4.0f) {
            const ButtonGlyph under = picker.guide_button_at(x, y);
            if (under == ButtonGlyph::Count) continue;
            if (under != ButtonGlyph::A && under != ButtonGlyph::B &&
                under != ButtonGlyph::Minus) {
                found_other = true;
            }

            LayoutPicker fresh(registry);
            fresh.open(state);
            fresh.render(quads, text, glyphs, kWidth, kHeight, "Game");
            const LayoutPickerResult result = fresh.update({}, {tap(x, y)}, kWidth, kHeight);
            if (under == ButtonGlyph::B && result.close_requested) pressed_resume = true;
            if (under == ButtonGlyph::Minus && result.settings_requested) {
                asked_for_settings = true;
            }
        }
    }
    CHECK_MSG(pressed_resume, "tapping Resume did not close the menu");
    CHECK_MSG(asked_for_settings, "tapping Settings did not ask for them");
    CHECK_MSG(!found_other, "the guide still names a button whose entry moved into the list");
}

// A companion arriving changes what the menu offers, so the row is rebuilt
// rather than left as it was when the menu opened -- two menus with different
// entries put the same cursor on different things.
void test_the_menu_follows_a_device_arriving_and_leaving() {
    LayoutRegistry registry;
    LayoutPickerState alone;
    alone.layout_id = registry.at(0).id;
    alone.session_active = false;

    LayoutPicker picker(registry);
    picker.open(alone);

    // What a companion brings: Disconnect, and Sound under Options.
    const auto options = [&] {
        int count = 0;
        while (count < LayoutPicker::kSectionStride - 1) {
            const int focus = LayoutPicker::focus_of(MenuSection::Options, count);
            picker.set_focus(focus);
            if (picker.focus() != focus) break;
            ++count;
        }
        return count;
    };
    const auto can_disconnect = [&] {
        picker.set_focus(LayoutPicker::focus_of(MenuSection::Disconnect));
        return picker.section() == MenuSection::Disconnect;
    };
    const int alone_options = options();
    CHECK(!can_disconnect());

    picker.set_session_active(true);
    CHECK_MSG(options() > alone_options && can_disconnect(),
              "a companion arriving did not add the entries that come with it");
    CHECK_MSG(picker.update({UiAction::MenuDisconnect}, {}, kWidth, kHeight).disconnect_requested,
              "the menu would not disconnect from a device that had just arrived");

    picker.set_session_active(false);
    CHECK_MSG(options() == alone_options && !can_disconnect(),
              "a companion leaving did not take its entries away again");
}

// The Standard layout is the console's own arrangement, drawn from the shape of
// the console itself. A Wii U is a box under a television and a pad in your
// hands, which is not a shape two screens sit inside -- so there is no Standard
// to offer, and offering it anyway meant a tile that drew nothing and a game
// that started onto a black window.
void test_a_console_with_no_shape_of_its_own_is_not_offered_its_own_shape() {
    LayoutRegistry registry;
    const std::vector<std::string> handheld =
        layouts_offered_for(registry, SystemId::Ds, system_profile(SystemId::Ds).screens);

    const auto offers = [](const std::vector<std::string>& ids, const std::string& id) {
        return std::find(ids.begin(), ids.end(), id) != ids.end();
    };
    CHECK_MSG(offers(handheld, "standard"),
              "a console this app has a picture of stopped offering its own arrangement");

    // Vertical Screens and Side-by-Side stand in for a DS's hinge, so both are
    // offered there.
    for (const char* hinge : {"vertical", "side_by_side"}) {
        CHECK_MSG(offers(handheld, hinge),
                  "a console with a hinge stopped being offered the layout standing in for it");
    }
    // Standing in for a Wii U's screen that is ever the whole of the game
    // rather than half of one, so a console with a hinge has no use for
    // either.
    for (const char* solo : {"tv_only", "gamepad_only"}) {
        CHECK_MSG(!offers(handheld, solo),
                  "a console with no screen that stands alone was offered one anyway");
    }

    // A Wii U itself offers no layout at all any more: which single screen it
    // shows was decided once, at connect, and its menu is the plain list
    // test_wii_u_menu_is_a_plain_list covers instead -- see rebuild_tiles().
    const std::vector<std::string> wii_u =
        layouts_offered_for(registry, SystemId::WiiU, system_profile(SystemId::WiiU).screens);
    CHECK_MSG(wii_u.empty(), "a Wii U's menu still offered a layout to pick");
}

// The settings belong to the machine running the game, and the menu is the one
// place both players can reach from. So the prompt is always there, it is
// leftmost -- it is the only one on that row about the machine rather than
// about the game -- and pressing it asks rather than doing anything itself.
void test_the_settings_prompt_is_there_and_is_leftmost() {
    LayoutRegistry registry;
    LayoutPickerState state;
    state.layout_id = LayoutRegistry::fallback_id();
    // With a second device connected, so the row is as long as it ever gets and
    // "leftmost" is a claim about something rather than about a row of one.
    state.session_active = true;

    // Where each prompt ended up, found by touching the row rather than by
    // being told: the row is laid out from the glyph artwork's own proportions,
    // which only the drawing side knows.
    QuadRenderer quads;
    TextRenderer text;
    ButtonGlyphSheet glyphs;
    LayoutPicker picker(registry);
    picker.open(state);
    picker.render(quads, text, glyphs, kWidth, kHeight, "Game");

    float leftmost_settings = kWidth;
    float leftmost_other = kWidth;
    bool found_settings = false;
    for (float y = 4.0f; y < kHeight; y += 4.0f) {
        for (float x = 4.0f; x < kWidth; x += 4.0f) {
            const ButtonGlyph glyph = picker.guide_button_at(x, y);
            if (glyph == ButtonGlyph::Count) continue;
            if (glyph == ButtonGlyph::Minus) {
                found_settings = true;
                leftmost_settings = std::min(leftmost_settings, x);
            } else {
                leftmost_other = std::min(leftmost_other, x);
            }
        }
    }
    CHECK_MSG(found_settings, "the menu offers no way to reach the settings");
    CHECK_MSG(leftmost_settings < leftmost_other,
              "the settings prompt is not the leftmost one on the row");

    // And it asks rather than answering: the caller puts the screen up, because
    // on a host it is the caller that knows whose settings these are.
    LayoutPicker pressing(registry);
    pressing.open(state);
    const LayoutPickerResult result =
        pressing.update({UiAction::MenuSettings}, {}, kWidth, kHeight);
    CHECK_MSG(result.settings_requested, "pressing - in the menu asked for nothing");
    CHECK_MSG(!result.close_requested,
              "asking for the settings also closed the menu, so there would be nothing to come "
              "back to");
}

}  // namespace

int main() {
    test_the_sections_come_in_order();
    test_moving_between_the_list_and_what_it_shows();
    test_loading_a_save_point_asks_first();
    test_saving_asks_only_before_overwriting();
    test_save_points_only_where_the_core_can_make_them();
    test_the_sections_that_end_something();
    test_the_stylus_button();
    test_hands_are_remembered_per_layout();
    test_a_console_with_no_shape_of_its_own_is_not_offered_its_own_shape();
    test_the_settings_prompt_is_there_and_is_leftmost();
    test_the_render_button_appears_where_there_is_a_scale_to_choose();
    test_the_right_stick_button_appears_only_where_there_is_a_stick();
    test_reset_and_smoothing_are_hidden_where_the_console_owns_them();
    test_the_pointer_button_appears_only_where_there_is_a_pointer();
    test_the_gap_option_is_always_offered();
    test_toggling_the_gap_updates_what_the_previews_would_draw();
    test_the_other_buttons_still_work();
    test_wii_u_menu_is_a_plain_list();
    test_wii_u_menu_guide_is_just_ok_and_resume();
    test_reset_and_quit_are_asked_about_first();
    test_a_confirmation_can_be_backed_out_of();
    test_nothing_else_answers_while_a_confirmation_is_up();
    test_disconnect_is_the_plus_button_and_only_in_a_session();
    test_a_shown_menu_takes_everything_it_is_given();
    test_entries_are_rebuilt_before_the_cursor_lands();
    test_a_button_only_answers_a_tap_aimed_at_it();
    test_tapping_a_control_prompt_presses_its_button();
    test_the_menu_follows_a_device_arriving_and_leaving();
    return test::finish("quick-settings");
}
