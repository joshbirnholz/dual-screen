// The face-button mapping had a real bug: the code assumed SDL reports face
// buttons by physical position, when SDL's documented default is to report them
// by printed label. These tests pin both conventions down so the assumption
// cannot drift back to being implicit.

#include "test_support.hpp"

#include <set>
#include <string>
#include <vector>

#include "input/controller_mapping.hpp"
#include "input/ds_input.hpp"
#include "input/input_manager.hpp"

using namespace ds;

namespace {

DsButton mapped(SDL_GameControllerButton button, bool& found) {
    DsButton result = DsButton::A;
    found = ds_button_for_controller_button(button, result);
    return result;
}

// Nintendo positional layout: east is A, south is B, north is X, west is Y.
// SDL is asked to report positions, so this holds on every pad.
void test_face_buttons_are_nintendo_positional() {
    bool found = false;
    CHECK(mapped(SDL_CONTROLLER_BUTTON_A, found) == DsButton::B);  // south
    CHECK(found);
    CHECK(mapped(SDL_CONTROLLER_BUTTON_B, found) == DsButton::A);  // east
    CHECK(mapped(SDL_CONTROLLER_BUTTON_X, found) == DsButton::Y);  // west
    CHECK(mapped(SDL_CONTROLLER_BUTTON_Y, found) == DsButton::X);  // north
}

void test_shoulders_start_and_dpad() {
    bool found = false;
    CHECK(mapped(SDL_CONTROLLER_BUTTON_LEFTSHOULDER, found) == DsButton::L);
    CHECK(found);
    CHECK(mapped(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, found) == DsButton::R);
    CHECK(mapped(SDL_CONTROLLER_BUTTON_START, found) == DsButton::Start);
    CHECK(mapped(SDL_CONTROLLER_BUTTON_BACK, found) == DsButton::Select);
    CHECK(mapped(SDL_CONTROLLER_BUTTON_DPAD_UP, found) == DsButton::Up);
    CHECK(mapped(SDL_CONTROLLER_BUTTON_DPAD_DOWN, found) == DsButton::Down);
    CHECK(mapped(SDL_CONTROLLER_BUTTON_DPAD_LEFT, found) == DsButton::Left);
    CHECK(mapped(SDL_CONTROLLER_BUTTON_DPAD_RIGHT, found) == DsButton::Right);
}

// The whole point of going positional: the button that confirms a menu is the
// same button that is DS A in game, and the one that cancels is DS B.
void test_menus_and_game_agree() {
    CHECK(kMenuConfirmButton != kMenuCancelButton);

    bool found = false;
    DsButton confirm = DsButton::B;
    CHECK(ds_button_for_controller_button(kMenuConfirmButton, confirm));
    CHECK_MSG(confirm == DsButton::A, "the menu's confirm button is not DS A");

    DsButton cancel = DsButton::A;
    CHECK(ds_button_for_controller_button(kMenuCancelButton, cancel));
    CHECK_MSG(cancel == DsButton::B, "the menu's cancel button is not DS B");

    // East confirms, south cancels.
    CHECK(kMenuConfirmButton == SDL_CONTROLLER_BUTTON_B);
    CHECK(kMenuCancelButton == SDL_CONTROLLER_BUTTON_A);
    (void)found;
}

void test_sdl_is_asked_for_positions() {
    // "0" is SDL's "report face buttons by position". Getting this backwards
    // would invert every face button on a Nintendo-layout pad.
    CHECK_STR_EQ(kFaceButtonsByPosition, "0");
}

void test_every_ds_button_is_reachable_exactly_once() {
    std::set<int> reached;
    for (int index = 0; index < SDL_CONTROLLER_BUTTON_MAX; ++index) {
        DsButton ds_button = DsButton::A;
        if (ds_button_for_controller_button(static_cast<SDL_GameControllerButton>(index),
                                            ds_button)) {
            // No two controller buttons may fight over one DS button.
            CHECK(reached.insert(static_cast<int>(ds_button)).second);
        }
    }
    // All twelve of a handheld's buttons have to be playable. The four after
    // them -- ZL, ZR and the two stick presses -- are deliberately not on this
    // map: which physical control produces them depends on the console being
    // played, so input_manager reads those directly. See DsButton.
    CHECK_EQ(reached.size(), static_cast<std::size_t>(DsButton::Zl));
}

void test_unmapped_buttons_are_rejected() {
    bool found = true;
    for (const SDL_GameControllerButton button :
         {SDL_CONTROLLER_BUTTON_RIGHTSTICK, SDL_CONTROLLER_BUTTON_LEFTSTICK,
          SDL_CONTROLLER_BUTTON_GUIDE}) {
        mapped(button, found);
        CHECK(!found);
    }
}

// Prompt letters follow the pad's printing, since that is what the player is
// looking at -- but they never change what a button does.
void test_prompt_letters_follow_the_pad() {
    const MenuButtonLabels xbox = menu_button_labels(PadLetterLayout::Xbox);
    CHECK_STR_EQ(xbox.confirm, "B");
    CHECK_STR_EQ(xbox.cancel, "A");

    const MenuButtonLabels nintendo = menu_button_labels(PadLetterLayout::Nintendo);
    CHECK_STR_EQ(nintendo.confirm, "A");
    CHECK_STR_EQ(nintendo.cancel, "B");

    CHECK(pad_letter_layout(SDL_CONTROLLER_TYPE_VIRTUAL) == PadLetterLayout::Xbox);
    CHECK(pad_letter_layout(SDL_CONTROLLER_TYPE_XBOX360) == PadLetterLayout::Xbox);
    CHECK(pad_letter_layout(SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO) == PadLetterLayout::Nintendo);
}

SDL_Event button_down_event(SDL_JoystickID which, SDL_GameControllerButton button) {
    SDL_Event event {};
    event.type = SDL_CONTROLLERBUTTONDOWN;
    event.cbutton.which = which;
    event.cbutton.button = static_cast<Uint8>(button);
    event.cbutton.state = SDL_PRESSED;
    return event;
}

std::size_t count_action(const std::vector<UiAction>& actions, UiAction wanted) {
    std::size_t total = 0;
    for (const UiAction action : actions) {
        if (action == wanted) ++total;
    }
    return total;
}

// Steam Input presents a virtual pad while the physical device usually stays
// visible too, so one physical press arrives as two identical button-down
// events. Acting on both stepped into a folder and straight back out of it,
// which read as the button doing nothing.
void test_duplicate_presses_are_merged() {
    InputManager input;
    input.begin_frame();

    // The same press, reported by two devices in one frame. Uses the menu
    // constants rather than a hardcoded letter, so changing which button
    // confirms cannot quietly turn this test into a no-op.
    input.handle_event(button_down_event(0, kMenuConfirmButton));
    input.handle_event(button_down_event(1, kMenuConfirmButton));
    CHECK_EQ(count_action(input.ui_actions(), UiAction::Confirm), static_cast<std::size_t>(1));

    // Distinct actions in the same frame all survive.
    input.handle_event(button_down_event(0, SDL_CONTROLLER_BUTTON_DPAD_DOWN));
    input.handle_event(button_down_event(1, SDL_CONTROLLER_BUTTON_DPAD_DOWN));
    input.handle_event(button_down_event(0, kMenuCancelButton));
    CHECK_EQ(count_action(input.ui_actions(), UiAction::Down), static_cast<std::size_t>(1));
    CHECK_EQ(count_action(input.ui_actions(), UiAction::Cancel), static_cast<std::size_t>(1));
    CHECK_EQ(count_action(input.ui_actions(), UiAction::Confirm), static_cast<std::size_t>(1));

    // A new frame accepts the action again -- this suppresses duplicates, not
    // repeat presses.
    input.begin_frame();
    CHECK_EQ(count_action(input.ui_actions(), UiAction::Confirm), static_cast<std::size_t>(0));
    input.handle_event(button_down_event(0, kMenuConfirmButton));
    CHECK_EQ(count_action(input.ui_actions(), UiAction::Confirm), static_cast<std::size_t>(1));
}

// A Steam Input layout that sends keystrokes rather than gamepad buttons still
// has to be able to drive the menus.
void test_keyboard_can_drive_menus() {
    InputManager input;

    const auto key_event = [](SDL_Keycode key) {
        SDL_Event event {};
        event.type = SDL_KEYDOWN;
        event.key.keysym.sym = key;
        event.key.repeat = 0;
        return event;
    };

    input.begin_frame();
    input.handle_event(key_event(SDLK_RETURN));
    CHECK_EQ(count_action(input.ui_actions(), UiAction::Confirm), static_cast<std::size_t>(1));

    input.begin_frame();
    input.handle_event(key_event(SDLK_SPACE));
    CHECK_EQ(count_action(input.ui_actions(), UiAction::Confirm), static_cast<std::size_t>(1));

    input.begin_frame();
    input.handle_event(key_event(SDLK_ESCAPE));
    CHECK_EQ(count_action(input.ui_actions(), UiAction::Cancel), static_cast<std::size_t>(1));

    input.begin_frame();
    input.handle_event(key_event(SDLK_BACKSPACE));
    CHECK_EQ(count_action(input.ui_actions(), UiAction::Cancel), static_cast<std::size_t>(1));
}

}  // namespace

int main() {
    test_face_buttons_are_nintendo_positional();
    test_shoulders_start_and_dpad();
    test_menus_and_game_agree();
    test_sdl_is_asked_for_positions();
    test_every_ds_button_is_reachable_exactly_once();
    test_unmapped_buttons_are_rejected();
    test_prompt_letters_follow_the_pad();
    test_duplicate_presses_are_merged();
    test_keyboard_can_drive_menus();
    return test::finish("controller-mapping");
}
