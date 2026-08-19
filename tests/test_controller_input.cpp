// Drives a real SDL virtual game controller through InputManager, so the whole
// path -- SDL button -> mapping -> DS button state -- is exercised without any
// hardware. This is the level the face-button bug actually lived at: the table
// was right in isolation and the wrong one was being selected.

#include "test_support.hpp"

#include <SDL.h>

#include "config/settings.hpp"
#include "input/controller_mapping.hpp"
#include "input/input_manager.hpp"
#include "layout/layout_registry.hpp"

using namespace ds;

namespace {

// A virtual pad reports as SDL_CONTROLLER_TYPE_VIRTUAL, which prints its
// letters Xbox-style -- the same as Steam Input's virtual controller.
struct VirtualPad {
    int device_index = -1;
    SDL_Joystick* joystick = nullptr;

    bool attach() {
        device_index = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER, 6,
                                                 SDL_CONTROLLER_BUTTON_MAX, 1);
        if (device_index < 0) {
            return false;
        }
        SDL_GameControllerUpdate();
        joystick = SDL_JoystickOpen(device_index);
        return joystick != nullptr;
    }

    void press(SDL_GameControllerButton button, bool down) {
        SDL_JoystickSetVirtualButton(joystick, static_cast<int>(button), down ? 1 : 0);
        SDL_GameControllerUpdate();
    }

    void detach() {
        if (joystick != nullptr) {
            SDL_JoystickClose(joystick);
            joystick = nullptr;
        }
        if (device_index >= 0) {
            SDL_JoystickDetachVirtual(device_index);
            device_index = -1;
        }
    }
};

DsInputState press_and_read(SDL_GameControllerButton button) {
    VirtualPad pad;
    if (!pad.attach()) {
        return DsInputState();
    }

    InputManager input;
    input.initialize(Settings());

    pad.press(button, true);
    input.begin_frame();
    input.end_frame(LayoutRegistry().fallback(), 1280.0f, 800.0f);
    const DsInputState state = input.ds_state();

    input.shutdown();
    pad.detach();
    return state;
}

// The reported bug, end to end: on an Xbox-layout pad the game's A is the east
// button, so pressing the button marked A gives DS B.
void test_face_buttons_follow_ds_positions() {
    DsInputState state = press_and_read(SDL_CONTROLLER_BUTTON_A);
    CHECK_MSG(state.pressed(DsButton::B), "the button marked A did not give DS B");
    CHECK(!state.pressed(DsButton::A));

    state = press_and_read(SDL_CONTROLLER_BUTTON_B);
    CHECK_MSG(state.pressed(DsButton::A), "the button marked B did not give DS A");
    CHECK(!state.pressed(DsButton::B));

    state = press_and_read(SDL_CONTROLLER_BUTTON_X);
    CHECK_MSG(state.pressed(DsButton::Y), "the button marked X did not give DS Y");
    CHECK(!state.pressed(DsButton::X));

    state = press_and_read(SDL_CONTROLLER_BUTTON_Y);
    CHECK_MSG(state.pressed(DsButton::X), "the button marked Y did not give DS X");
    CHECK(!state.pressed(DsButton::Y));
}

// Shoulders, Start and the d-pad are unaffected by any of that.
void test_shoulders_and_dpad_are_stable() {
    CHECK(press_and_read(SDL_CONTROLLER_BUTTON_LEFTSHOULDER).pressed(DsButton::L));
    CHECK(press_and_read(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER).pressed(DsButton::R));
    CHECK(press_and_read(SDL_CONTROLLER_BUTTON_DPAD_UP).pressed(DsButton::Up));
    CHECK(press_and_read(SDL_CONTROLLER_BUTTON_START).pressed(DsButton::Start));
    CHECK(press_and_read(SDL_CONTROLLER_BUTTON_BACK).pressed(DsButton::Select));
}

// The stick click must not reach the game at all.
void test_stick_click_is_not_a_ds_button() {
    const DsInputState state = press_and_read(SDL_CONTROLLER_BUTTON_RIGHTSTICK);
    CHECK_EQ(state.buttons, 0u);
}

}  // namespace

int main() {
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, kUseButtonLabels);
    if (SDL_Init(SDL_INIT_GAMECONTROLLER) != 0) {
        std::fprintf(stderr, "skipping: SDL game controller subsystem unavailable: %s\n",
                     SDL_GetError());
        return 0;
    }

    test_face_buttons_follow_ds_positions();
    test_shoulders_and_dpad_are_stable();
    test_stick_click_is_not_a_ds_button();

    SDL_Quit();
    return test::finish("controller-input");
}
