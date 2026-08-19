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

// The stick click must not reach the game at all.
void test_stick_click_is_not_a_ds_button() {
    const DsInputState state = press_and_read(SDL_CONTROLLER_BUTTON_RIGHTSTICK);
    CHECK_EQ(state.buttons, 0u);
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
    input.end_frame(LayoutRegistry().fallback(), 1280.0f, 800.0f);
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
    test_stick_click_is_not_a_ds_button();
    test_face_buttons_reach_the_core();

    SDL_Quit();
    return test::finish("controller-input");
}
