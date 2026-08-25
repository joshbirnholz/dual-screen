// Drives a real SDL virtual game controller through InputManager, so the whole
// path -- SDL button -> mapping -> DS button state -- is exercised without any
// hardware. This is the level the face-button bug actually lived at: the table
// was right in isolation and the wrong one was being selected.

#include "test_support.hpp"

#include <SDL.h>

#include <cstdlib>
#include <fstream>
#include <string>

#include "config/paths.hpp"
#include "config/settings.hpp"
#include "core/libretro_screen_source.hpp"
#include "input/controller_mapping.hpp"
#include "input/input_manager.hpp"
#include "core/system.hpp"
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
    input.end_frame(LayoutRegistry().fallback(), 1280.0f, 800.0f,
                    system_profile(SystemId::Ds).screens);
    const DsInputState state = input.ds_state();

    input.shutdown();
    pad.detach();
    return state;
}

// Nintendo positional, end to end: south is DS B, east is DS A.
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

// Pushing the left stick, read the way each console reads it. A DS has no
// stick, so the stick has to stand in for the d-pad; a 3DS has both, and
// sending both from one stick is what made its circle pad unusable -- every
// core that reads the d-pad saw a direction held down for as long as the stick
// was off centre.
DsInputState push_stick_and_read(bool stick_is_dpad) {
    VirtualPad pad;
    if (!pad.attach()) {
        return DsInputState();
    }

    InputManager input;
    input.initialize(Settings());
    input.set_stick_is_dpad(stick_is_dpad);

    // Hard left, well past any deadzone.
    SDL_JoystickSetVirtualAxis(pad.joystick, SDL_CONTROLLER_AXIS_LEFTX, -32000);
    SDL_GameControllerUpdate();
    input.begin_frame();
    input.end_frame(LayoutRegistry().fallback(), 1280.0f, 800.0f,
                    system_profile(SystemId::ThreeDs).screens);
    const DsInputState state = input.ds_state();

    input.shutdown();
    pad.detach();
    return state;
}

void test_the_stick_is_the_dpad_only_where_there_is_no_stick() {
    // A console with an analogue stick of its own: the circle pad moves and the
    // d-pad stays up.
    const DsInputState with_stick = push_stick_and_read(/*stick_is_dpad=*/false);
    CHECK_MSG(with_stick.circle_x < -16000, "the circle pad did not follow the stick");
    CHECK_MSG(!with_stick.pressed(DsButton::Left),
              "the stick pressed the d-pad on a console that has its own stick");
    CHECK_EQ(with_stick.buttons, 0u);

    // A console without one: the same push has to reach the d-pad, because
    // that is the only way to walk.
    const DsInputState without = push_stick_and_read(/*stick_is_dpad=*/true);
    CHECK_MSG(without.pressed(DsButton::Left),
              "the stick did not stand in for the d-pad on a console with no stick");
    CHECK_MSG(without.circle_x < -16000, "the circle pad should still follow the stick");
}

// The stick click must not reach the game at all.
void test_stick_click_is_not_a_ds_button() {
    CHECK_EQ(press_and_read(SDL_CONTROLLER_BUTTON_RIGHTSTICK).buttons, 0u);
    CHECK_EQ(press_and_read(SDL_CONTROLLER_BUTTON_LEFTSTICK).buttons, 0u);
}

// The right stick is a second, independent axis -- the C-Stick a Circle Pad
// Pro or New 3DS reads, unrelated to the circle pad on the left.
void test_right_stick_reaches_the_c_stick() {
    VirtualPad pad;
    if (!pad.attach()) {
        return;
    }

    InputManager input;
    input.initialize(Settings());

    SDL_JoystickSetVirtualAxis(pad.joystick, SDL_CONTROLLER_AXIS_RIGHTX, -32000);
    SDL_GameControllerUpdate();
    input.begin_frame();
    input.end_frame(LayoutRegistry().fallback(), 1280.0f, 800.0f,
                    system_profile(SystemId::ThreeDs).screens);
    const DsInputState state = input.ds_state();

    input.shutdown();
    pad.detach();

    CHECK_MSG(state.c_stick_x < -16000, "the C-Stick did not follow the right stick");
    CHECK_EQ(state.circle_x, 0);
}

// Counts the UI actions of one kind that a single press produces. Once, not
// twice: Steam Input presents the pad twice over, and a doubled toggle would
// cancel itself out and look like the button doing nothing.
int ui_actions_from_press(SDL_GameControllerButton button, UiAction wanted) {
    VirtualPad pad;
    if (!pad.attach()) {
        return -1;
    }

    InputManager input;
    input.initialize(Settings());

    input.begin_frame();
    pad.press(button, true);
    SDL_Event event;
    while (SDL_PollEvent(&event) != 0) {
        input.handle_event(event);
    }
    input.end_frame(LayoutRegistry().fallback(), 1280.0f, 800.0f,
                    system_profile(SystemId::Ds).screens);

    int count = 0;
    for (const UiAction action : input.ui_actions()) {
        if (action == wanted) ++count;
    }

    input.shutdown();
    pad.detach();
    return count;
}

// R3 is the smoothing toggle. It has to arrive as a UI action exactly once,
// and must not double as a DS button -- that is what lets it mean the same
// thing in a game as it does in a menu.
void test_stick_click_toggles_filtering() {
    CHECK_EQ(ui_actions_from_press(SDL_CONTROLLER_BUTTON_RIGHTSTICK, UiAction::ToggleFilter), 1);
    CHECK_EQ(ui_actions_from_press(SDL_CONTROLLER_BUTTON_RIGHTSTICK, UiAction::Confirm), 0);
    CHECK_EQ(ui_actions_from_press(SDL_CONTROLLER_BUTTON_A, UiAction::ToggleFilter), 0);
}

// L3 is the layout swap, and the same rules apply as to R3 above: exactly one
// action per press, and no DS button of its own, so a game never loses a press
// to it.
void test_left_stick_click_swaps_the_layout() {
    CHECK_EQ(ui_actions_from_press(SDL_CONTROLLER_BUTTON_LEFTSTICK, UiAction::SwapLayout), 1);
    CHECK_EQ(ui_actions_from_press(SDL_CONTROLLER_BUTTON_LEFTSTICK, UiAction::Confirm), 0);
    // The two stick clicks do not bleed into each other: R3 still only smooths
    // and L3 still only swaps.
    CHECK_EQ(ui_actions_from_press(SDL_CONTROLLER_BUTTON_LEFTSTICK, UiAction::ToggleFilter), 0);
    CHECK_EQ(ui_actions_from_press(SDL_CONTROLLER_BUTTON_RIGHTSTICK, UiAction::SwapLayout), 0);
}

// Pushes the right stick fully right and reads what the game would be handed,
// under one mapping.
DsInputState push_right_stick_and_read(RightStickMode mode) {
    VirtualPad pad;
    if (!pad.attach()) {
        return DsInputState();
    }

    InputManager input;
    input.initialize(Settings());
    input.set_stick_is_dpad(false);  // a 3DS: the console this choice exists for
    input.set_right_stick_mode(mode);

    SDL_JoystickSetVirtualAxis(pad.joystick, SDL_CONTROLLER_AXIS_RIGHTX, 32000);
    SDL_GameControllerUpdate();

    input.begin_frame();
    input.end_frame(LayoutRegistry().fallback(), 1280.0f, 800.0f,
                    system_profile(SystemId::ThreeDs).screens);
    const DsInputState state = input.ds_state();

    input.shutdown();
    pad.detach();
    return state;
}

// The right stick is whatever the game wants it to be. Each mapping has to
// reach exactly one destination: a stick that fed two of them at once is the
// bug that made the 3DS's circle pad unusable when the left stick did it.
void test_the_right_stick_maps_where_it_is_told() {
    const DsInputState c_stick = push_right_stick_and_read(RightStickMode::CStick);
    CHECK_MSG(c_stick.c_stick_x > 16000, "the C-Stick did not follow the right stick");
    CHECK_EQ(c_stick.circle_x, 0);
    CHECK(!c_stick.pressed(DsButton::Right));

    const DsInputState circle_pad = push_right_stick_and_read(RightStickMode::CirclePad);
    CHECK_MSG(circle_pad.circle_x > 16000, "the circle pad did not follow the right stick");
    CHECK_EQ(circle_pad.c_stick_x, 0);
    CHECK(!circle_pad.pressed(DsButton::Right));

    const DsInputState dpad = push_right_stick_and_read(RightStickMode::DPad);
    CHECK_MSG(dpad.pressed(DsButton::Right), "the d-pad did not follow the right stick");
    CHECK(!dpad.pressed(DsButton::Left));
    CHECK_EQ(dpad.c_stick_x, 0);
    CHECK_EQ(dpad.circle_x, 0);

    // Off has to reach nothing at all -- the whole point of it is a game that
    // does something unwanted with a stick it is handed.
    const DsInputState off = push_right_stick_and_read(RightStickMode::Off);
    CHECK_EQ(off.c_stick_x, 0);
    CHECK_EQ(off.circle_x, 0);
    CHECK_EQ(off.buttons, 0u);
}

// Start and Select together open the menu. That chord is the only way in on a
// device with no Steam Input in front of the pad -- a Switch running this as a
// companion -- so it has to fire once for the pair however they are pressed,
// and the game must not also see the two buttons: plenty of DS games read them
// together as a soft reset.
void test_start_and_select_open_the_menu() {
    VirtualPad pad;
    if (!pad.attach()) {
        return;
    }

    InputManager input;
    input.initialize(Settings());

    const auto frame = [&input]() {
        input.begin_frame();
        SDL_Event event;
        while (SDL_PollEvent(&event) != 0) {
            input.handle_event(event);
        }
        input.end_frame(LayoutRegistry().fallback(), 1280.0f, 800.0f,
                        system_profile(SystemId::Ds).screens);
    };
    const auto menu_opens = [&input]() {
        int count = 0;
        for (const UiAction action : input.ui_actions()) {
            if (action == UiAction::ToggleMenu) ++count;
        }
        return count;
    };

    // Select on its own is just Select, and reaches the game as itself.
    pad.press(SDL_CONTROLLER_BUTTON_BACK, true);
    frame();
    CHECK_EQ(menu_opens(), 0);
    CHECK_MSG(input.ds_state().pressed(DsButton::Select), "Select alone did not reach the game");

    // The pair opens the menu, and neither button goes through.
    pad.press(SDL_CONTROLLER_BUTTON_START, true);
    frame();
    CHECK_EQ(menu_opens(), 1);
    CHECK_MSG(!input.ds_state().pressed(DsButton::Start), "Start leaked into the game");
    CHECK_MSG(!input.ds_state().pressed(DsButton::Select), "Select leaked into the game");

    // Still held. Opening once a frame would toggle the menu sixty times a
    // second, which reads as the shortcut not working at all.
    frame();
    CHECK_EQ(menu_opens(), 0);
    frame();
    CHECK_EQ(menu_opens(), 0);

    // Releasing either one arms it again, and the pair fires a second time --
    // which is what makes the same chord close the menu it opened.
    pad.press(SDL_CONTROLLER_BUTTON_START, false);
    frame();
    CHECK_EQ(menu_opens(), 0);
    pad.press(SDL_CONTROLLER_BUTTON_START, true);
    frame();
    CHECK_EQ(menu_opens(), 1);

    input.shutdown();
    pad.detach();
}

// The mock core lights an indicator square per libretro joypad button. Their
// positions come from mock_core.cpp's draw_button_indicators().
struct Indicator {
    const char* name;
    int x;
};
constexpr Indicator kIndicators[] = {
    {"UP", 8},  {"DOWN", 28}, {"LEFT", 48}, {"RIGHT", 68}, {"A", 96},     {"B", 116},
    {"X", 136}, {"Y", 156},   {"L", 184},   {"R", 204},    {"START", 224}, {"SELECT", 240},
};
constexpr int kIndicatorRow = 192 - 24 + 6;  // middle of the 14px-tall squares

std::string mock_core_path() {
    const char* value = std::getenv("DUALSCREEN_MOCK_CORE");
    return value != nullptr ? std::string(value) : std::string();
}

std::string make_fake_rom() {
    char pattern[] = "/tmp/dualscreen-input-XXXXXX";
    const char* directory = mkdtemp(pattern);
    if (directory == nullptr) return std::string();
    const std::string path = path_join(directory, "fake.nds");
    std::ofstream file(path, std::ios::binary);
    file << "rom";
    return path;
}

// Presses one controller button and reports which libretro joypad buttons the
// core actually saw. This is the whole chain: SDL -> mapping -> InputSink ->
// the core's input_state callback -> its framebuffer.
std::string buttons_seen_by_core(SDL_GameControllerButton button, bool& ok) {
    ok = false;
    const std::string core_path = mock_core_path();
    if (core_path.empty()) return "";

    VirtualPad pad;
    if (!pad.attach()) return "";

    InputManager input;
    input.initialize(Settings());

    LibretroCoreScreenSource source;
    LibretroSourceConfig config;
    config.core_path = core_path;
    config.rom_path = make_fake_rom();
    config.system_directory = "/tmp";
    config.save_directory = "/tmp";
    std::string error;
    if (!source.open(config, &error)) {
        pad.detach();
        return "";
    }

    pad.press(button, true);
    input.begin_frame();
    input.end_frame(LayoutRegistry().fallback(), 1280.0f, 800.0f,
                    system_profile(SystemId::Ds).screens);
    source.input_sink().submit(input.ds_state());
    source.advance_frame(&error);

    std::string lit;
    const CoreVideoFrame& frame = source.core_frame_for_test();
    if (frame.valid()) {
        for (const Indicator& indicator : kIndicators) {
            const uint32_t pixel =
                frame.pixels[static_cast<std::size_t>(kIndicatorRow) * frame.stride_pixels +
                             static_cast<std::size_t>(indicator.x) + 6] & 0x00ffffffu;
            if (pixel == 0x00ffff78u) {  // pack(255, 255, 120)
                if (!lit.empty()) lit += ",";
                lit += indicator.name;
            }
        }
        ok = true;
    }

    source.close();
    input.shutdown();
    pad.detach();
    return lit;
}

// The full chain, which the mapping-only tests cannot see: pressing the button
// marked A must reach the core as libretro's B, and nothing else.
void test_face_buttons_reach_the_core() {
    bool ok = false;
    struct Expectation {
        SDL_GameControllerButton pressed;
        const char* expected;
    };
    const Expectation cases[] = {
        {SDL_CONTROLLER_BUTTON_A, "B"},
        {SDL_CONTROLLER_BUTTON_B, "A"},
        {SDL_CONTROLLER_BUTTON_X, "Y"},
        {SDL_CONTROLLER_BUTTON_Y, "X"},
        {SDL_CONTROLLER_BUTTON_DPAD_UP, "UP"},
        {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, "L"},
        {SDL_CONTROLLER_BUTTON_START, "START"},
    };
    for (const Expectation& expectation : cases) {
        const std::string lit = buttons_seen_by_core(expectation.pressed, ok);
        if (!ok) {
            CHECK_MSG(false, "could not run the core");
            return;
        }
        CHECK_MSG(lit == expectation.expected,
                  std::string("pressing SDL button ") +
                      SDL_GameControllerGetStringForButton(expectation.pressed) +
                      " reached the core as [" + lit + "], expected [" + expectation.expected + "]");
    }
}

}  // namespace

int main() {
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, kFaceButtonsByPosition);
    if (SDL_Init(SDL_INIT_GAMECONTROLLER) != 0) {
        std::fprintf(stderr, "skipping: SDL game controller subsystem unavailable: %s\n",
                     SDL_GetError());
        return 0;
    }

    test_face_buttons_follow_ds_positions();
    test_shoulders_and_dpad_are_stable();
    test_the_stick_is_the_dpad_only_where_there_is_no_stick();
    test_stick_click_is_not_a_ds_button();
    test_right_stick_reaches_the_c_stick();
    test_stick_click_toggles_filtering();
    test_left_stick_click_swaps_the_layout();
    test_start_and_select_open_the_menu();
    test_the_right_stick_maps_where_it_is_told();
    test_face_buttons_reach_the_core();

    SDL_Quit();
    return test::finish("controller-input");
}
