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

DsButton mapped(SDL_GameControllerButton button, FaceButtonConvention convention, bool& found) {
    DsButton result = DsButton::A;
    found = ds_button_for_controller_button(button, convention, result);
    return result;
}

void test_labels_convention_matches_letters() {
    const FaceButtonConvention labels = FaceButtonConvention::Labels;
    bool found = false;

    // SDL is handing us the printed letters, so they pass straight through.
    CHECK(mapped(SDL_CONTROLLER_BUTTON_A, labels, found) == DsButton::A);
    CHECK(found);
    CHECK(mapped(SDL_CONTROLLER_BUTTON_B, labels, found) == DsButton::B);
    CHECK(mapped(SDL_CONTROLLER_BUTTON_X, labels, found) == DsButton::X);
    CHECK(mapped(SDL_CONTROLLER_BUTTON_Y, labels, found) == DsButton::Y);
}

void test_positions_convention_uses_the_ds_layout() {
    const FaceButtonConvention positions = FaceButtonConvention::Positions;
    bool found = false;

    // SDL is handing us positions, so the DS's own layout applies: B is south,
    // A is east, Y is west, X is north.
    CHECK(mapped(SDL_CONTROLLER_BUTTON_A, positions, found) == DsButton::B);  // south
    CHECK(found);
    CHECK(mapped(SDL_CONTROLLER_BUTTON_B, positions, found) == DsButton::A);  // east
    CHECK(mapped(SDL_CONTROLLER_BUTTON_X, positions, found) == DsButton::Y);  // west
    CHECK(mapped(SDL_CONTROLLER_BUTTON_Y, positions, found) == DsButton::X);  // north
}

void test_shared_buttons_are_convention_independent() {
    bool found_labels = false;
    bool found_positions = false;
    const SDL_GameControllerButton shared[] = {
        SDL_CONTROLLER_BUTTON_LEFTSHOULDER, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
        SDL_CONTROLLER_BUTTON_START,        SDL_CONTROLLER_BUTTON_BACK,
        SDL_CONTROLLER_BUTTON_DPAD_UP,      SDL_CONTROLLER_BUTTON_DPAD_DOWN,
        SDL_CONTROLLER_BUTTON_DPAD_LEFT,    SDL_CONTROLLER_BUTTON_DPAD_RIGHT,
    };
    for (const SDL_GameControllerButton button : shared) {
        const DsButton a = mapped(button, FaceButtonConvention::Labels, found_labels);
        const DsButton b = mapped(button, FaceButtonConvention::Positions, found_positions);
        CHECK(found_labels);
        CHECK(found_positions);
        CHECK(a == b);
    }
}

void test_every_ds_button_is_reachable_exactly_once() {
    for (const FaceButtonConvention convention :
         {FaceButtonConvention::Labels, FaceButtonConvention::Positions}) {
        std::set<int> reached;
        for (int index = 0; index < SDL_CONTROLLER_BUTTON_MAX; ++index) {
            DsButton ds_button = DsButton::A;
            if (ds_button_for_controller_button(static_cast<SDL_GameControllerButton>(index),
                                                convention, ds_button)) {
                // No two controller buttons may fight over one DS button.
                const bool inserted = reached.insert(static_cast<int>(ds_button)).second;
                CHECK_MSG(inserted, std::string("two controller buttons map to the same DS button "
                                                "under ") + to_string(convention));
            }
        }
        // All twelve DS buttons have to be playable.
        CHECK_EQ(reached.size(), static_cast<std::size_t>(DsButton::Count));
    }
}

void test_unmapped_buttons_are_rejected() {
    bool found = true;
    // R3 is the menu button and must not also reach the game, or opening the
    // menu would press something.
    mapped(SDL_CONTROLLER_BUTTON_RIGHTSTICK, FaceButtonConvention::Labels, found);
    CHECK(!found);
    mapped(SDL_CONTROLLER_BUTTON_LEFTSTICK, FaceButtonConvention::Labels, found);
    CHECK(!found);
    mapped(SDL_CONTROLLER_BUTTON_GUIDE, FaceButtonConvention::Labels, found);
    CHECK(!found);
}

void test_confirm_and_cancel_track_the_convention() {
    for (const FaceButtonConvention convention :
         {FaceButtonConvention::Labels, FaceButtonConvention::Positions}) {
        // Confirm and cancel must be different buttons, or menus deadlock.
        CHECK(confirm_button(convention) != cancel_button(convention));

        // Confirm is always whichever button the player calls "A", so it must
        // be the one that maps to DS A.
        DsButton ds_button = DsButton::B;
        CHECK(ds_button_for_controller_button(confirm_button(convention), convention, ds_button));
        CHECK_MSG(ds_button == DsButton::A,
                  std::string("confirm is not the player's A button under ") + to_string(convention));

        CHECK(ds_button_for_controller_button(cancel_button(convention), convention, ds_button));
        CHECK_MSG(ds_button == DsButton::B,
                  std::string("cancel is not the player's B button under ") + to_string(convention));

    }
}

void test_hint_labels_name_the_physical_button() {
    // Two different things are easy to conflate here. Which DS button confirm
    // *acts as* is always DS A (asserted above). Which letter is *printed* on
    // the button the player has to press is not the same thing, because it
    // depends on the pad the convention was chosen for.
    //
    // Labels mode: SDL hands us letters, so confirm is the button printed "A".
    CHECK_STR_EQ(confirm_button_label(FaceButtonConvention::Labels), "A");
    CHECK_STR_EQ(cancel_button_label(FaceButtonConvention::Labels), "B");

    // Positions mode exists for an Xbox-layout pad -- a Nintendo-layout pad is
    // already correct in Labels mode. Confirm is the east button, and on an
    // Xbox-layout pad east is printed "B". Telling that player to "press A"
    // would send them to the wrong button.
    CHECK_STR_EQ(confirm_button_label(FaceButtonConvention::Positions), "B");
    CHECK_STR_EQ(cancel_button_label(FaceButtonConvention::Positions), "A");
}

void test_hint_value_matches_convention() {
    // These are the exact strings SDL documents; getting them backwards would
    // invert every face button.
    CHECK_STR_EQ(sdl_button_labels_hint(FaceButtonConvention::Labels), "1");
    CHECK_STR_EQ(sdl_button_labels_hint(FaceButtonConvention::Positions), "0");
}

void test_convention_parsing() {
    CHECK(face_button_convention_from_string("labels", FaceButtonConvention::Positions) ==
          FaceButtonConvention::Labels);
    CHECK(face_button_convention_from_string("positions", FaceButtonConvention::Labels) ==
          FaceButtonConvention::Positions);
    // Anything unrecognised keeps the caller's default rather than guessing.
    CHECK(face_button_convention_from_string("nonsense", FaceButtonConvention::Labels) ==
          FaceButtonConvention::Labels);
    CHECK(face_button_convention_from_string("", FaceButtonConvention::Positions) ==
          FaceButtonConvention::Positions);
    // Round-trips through its own string form.
    for (const FaceButtonConvention convention :
         {FaceButtonConvention::Labels, FaceButtonConvention::Positions}) {
        CHECK(face_button_convention_from_string(to_string(convention),
                                                 FaceButtonConvention::Labels) == convention);
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
    test_labels_convention_matches_letters();
    test_positions_convention_uses_the_ds_layout();
    test_shared_buttons_are_convention_independent();
    test_every_ds_button_is_reachable_exactly_once();
    test_unmapped_buttons_are_rejected();
    test_confirm_and_cancel_track_the_convention();
    test_hint_labels_name_the_physical_button();
    test_hint_value_matches_convention();
    test_convention_parsing();
    test_duplicate_presses_are_merged();
    test_keyboard_can_drive_menus();
    return test::finish("controller-mapping");
}
