#include "test_support.hpp"

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
    int resume_without_pointer = 0;
    for (float y = 4.0f; y < kHeight; y += 8.0f) {
        for (float x = 4.0f; x < kWidth; x += 8.0f) {
            LayoutPicker offered(registry);
            offered.open(state);
            if (offered.update({}, {tap(x, y)}, kWidth, kHeight).cursor_mode_changed) {
                ++with_pointer;
            }

            LayoutPickerState console = state;
            console.cursor_choosable = false;
            LayoutPicker absent(registry);
            absent.open(console);
            const LayoutPickerResult result = absent.update({}, {tap(x, y)}, kWidth, kHeight);
            if (result.cursor_mode_changed) ++without_pointer;
            if (result.close_requested) ++resume_without_pointer;
        }
    }
    CHECK(with_pointer > 0);
    CHECK_EQ(without_pointer, 0);
    CHECK_EQ(resume_without_pointer, 0);  // nothing in the footer closes the menu any more
}

// Every remaining footer button reports its own intent and no one else's. The
// row is now only the settings that have a value to cycle -- Smoothing, Reset,
// Resume, Quit and Disconnect are face buttons named in the guide instead.
void test_the_other_buttons_still_work() {
    LayoutRegistry registry;
    LayoutPickerState state;
    state.layout_id = "vertical";
    state.session_active = true;
    state.max_render_scale = 3;
    state.right_stick_choosable = true;

    int sound = 0;
    int hands = 0;
    int cursor = 0;
    int gap = 0;
    int right_stick = 0;
    int render_scale = 0;
    int gone = 0;
    for (float y = 4.0f; y < kHeight; y += 8.0f) {
        for (float x = 4.0f; x < kWidth; x += 8.0f) {
            LayoutPicker picker(registry);
            picker.open(state);
            const LayoutPickerResult result = picker.update({}, {tap(x, y)}, kWidth, kHeight);
            if (result.audio_target_changed) ++sound;
            if (result.handedness_changed) ++hands;
            if (result.cursor_mode_changed) ++cursor;
            if (result.screen_gap_toggled) ++gap;
            if (result.right_stick_mode_changed) ++right_stick;
            if (result.render_scale_changed) ++render_scale;
            if (result.filtering_toggled || result.reset_requested || result.quit_requested ||
                result.disconnect_requested || result.close_requested) {
                ++gone;
            }
        }
    }
    CHECK(sound > 0);
    CHECK(hands > 0);
    CHECK(cursor > 0);
    CHECK(gap > 0);
    CHECK(right_stick > 0);
    CHECK(render_scale > 0);
    CHECK_MSG(gone == 0,
              "a tap somewhere in the menu still triggered one of the actions that moved to a "
              "face button");
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
    from_host.handedness = Handedness::Left;
    from_host.bilinear_filtering = true;
    picker.show(from_host, 1, MenuConfirm::None);

    CHECK_STR_EQ(picker.selected_layout_id(), "vertical");
    CHECK(picker.audio_target() == AudioTarget::BottomScreen);
    CHECK(picker.handedness() == Handedness::Left);
    CHECK_EQ(picker.filtering(), true);
    CHECK_EQ(picker.focus(), 1);

    // Including a question the other device put up, and taking it away again.
    picker.show(from_host, 1, MenuConfirm::Quit);
    CHECK(picker.awaiting_confirmation());
    picker.show(from_host, 1, MenuConfirm::None);
    CHECK(!picker.awaiting_confirmation());
}

// The row of buttons has to be rebuilt before the cursor is placed: a focus
// index counts entries, so a cursor sent for the larger row would be refused
// if the row it landed against were still the old one.
void test_entries_are_rebuilt_before_the_cursor_lands() {
    LayoutRegistry registry;
    LayoutPicker picker(registry);

    LayoutPickerState few;
    few.layout_id = registry.at(0).id;
    few.session_active = true;
    picker.open(few);

    // The first index this menu has no entry for, found by asking for each in
    // turn: an index it does not have is ignored, so the cursor stops moving.
    int beyond = 0;
    while (beyond < 64) {
        picker.show(few, beyond, MenuConfirm::None);
        if (picker.focus() != beyond) break;
        ++beyond;
    }
    CHECK_MSG(beyond > 0 && beyond < 64, "could not find the end of the menu's entries");

    LayoutPickerState more = few;
    more.max_render_scale = 3;          // adds the render scale button
    more.right_stick_choosable = true;  // and the right stick one
    picker.show(more, beyond, MenuConfirm::None);
    CHECK_MSG(picker.focus() == beyond,
              "a cursor sent for a menu with more entries landed against the old row");
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
            direct.open(state);
            if (direct.update({}, {tap(x, y)}, kWidth, kHeight).selection_changed) ++aimed;

            // Down in the far corner, up here.
            LayoutPicker onto(registry);
            onto.open(state);
            if (onto.update({}, {slide_onto(kWidth - 2.0f, kHeight - 2.0f, x, y)}, kWidth, kHeight)
                    .selection_changed) {
                ++slid_on;
            }

            // Down here, up in the far corner.
            LayoutPicker away(registry);
            away.open(state);
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
    bool pressed_resume = false;
    bool asked_to_disconnect = false;
    bool asked_about_quitting = false;
    for (float y = 2.0f; y < kHeight; y += 4.0f) {
        for (float x = 2.0f; x < kWidth; x += 4.0f) {
            const ButtonGlyph under = picker.guide_button_at(x, y);
            if (under == ButtonGlyph::Count) continue;

            LayoutPicker fresh(registry);
            fresh.open(state);
            fresh.render(quads, text, glyphs, kWidth, kHeight, "Game");
            const LayoutPickerResult result = fresh.update({}, {tap(x, y)}, kWidth, kHeight);
            if (under == ButtonGlyph::B && result.close_requested) pressed_resume = true;
            if (under == ButtonGlyph::Plus && result.disconnect_requested) {
                asked_to_disconnect = true;
            }
            if (under == ButtonGlyph::X && fresh.awaiting_confirmation()) {
                asked_about_quitting = true;
            }
        }
    }
    CHECK_MSG(pressed_resume, "tapping Resume did not close the menu");
    CHECK_MSG(asked_to_disconnect, "tapping Disconnect did not end the session");
    CHECK_MSG(asked_about_quitting, "tapping Quit did not ask before quitting");
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

    // The last entry the cursor can reach counts the entries there are.
    const auto last_entry = [&] {
        int index = 0;
        while (index < 64) {
            picker.set_focus(index);
            if (picker.focus() != index) break;
            ++index;
        }
        return index;
    };
    const int alone_entries = last_entry();

    picker.set_session_active(true);
    CHECK_MSG(last_entry() > alone_entries,
              "a companion arriving did not add the entries that come with it");
    CHECK_MSG(picker.update({UiAction::MenuDisconnect}, {}, kWidth, kHeight).disconnect_requested,
              "the menu would not disconnect from a device that had just arrived");

    picker.set_session_active(false);
    CHECK_MSG(last_entry() == alone_entries,
              "a companion leaving did not take its entries away again");
}

}  // namespace

int main() {
    test_the_render_button_appears_where_there_is_a_scale_to_choose();
    test_the_right_stick_button_appears_only_where_there_is_a_stick();
    test_the_pointer_button_appears_only_where_there_is_a_pointer();
    test_the_gap_button_appears_only_where_the_layout_offers_it();
    test_choosing_a_gap_layout_reveals_the_button();
    test_toggling_the_gap_updates_what_the_previews_would_draw();
    test_the_other_buttons_still_work();
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
