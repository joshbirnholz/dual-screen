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

DsButton mapped(SDL_GameControllerButton button,
                GameFaceMapping mapping,
                PadLetterLayout layout,
                bool& found) {
    DsButton result = DsButton::A;
    found = ds_button_for_controller_button(button, mapping, layout, result);
    return result;
}

// What the player actually asked for: on an Xbox-layout pad -- which is what
// Steam Input's virtual controller is -- the game's A/B and X/Y are swapped
// relative to the printed letters, so the buttons sit where a DS puts them.
void test_ds_layout_swaps_on_an_xbox_pad() {
    const GameFaceMapping ds = GameFaceMapping::DsLayout;
    const PadLetterLayout xbox = PadLetterLayout::Xbox;
    bool found = false;

    CHECK(mapped(SDL_CONTROLLER_BUTTON_A, ds, xbox, found) == DsButton::B);  // A is south
    CHECK(found);
    CHECK(mapped(SDL_CONTROLLER_BUTTON_B, ds, xbox, found) == DsButton::A);  // B is east
    CHECK(mapped(SDL_CONTROLLER_BUTTON_X, ds, xbox, found) == DsButton::Y);  // X is west
    CHECK(mapped(SDL_CONTROLLER_BUTTON_Y, ds, xbox, found) == DsButton::X);  // Y is north
}

// The same setting on a Nintendo-layout pad must *not* swap: its letters
// already sit where a DS puts them, so swapping would break it.
void test_ds_layout_is_identity_on_a_nintendo_pad() {
    const GameFaceMapping ds = GameFaceMapping::DsLayout;
    const PadLetterLayout nintendo = PadLetterLayout::Nintendo;
    bool found = false;

    CHECK(mapped(SDL_CONTROLLER_BUTTON_A, ds, nintendo, found) == DsButton::A);
    CHECK(found);
    CHECK(mapped(SDL_CONTROLLER_BUTTON_B, ds, nintendo, found) == DsButton::B);
    CHECK(mapped(SDL_CONTROLLER_BUTTON_X, ds, nintendo, found) == DsButton::X);
    CHECK(mapped(SDL_CONTROLLER_BUTTON_Y, ds, nintendo, found) == DsButton::Y);
}

// match_labels means the printed letter wins, on every pad.
void test_match_labels_ignores_the_pad_layout() {
    const GameFaceMapping labels = GameFaceMapping::MatchLabels;
    bool found = false;
    for (const PadLetterLayout layout : {PadLetterLayout::Xbox, PadLetterLayout::Nintendo}) {
        CHECK(mapped(SDL_CONTROLLER_BUTTON_A, labels, layout, found) == DsButton::A);
        CHECK(found);
        CHECK(mapped(SDL_CONTROLLER_BUTTON_B, labels, layout, found) == DsButton::B);
        CHECK(mapped(SDL_CONTROLLER_BUTTON_X, labels, layout, found) == DsButton::X);
        CHECK(mapped(SDL_CONTROLLER_BUTTON_Y, labels, layout, found) == DsButton::Y);
    }
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

// The two mappings must differ on an Xbox pad and agree on a Nintendo one --
// that is the whole point of resolving through physical position.
void test_the_two_mappings_differ_only_where_the_letters_do() {
    bool found_a = false;
    bool found_b = false;
    const SDL_GameControllerButton faces[] = {
        SDL_CONTROLLER_BUTTON_A, SDL_CONTROLLER_BUTTON_B,
        SDL_CONTROLLER_BUTTON_X, SDL_CONTROLLER_BUTTON_Y};

    int differences = 0;
    for (const SDL_GameControllerButton button : faces) {
        if (mapped(button, GameFaceMapping::DsLayout, PadLetterLayout::Xbox, found_a) !=
            mapped(button, GameFaceMapping::MatchLabels, PadLetterLayout::Xbox, found_b)) {
            ++differences;
        }
        CHECK(mapped(button, GameFaceMapping::DsLayout, PadLetterLayout::Nintendo, found_a) ==
              mapped(button, GameFaceMapping::MatchLabels, PadLetterLayout::Nintendo, found_b));
    }
    CHECK_EQ(differences, 4);  // all four face buttons move on an Xbox pad
}

void test_shared_buttons_are_mapping_independent() {
    bool found_a = false;
    bool found_b = false;
    const SDL_GameControllerButton shared[] = {
        SDL_CONTROLLER_BUTTON_LEFTSHOULDER, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
        SDL_CONTROLLER_BUTTON_START,        SDL_CONTROLLER_BUTTON_BACK,
        SDL_CONTROLLER_BUTTON_DPAD_UP,      SDL_CONTROLLER_BUTTON_DPAD_DOWN,
        SDL_CONTROLLER_BUTTON_DPAD_LEFT,    SDL_CONTROLLER_BUTTON_DPAD_RIGHT,
    };
    for (const SDL_GameControllerButton button : shared) {
        const DsButton a = mapped(button, GameFaceMapping::DsLayout, PadLetterLayout::Xbox, found_a);
        const DsButton b =
            mapped(button, GameFaceMapping::MatchLabels, PadLetterLayout::Nintendo, found_b);
        CHECK(found_a);
        CHECK(found_b);
        CHECK(a == b);
    }
}

void test_every_ds_button_is_reachable_exactly_once() {
    for (const GameFaceMapping mapping : {GameFaceMapping::DsLayout, GameFaceMapping::MatchLabels}) {
        for (const PadLetterLayout layout : {PadLetterLayout::Xbox, PadLetterLayout::Nintendo}) {
            std::set<int> reached;
            for (int index = 0; index < SDL_CONTROLLER_BUTTON_MAX; ++index) {
                DsButton ds_button = DsButton::A;
                if (ds_button_for_controller_button(static_cast<SDL_GameControllerButton>(index),
                                                    mapping, layout, ds_button)) {
                    // No two controller buttons may fight over one DS button.
                    CHECK_MSG(reached.insert(static_cast<int>(ds_button)).second,
                              std::string("two buttons collide under ") + to_string(mapping));
                }
            }
            // All twelve DS buttons have to be playable.
            CHECK_EQ(reached.size(), static_cast<std::size_t>(DsButton::Count));
        }
    }
}

void test_unmapped_buttons_are_rejected() {
    bool found = true;
    // The stick clicks and Guide must never reach the game.
    for (const SDL_GameControllerButton button :
         {SDL_CONTROLLER_BUTTON_RIGHTSTICK, SDL_CONTROLLER_BUTTON_LEFTSTICK,
          SDL_CONTROLLER_BUTTON_GUIDE}) {
        mapped(button, GameFaceMapping::DsLayout, PadLetterLayout::Xbox, found);
        CHECK(!found);
    }
}

void test_menu_buttons_are_the_printed_letters() {
    // Menus are deliberately not affected by the game mapping: they always use
    // the buttons marked A and B, which is what every platform's own UI does.
    CHECK(kMenuConfirmButton == SDL_CONTROLLER_BUTTON_A);
    CHECK(kMenuCancelButton == SDL_CONTROLLER_BUTTON_B);
    CHECK(kMenuConfirmButton != kMenuCancelButton);
    // And SDL has to be reporting printed letters for that to hold.
    CHECK_STR_EQ(kUseButtonLabels, "1");
}

void test_mapping_parsing() {
    CHECK(game_face_mapping_from_string("ds_layout", GameFaceMapping::MatchLabels) ==
          GameFaceMapping::DsLayout);
    CHECK(game_face_mapping_from_string("match_labels", GameFaceMapping::DsLayout) ==
          GameFaceMapping::MatchLabels);
    // The short-lived key this replaced still resolves, so nobody's settings
    // silently change meaning under them.
    CHECK(game_face_mapping_from_string("positions", GameFaceMapping::MatchLabels) ==
          GameFaceMapping::DsLayout);
    CHECK(game_face_mapping_from_string("labels", GameFaceMapping::DsLayout) ==
          GameFaceMapping::MatchLabels);
    // Anything unrecognised keeps the caller's default rather than guessing.
    CHECK(game_face_mapping_from_string("nonsense", GameFaceMapping::DsLayout) ==
          GameFaceMapping::DsLayout);
    CHECK(game_face_mapping_from_string("", GameFaceMapping::MatchLabels) ==
          GameFaceMapping::MatchLabels);
    for (const GameFaceMapping mapping : {GameFaceMapping::DsLayout, GameFaceMapping::MatchLabels}) {
        CHECK(game_face_mapping_from_string(to_string(mapping), GameFaceMapping::DsLayout) ==
              mapping);
    }
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
    test_ds_layout_swaps_on_an_xbox_pad();
    test_ds_layout_is_identity_on_a_nintendo_pad();
    test_match_labels_ignores_the_pad_layout();
    test_pad_layout_detection();
    test_the_two_mappings_differ_only_where_the_letters_do();
    test_shared_buttons_are_mapping_independent();
    test_every_ds_button_is_reachable_exactly_once();
    test_unmapped_buttons_are_rejected();
    test_menu_buttons_are_the_printed_letters();
    test_mapping_parsing();
    test_duplicate_presses_are_merged();
    test_keyboard_can_drive_menus();
    return test::finish("controller-mapping");
}
