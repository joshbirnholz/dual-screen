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

DsButton mapped(SDL_GameControllerButton button, PadLetterLayout layout, bool& found) {
    DsButton result = DsButton::A;
    found = ds_button_for_controller_button(button, layout, result);
    return result;
}

// On an Xbox-layout pad -- which is what Steam Input's virtual controller is --
// the game's face buttons sit where a DS puts them, so the button marked A is
// DS B.
void test_face_buttons_follow_ds_positions_on_an_xbox_pad() {
    const PadLetterLayout xbox = PadLetterLayout::Xbox;
    bool found = false;

    CHECK(mapped(SDL_CONTROLLER_BUTTON_A, xbox, found) == DsButton::B);  // A is south
    CHECK(found);
    CHECK(mapped(SDL_CONTROLLER_BUTTON_B, xbox, found) == DsButton::A);  // B is east
    CHECK(mapped(SDL_CONTROLLER_BUTTON_X, xbox, found) == DsButton::Y);  // X is west
    CHECK(mapped(SDL_CONTROLLER_BUTTON_Y, xbox, found) == DsButton::X);  // Y is north
}

// The same rule on a Nintendo-layout pad is the identity: its letters already
// sit where a DS puts them, so nothing moves.
void test_face_buttons_do_not_move_on_a_nintendo_pad() {
    const PadLetterLayout nintendo = PadLetterLayout::Nintendo;
    bool found = false;

    CHECK(mapped(SDL_CONTROLLER_BUTTON_A, nintendo, found) == DsButton::A);
    CHECK(found);
    CHECK(mapped(SDL_CONTROLLER_BUTTON_B, nintendo, found) == DsButton::B);
    CHECK(mapped(SDL_CONTROLLER_BUTTON_X, nintendo, found) == DsButton::X);
    CHECK(mapped(SDL_CONTROLLER_BUTTON_Y, nintendo, found) == DsButton::Y);
}

void test_pad_layout_detection() {
    // Steam Input's virtual pad, and everything else, prints Xbox-style.
    for (const SDL_GameControllerType type :
         {SDL_CONTROLLER_TYPE_XBOX360, SDL_CONTROLLER_TYPE_XBOXONE, SDL_CONTROLLER_TYPE_PS4,
          SDL_CONTROLLER_TYPE_PS5, SDL_CONTROLLER_TYPE_VIRTUAL, SDL_CONTROLLER_TYPE_UNKNOWN}) {
        CHECK_MSG(pad_letter_layout(type) == PadLetterLayout::Xbox, to_string(type));
    }
    for (const SDL_GameControllerType type :
         {SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO,
          SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_LEFT,
          SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT,
          SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_PAIR}) {
        CHECK_MSG(pad_letter_layout(type) == PadLetterLayout::Nintendo, to_string(type));
    }
}

void test_shared_buttons_are_layout_independent() {
    bool found_a = false;
    bool found_b = false;
    const SDL_GameControllerButton shared[] = {
        SDL_CONTROLLER_BUTTON_LEFTSHOULDER, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
        SDL_CONTROLLER_BUTTON_START,        SDL_CONTROLLER_BUTTON_BACK,
        SDL_CONTROLLER_BUTTON_DPAD_UP,      SDL_CONTROLLER_BUTTON_DPAD_DOWN,
        SDL_CONTROLLER_BUTTON_DPAD_LEFT,    SDL_CONTROLLER_BUTTON_DPAD_RIGHT,
    };
    for (const SDL_GameControllerButton button : shared) {
        const DsButton a = mapped(button, PadLetterLayout::Xbox, found_a);
        const DsButton b = mapped(button, PadLetterLayout::Nintendo, found_b);
        CHECK(found_a);
        CHECK(found_b);
        CHECK(a == b);
    }
}

void test_every_ds_button_is_reachable_exactly_once() {
    for (const PadLetterLayout layout : {PadLetterLayout::Xbox, PadLetterLayout::Nintendo}) {
        std::set<int> reached;
        for (int index = 0; index < SDL_CONTROLLER_BUTTON_MAX; ++index) {
            DsButton ds_button = DsButton::A;
            if (ds_button_for_controller_button(static_cast<SDL_GameControllerButton>(index),
                                                layout, ds_button)) {
                // No two controller buttons may fight over one DS button.
                CHECK(reached.insert(static_cast<int>(ds_button)).second);
            }
        }
        // All twelve DS buttons have to be playable.
        CHECK_EQ(reached.size(), static_cast<std::size_t>(DsButton::Count));
    }
}

void test_unmapped_buttons_are_rejected() {
    bool found = true;
    // The stick clicks and Guide must never reach the game.
    for (const SDL_GameControllerButton button :
         {SDL_CONTROLLER_BUTTON_RIGHTSTICK, SDL_CONTROLLER_BUTTON_LEFTSTICK,
          SDL_CONTROLLER_BUTTON_GUIDE}) {
        mapped(button, PadLetterLayout::Xbox, found);
        CHECK(!found);
    }
}

void test_menu_buttons_are_the_printed_letters() {
    // Menus are deliberately not affected by the DS mapping: they always use
    // the buttons marked A and B, which is what every platform's own UI does.
    // On an Xbox pad that means A confirms a menu but is DS B in game, which is
    // correct -- the menu is not the game.
    CHECK(kMenuConfirmButton == SDL_CONTROLLER_BUTTON_A);
    CHECK(kMenuCancelButton == SDL_CONTROLLER_BUTTON_B);
    CHECK(kMenuConfirmButton != kMenuCancelButton);
    // And SDL has to be reporting printed letters for that to hold.
    CHECK_STR_EQ(kUseButtonLabels, "1");
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

    // The same press, reported by two devices in one frame.
    input.handle_event(button_down_event(0, SDL_CONTROLLER_BUTTON_A));
    input.handle_event(button_down_event(1, SDL_CONTROLLER_BUTTON_A));
    CHECK_EQ(count_action(input.ui_actions(), UiAction::Confirm), static_cast<std::size_t>(1));

    // Distinct actions in the same frame all survive.
    input.handle_event(button_down_event(0, SDL_CONTROLLER_BUTTON_DPAD_DOWN));
    input.handle_event(button_down_event(1, SDL_CONTROLLER_BUTTON_DPAD_DOWN));
    input.handle_event(button_down_event(0, SDL_CONTROLLER_BUTTON_B));
    CHECK_EQ(count_action(input.ui_actions(), UiAction::Down), static_cast<std::size_t>(1));
    CHECK_EQ(count_action(input.ui_actions(), UiAction::Cancel), static_cast<std::size_t>(1));
    CHECK_EQ(count_action(input.ui_actions(), UiAction::Confirm), static_cast<std::size_t>(1));

    // A new frame accepts the action again -- this suppresses duplicates, not
    // repeat presses.
    input.begin_frame();
    CHECK_EQ(count_action(input.ui_actions(), UiAction::Confirm), static_cast<std::size_t>(0));
    input.handle_event(button_down_event(0, SDL_CONTROLLER_BUTTON_A));
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
    test_face_buttons_follow_ds_positions_on_an_xbox_pad();
    test_face_buttons_do_not_move_on_a_nintendo_pad();
    test_pad_layout_detection();
    test_shared_buttons_are_layout_independent();
    test_every_ds_button_is_reachable_exactly_once();
    test_unmapped_buttons_are_rejected();
    test_menu_buttons_are_the_printed_letters();
    test_duplicate_presses_are_merged();
    test_keyboard_can_drive_menus();
    return test::finish("controller-mapping");
}
