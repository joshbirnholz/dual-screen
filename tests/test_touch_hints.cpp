// The interface a device with no controller is left with.
//
// Every prompt in this app is drawn as a console button: a black disc with a
// letter in it, and the action beside it. That is the right thing when there
// is a pad in somebody's hands and precisely the wrong thing otherwise -- a
// phone has no B button, so "B Back" is an instruction that cannot be
// followed, and the way out of a screen is a picture of a way out.
//
// What is checked here is the part that can be: the arithmetic of which
// prompt a finger landed on, and that every prompt this app draws outside a
// quick menu actually does something when it is tapped. A prompt that draws,
// invites a finger and then does nothing is worse than no prompt at all, and
// on a device with no buttons it is the difference between an app you can
// leave and one you cannot.
//
// What is not checked here is how it looks. That needs a GL context and eyes.

#include "test_support.hpp"

#include <vector>

#include "ui/control_guide.hpp"

using namespace ds;

namespace {

GuideHit hit(ButtonGlyph glyph, float x, float y, float w, float h) {
    return GuideHit{glyph, PixelRect{x, y, w, h}};
}

void test_a_finger_finds_the_prompt_it_landed_on() {
    const std::vector<GuideHit> hits = {
        hit(ButtonGlyph::B, 10.0f, 100.0f, 80.0f, 30.0f),
        hit(ButtonGlyph::A, 110.0f, 100.0f, 80.0f, 30.0f),
    };
    CHECK(guide_button_at(hits, 50.0f, 115.0f) == ButtonGlyph::B);
    CHECK(guide_button_at(hits, 150.0f, 115.0f) == ButtonGlyph::A);
    // The gap between two prompts belongs to neither, or a finger aimed at
    // one would act on whichever happened to be listed first.
    CHECK(guide_button_at(hits, 100.0f, 115.0f) == ButtonGlyph::Count);
    CHECK(guide_button_at(hits, 50.0f, 50.0f) == ButtonGlyph::Count);
    CHECK(guide_button_at({}, 50.0f, 115.0f) == ButtonGlyph::Count);
}

void test_every_prompt_outside_a_menu_does_something() {
    // The glyphs this app puts in front of somebody who is not in the quick
    // menu: the launcher's Quit and Select, the host picker's Connect and
    // Back, the connecting screen's way out, and the way into the menu that
    // a device with no pad is otherwise missing entirely.
    const ButtonGlyph drawn[] = {ButtonGlyph::A, ButtonGlyph::B, ButtonGlyph::Plus,
                                 ButtonGlyph::Minus};
    for (const ButtonGlyph glyph : drawn) {
        UiAction action = UiAction::Confirm;
        CHECK(ui_action_for_glyph(glyph, action));
    }
    // And each one means what the word beside it says.
    UiAction action = UiAction::Confirm;
    CHECK(ui_action_for_glyph(ButtonGlyph::A, action) && action == UiAction::Confirm);
    CHECK(ui_action_for_glyph(ButtonGlyph::B, action) && action == UiAction::Cancel);
    CHECK(ui_action_for_glyph(ButtonGlyph::Plus, action) && action == UiAction::Quit);
    CHECK(ui_action_for_glyph(ButtonGlyph::Minus, action) && action == UiAction::ToggleMenu);
}

// The quick menu's own prompts are deliberately not here: a press on one of
// those is forwarded to the host rather than acted on locally, so a mapping
// for them in this direction would be a second opinion about what they mean.
void test_the_menus_own_prompts_are_left_to_the_menu() {
    UiAction action = UiAction::Confirm;
    CHECK(!ui_action_for_glyph(ButtonGlyph::X, action));
    CHECK(!ui_action_for_glyph(ButtonGlyph::Y, action));
    CHECK(!ui_action_for_glyph(ButtonGlyph::RightStick, action));
}

// A prompt that names a button for something you can simply tap is a second
// way to do what tapping already does -- and the worse of the two, since the
// row or the tile is a bigger target and is where the eye already is.
void test_a_prompt_for_something_tappable_goes_away_on_touch() {
    const std::vector<GuidePrompt> prompts = {
        {ButtonGlyph::Plus, "Quit", false},
        {ButtonGlyph::A, "Select", true},
        {ButtonGlyph::B, "Back", false},
    };
    const bool was = touch_hints();

    // With a pad, something has to say which button opens the thing under the
    // cursor, so all three stay.
    set_touch_hints(false);
    CHECK(prompts_worth_drawing(prompts).size() == 3);

    set_touch_hints(true);
    const std::vector<GuidePrompt> touch = prompts_worth_drawing(prompts);
    CHECK(touch.size() == 2);
    CHECK(touch[0].action == "Quit");
    // Back stays: nothing on the screen is "back", so there is no thing to
    // tap instead.
    CHECK(touch[1].action == "Back");
    set_touch_hints(was);
}

// Not every confirm is redundant. A screen that is only words has no object
// to tap, and the halves of a confirmation dialog are tappable *because* the
// prompts label them.
void test_a_confirm_with_nothing_to_tap_stays() {
    const std::vector<GuidePrompt> prompts = {{ButtonGlyph::A, "Continue", false}};
    const bool was = touch_hints();
    set_touch_hints(true);
    CHECK(prompts_worth_drawing(prompts).size() == 1);
    set_touch_hints(was);
}

void test_the_hint_style_is_asked_rather_than_assumed() {
    const bool was = touch_hints();
    set_touch_hints(true);
    CHECK(touch_hints());
    set_touch_hints(false);
    CHECK(!touch_hints());
    set_touch_hints(was);
}

}  // namespace

int main() {
    test_a_finger_finds_the_prompt_it_landed_on();
    test_every_prompt_outside_a_menu_does_something();
    test_the_menus_own_prompts_are_left_to_the_menu();
    test_a_prompt_for_something_tappable_goes_away_on_touch();
    test_a_confirm_with_nothing_to_tap_stays();
    test_the_hint_style_is_asked_rather_than_assumed();
    return test::finish("touch-hints");
}
