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

#include "input/input_manager.hpp"
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
    CHECK(ui_action_for_glyph(ButtonGlyph::Minus, action) && action == UiAction::MenuMinus);
}

// X and Y are what a menu's upper face buttons raise -- Controller Order's
// Identify is X. The stick clicks only ever appear in the quick menu, whose
// prompts route their own taps: a press there is forwarded to the host rather
// than acted on locally, so a mapping for them here would be a second opinion
// about what they mean.
void test_the_menus_own_prompts_are_left_to_the_menu() {
    UiAction action = UiAction::Confirm;
    CHECK(ui_action_for_glyph(ButtonGlyph::X, action) && action == UiAction::MenuQuit);
    CHECK(ui_action_for_glyph(ButtonGlyph::Y, action) && action == UiAction::MenuReset);
    CHECK(!ui_action_for_glyph(ButtonGlyph::RightStick, action));
}

// The row a launcher or a browser draws: one shortcut, one that opens what is
// highlighted, one that leaves.
std::vector<GuidePrompt> a_screens_prompts() {
    return {
        {ButtonGlyph::Plus, "Quit", PromptRole::OwnControl},
        {ButtonGlyph::A, "Select", PromptRole::ActsOnSelection},
        {ButtonGlyph::B, "Back", PromptRole::LeavesScreen},
    };
}

// A prompt that names a button for something you can simply tap is a second
// way to do what tapping already does -- and the worse of the two, since the
// row or the tile is a bigger target and is where the eye already is.
void test_a_prompt_for_something_tappable_goes_away_on_touch() {
    const PromptStyle was = prompt_style();

    // With a pad, something has to say which button opens the thing under the
    // cursor, so all three stay.
    set_prompt_style(PromptStyle::Pad);
    CHECK(prompts_worth_drawing(a_screens_prompts()).size() == 3);

    set_prompt_style(PromptStyle::Touch);
    const std::vector<GuidePrompt> touch = prompts_worth_drawing(a_screens_prompts());
    CHECK(touch.size() == 2);
    CHECK(touch[0].action == "Quit");
    // Back stays: nothing on the screen is "back", so there is no thing to
    // tap instead.
    CHECK(touch[1].action == "Back");
    set_prompt_style(was);
}

// A Siri Remote has a click and a back button, and nobody needs telling what
// either one does. It does not have a Quit button, so that prompt stays.
void test_select_and_back_go_away_on_a_remote() {
    const PromptStyle was = prompt_style();
    set_prompt_style(PromptStyle::Remote);
    const std::vector<GuidePrompt> remote = prompts_worth_drawing(a_screens_prompts());
    CHECK(remote.size() == 1);
    CHECK(remote[0].action == "Quit");
    set_prompt_style(was);
}

// Not every confirm is redundant. A screen that is only words has no object
// to tap, and the halves of a confirmation dialog are tappable *because* the
// prompts label them.
void test_a_confirm_with_nothing_to_tap_stays() {
    const std::vector<GuidePrompt> prompts = {{ButtonGlyph::A, "Continue"}};
    const PromptStyle was = prompt_style();
    for (const PromptStyle style : {PromptStyle::Touch, PromptStyle::Remote}) {
        set_prompt_style(style);
        CHECK(prompts_worth_drawing(prompts).size() == 1);
    }
    set_prompt_style(was);
}

// The destructive one. "Are you sure you want to quit? Unsaved progress will
// be lost" is answered by its two prompts and by nothing else on the screen,
// so a remote that drops the Back half of it leaves a lone OK under a warning.
void test_a_confirmation_dialog_keeps_both_answers() {
    const std::vector<GuidePrompt> dialog = {{ButtonGlyph::A, "OK"}, {ButtonGlyph::B, "Back"}};
    const PromptStyle was = prompt_style();
    for (const PromptStyle style : {PromptStyle::Pad, PromptStyle::Touch, PromptStyle::Remote}) {
        set_prompt_style(style);
        CHECK(prompts_worth_drawing(dialog).size() == 2);
    }
    set_prompt_style(was);
}

// Which style goes with what is plugged in. The remote is the interesting one:
// SDL reports it as a controller, so only the gamepad count tells them apart.
void test_the_style_follows_what_is_in_your_hands() {
    // A phone or a tablet: a pad if one is attached, fingers otherwise.
    CHECK(prompt_style_for(/*television=*/false, 0, 0) == PromptStyle::Touch);
    CHECK(prompt_style_for(/*television=*/false, 1, 1) == PromptStyle::Pad);

    // An Apple TV with only the remote -- which SDL counts as a controller,
    // and which is exactly why the second count exists.
    CHECK(prompt_style_for(/*television=*/true, 1, 0) == PromptStyle::Remote);
    // Pair a pad with it and the glyphs come back.
    CHECK(prompt_style_for(/*television=*/true, 2, 1) == PromptStyle::Pad);
    // Nothing at all is still not a touchscreen.
    CHECK(prompt_style_for(/*television=*/true, 0, 0) == PromptStyle::Remote);
}

// The name is the whole test, and it is the name a real Apple TV gave:
//
//     controller connected: Remote (SDL type: unknown, a pad)
//
// The version before this asked SDL whether the controller had an X button.
// It said yes, so every Apple TV got pad glyphs and the prompts this is all
// for never went away.
void test_a_siri_remote_is_recognised_by_what_sdl_calls_it() {
    CHECK(controller_is_a_remote("Remote"));
    // Whatever case, and with something around it, since the exact string is
    // Apple's to change and has no promise attached to it.
    CHECK(controller_is_a_remote("Apple TV Remote"));
    CHECK(controller_is_a_remote("Siri remote"));

    // The pads this has actually been used with stay pads.
    CHECK(!controller_is_a_remote("Xbox Wireless Controller"));
    CHECK(!controller_is_a_remote("DualSense Wireless Controller"));
    CHECK(!controller_is_a_remote("Nintendo Switch Pro Controller"));
    CHECK(!controller_is_a_remote("Steam Deck"));
    CHECK(!controller_is_a_remote(""));
    // SDL returns null for a controller it has no name for, which is not a
    // reason to treat it as a remote.
    CHECK(!controller_is_a_remote(nullptr));
}

// A Siri Remote has one button on its face, and clicking it arrives as SDL's
// south. East -- the default, and every Nintendo pad's convention -- names a
// button that is not there, so on an Apple TV nothing confirmed and the click
// read as cancel instead.
void test_yes_lands_on_the_button_a_remote_actually_has() {
    CHECK(confirm_button_for_style(PromptStyle::Remote) == ConfirmButton::South);
    // And nothing else moves: a pad paired with the Apple TV confirms on east
    // like every other pad, and so does a phone.
    CHECK(confirm_button_for_style(PromptStyle::Pad) == ConfirmButton::East);
    CHECK(confirm_button_for_style(PromptStyle::Touch) == ConfirmButton::East);
}

void test_the_hint_style_is_asked_rather_than_assumed() {
    const PromptStyle was = prompt_style();
    set_prompt_style(PromptStyle::Touch);
    CHECK(prompt_style() == PromptStyle::Touch);
    CHECK(touch_hints());
    // A remote gets glyphs, not chips: it has buttons, they are just not the
    // ones a prompt needs to name.
    set_prompt_style(PromptStyle::Remote);
    CHECK(!touch_hints());
    set_prompt_style(PromptStyle::Pad);
    CHECK(!touch_hints());
    set_prompt_style(was);
}

}  // namespace

int main() {
    test_a_finger_finds_the_prompt_it_landed_on();
    test_every_prompt_outside_a_menu_does_something();
    test_the_menus_own_prompts_are_left_to_the_menu();
    test_a_prompt_for_something_tappable_goes_away_on_touch();
    test_select_and_back_go_away_on_a_remote();
    test_a_confirm_with_nothing_to_tap_stays();
    test_a_confirmation_dialog_keeps_both_answers();
    test_the_style_follows_what_is_in_your_hands();
    test_a_siri_remote_is_recognised_by_what_sdl_calls_it();
    test_yes_lands_on_the_button_a_remote_actually_has();
    test_the_hint_style_is_asked_rather_than_assumed();
    return test::finish("touch-hints");
}
