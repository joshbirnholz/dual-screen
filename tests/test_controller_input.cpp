// Drives a real SDL virtual game controller through InputManager, so the whole
// path -- SDL button -> mapping -> DS button state -- is exercised without any
// hardware. This is the level the face-button bug actually lived at: the table
// was right in isolation and the wrong one was being selected.

#include "test_support.hpp"

#include <SDL.h>

#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include <algorithm>
#include <cmath>

#include "config/paths.hpp"
#include "config/settings.hpp"
#include "core/libretro_screen_source.hpp"
#include "input/controller_mapping.hpp"
#include "input/ds_input.hpp"
#include "input/input_manager.hpp"
#include "input/motion.hpp"
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

DsInputState press_and_read(SDL_GameControllerButton button, bool microphone = false) {
    VirtualPad pad;
    if (!pad.attach()) {
        return DsInputState();
    }

    InputManager input;
    input.initialize(Settings());
    input.set_microphone_button(microphone);

    pad.press(button, true);
    input.begin_frame();
    input.end_frame(LayoutRegistry().fallback(), 1280.0f, 800.0f,
                    system_profile(SystemId::Ds).screens);
    const DsInputState state = input.ds_state();

    input.shutdown();
    pad.detach();
    return state;
}

// Each pad on its own: what one holds is not the other's, and they come in
// the order they are listed -- Steam's order, under Steam Input -- with no
// keyboard among them. See input/pad_routing.hpp.
void test_each_pad_is_read_on_its_own_in_order() {
    VirtualPad first;
    VirtualPad second;
    if (!first.attach() || !second.attach()) {
        first.detach();
        second.detach();
        CHECK_MSG(false, "could not attach two virtual pads");
        return;
    }
    InputManager input;
    input.initialize(Settings());
    second.press(SDL_CONTROLLER_BUTTON_A, true);
    input.begin_frame();
    input.end_frame(LayoutRegistry().fallback(), 1280.0f, 800.0f,
                    system_profile(SystemId::WiiU).screens);

    const std::vector<PadReading> pads = input.pad_readings();
    CHECK_EQ(pads.size(), static_cast<std::size_t>(2));
    if (pads.size() == 2) {
        CHECK_MSG(pads[0].id < pads[1].id, "the pads did not come in the order they were listed");
        CHECK_MSG(pads[0].held.buttons == 0, "the first pad was read holding the second's press");
        CHECK_MSG(pads[1].held.buttons != 0, "the second pad's press was lost");
    }
    // And merged, the way every console that shares one pad still reads it.
    CHECK(input.ds_state().buttons == (pads.size() == 2 ? pads[1].held.buttons : 0u));

    // Nothing held while a menu has game input switched off.
    input.set_game_input_enabled(false);
    for (const PadReading& reading : input.pad_readings()) {
        CHECK_MSG(reading.held.buttons == 0, "a pad reported a press while a menu was up");
    }

    input.shutdown();
    second.detach();
    first.detach();
}

// Steam Input's virtual gamepad, as SDL sees one: Valve's vendor and the
// virtual gamepad's product. SDL hides these unless asked not to, which Steam
// does for every game it starts.
struct SteamPad : VirtualPad {
    bool attach_as_steam() {
        SDL_SetHint("SDL_GAMECONTROLLER_ALLOW_STEAM_VIRTUAL_GAMEPAD", "1");
        SDL_VirtualJoystickDesc desc;
        SDL_zero(desc);
        desc.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
        desc.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
        desc.naxes = 6;
        desc.nbuttons = SDL_CONTROLLER_BUTTON_MAX;
        desc.vendor_id = 0x28de;
        desc.product_id = 0x11ff;
        desc.name = "Steam Virtual Gamepad";
        device_index = SDL_JoystickAttachVirtualEx(&desc);
        if (device_index < 0) return false;
        SDL_GameControllerUpdate();
        joystick = SDL_JoystickOpen(device_index);
        return joystick != nullptr;
    }
};

// What a press on a Steam Input pad comes out as, in the game and in a menu.
struct SteamPress {
    DsInputState held;
    std::vector<UiAction> actions;
    bool attached = false;
};

SteamPress press_on_steam_pad(SDL_GameControllerButton button, ConfirmButton confirm) {
    SteamPress out;
    SteamPad pad;
    if (!pad.attach_as_steam()) {
        pad.detach();
        return out;
    }
    out.attached = true;
    InputManager input;
    input.initialize(Settings());
    input.set_confirm_button(confirm);
    pad.press(button, true);
    input.begin_frame();
    SDL_Event event;
    while (SDL_PollEvent(&event) != 0) input.handle_event(event);
    input.end_frame(LayoutRegistry().fallback(), 1280.0f, 800.0f, system_profile(SystemId::Ds).screens);
    out.held = input.ds_state();
    out.actions = input.ui_actions();
    input.shutdown();
    pad.detach();
    return out;
}

bool has_action(const std::vector<UiAction>& actions, UiAction wanted) {
    for (const UiAction action : actions) {
        if (action == wanted) return true;
    }
    return false;
}

// A Steam Input pad is read exactly as Steam presents it, the same as any
// other pad: its A is the button at the bottom, B the one on the right, as
// SDL names them -- whatever the controller underneath prints, and whatever
// the player mapped in Steam. Confirm/Cancel alone says which of those two
// answers yes.
void test_steam_input_pads_are_read_as_steam_presents_them() {
    const struct {
        SDL_GameControllerButton pressed;
        DsButton gives;
    } kPlaces[] = {
        {SDL_CONTROLLER_BUTTON_A, DsButton::B},
        {SDL_CONTROLLER_BUTTON_B, DsButton::A},
        {SDL_CONTROLLER_BUTTON_X, DsButton::Y},
        {SDL_CONTROLLER_BUTTON_Y, DsButton::X},
    };
    for (const auto& place : kPlaces) {
        const SteamPress press = press_on_steam_pad(place.pressed, ConfirmButton::East);
        if (!press.attached) {
            std::printf("# skipped: SDL here cannot present a Steam pad\n");
            return;
        }
        CHECK_MSG(press.held.pressed(place.gives),
                  std::string("Steam's ") + SDL_GameControllerGetStringForButton(place.pressed) +
                      " did not reach the game in its place");
    }
    CHECK(has_action(press_on_steam_pad(SDL_CONTROLLER_BUTTON_B, ConfirmButton::East).actions,
                     UiAction::Confirm));
    CHECK(has_action(press_on_steam_pad(SDL_CONTROLLER_BUTTON_A, ConfirmButton::East).actions,
                     UiAction::Cancel));
    CHECK(has_action(press_on_steam_pad(SDL_CONTROLLER_BUTTON_A, ConfirmButton::South).actions,
                     UiAction::Confirm));
    CHECK(has_action(press_on_steam_pad(SDL_CONTROLLER_BUTTON_B, ConfirmButton::South).actions,
                     UiAction::Cancel));
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

// The stick clicks are this app's own, and must not reach the game -- on a
// console that has nothing for them to be.
void test_stick_click_is_not_a_ds_button() {
    CHECK_EQ(press_and_read(SDL_CONTROLLER_BUTTON_RIGHTSTICK).buttons, 0u);
    CHECK_EQ(press_and_read(SDL_CONTROLLER_BUTTON_LEFTSTICK).buttons, 0u);
}

// Except on a DS, where L3 is the microphone. A few games will not let you
// past a room without being blown into, and by the time this app has taken the
// stick presses for itself a DS has no spare button left to give -- so this
// one is given back. It reaches the core as L3, which is where melonDS DS
// 1.3.1 listens: its own input descriptor for port 0 id 14 is "Microphone".
void test_the_left_stick_is_the_microphone_on_a_console_that_has_one() {
    const DsInputState blown = press_and_read(SDL_CONTROLLER_BUTTON_LEFTSTICK, true);
    CHECK(blown.pressed(DsButton::LeftStick));

    // The right one is untouched: it is still this app's, and a console with a
    // microphone does not suddenly have two.
    CHECK_EQ(press_and_read(SDL_CONTROLLER_BUTTON_RIGHTSTICK, true).buttons, 0u);

    // And nothing else arrives with it. A button that also pressed something
    // the game reads would be worse than no button at all.
    DsInputState only_mic;
    only_mic.set(DsButton::LeftStick, true);
    CHECK_EQ(blown.buttons, only_mic.buttons);
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

// Counts the UI actions of one kind that one press and release produces. Once,
// not twice: Steam Input presents the pad twice over, and a doubled toggle
// would cancel itself out and look like the button doing nothing.
//
// The whole gesture rather than just the press, because the stick buttons only
// become their own shortcut on release -- until then either may turn out to be
// half of the menu chord.
int ui_actions_from_press(SDL_GameControllerButton button, UiAction wanted) {
    VirtualPad pad;
    if (!pad.attach()) {
        return -1;
    }

    InputManager input;
    input.initialize(Settings());

    int count = 0;
    const auto frame = [&]() {
        input.begin_frame();
        SDL_Event event;
        while (SDL_PollEvent(&event) != 0) {
            input.handle_event(event);
        }
        input.end_frame(LayoutRegistry().fallback(), 1280.0f, 800.0f,
                        system_profile(SystemId::Ds).screens);
        for (const UiAction action : input.ui_actions()) {
            if (action == wanted) ++count;
        }
    };

    pad.press(button, true);
    frame();
    pad.press(button, false);
    frame();

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

// L3 and R3 together open the menu. That chord is the only way in on a device
// with no Steam Input in front of the pad -- a Switch or an Android handheld
// running this as a companion -- so it has to fire once for the pair however
// they are pressed.
//
// Apart the two stick buttons keep their own meanings, so reaching for the
// chord must not also swap the layout or toggle smoothing on the way in:
// whichever is pressed first is only itself if it is released alone.
//
// Neither reaches a DS or 3DS button, which is why the pair costs a game
// nothing -- Start and Select, which this replaced, are read together as a
// soft reset by plenty of DS games.
void test_the_stick_buttons_open_the_menu_together() {
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
    const auto count_of = [&input](UiAction wanted) {
        int count = 0;
        for (const UiAction action : input.ui_actions()) {
            if (action == wanted) ++count;
        }
        return count;
    };
    const auto menu_opens = [&]() { return count_of(UiAction::ToggleMenu); };

    // Held on its own, a stick button is not yet anything: it may still be
    // half of the chord.
    pad.press(SDL_CONTROLLER_BUTTON_LEFTSTICK, true);
    frame();
    CHECK_EQ(menu_opens(), 0);
    CHECK_EQ(count_of(UiAction::SwapLayout), 0);

    // The pair opens the menu, and neither one's own action fires.
    pad.press(SDL_CONTROLLER_BUTTON_RIGHTSTICK, true);
    frame();
    CHECK_EQ(menu_opens(), 1);
    CHECK_EQ(count_of(UiAction::SwapLayout), 0);
    CHECK_EQ(count_of(UiAction::ToggleFilter), 0);
    CHECK_MSG(input.ds_state().buttons == 0u, "a stick button reached the game");

    // Still held. Opening once a frame would toggle the menu sixty times a
    // second, which reads as the shortcut not working at all.
    frame();
    CHECK_EQ(menu_opens(), 0);
    frame();
    CHECK_EQ(menu_opens(), 0);

    // Letting go of the pair must not leave either shortcut behind it.
    pad.press(SDL_CONTROLLER_BUTTON_LEFTSTICK, false);
    frame();
    CHECK_EQ(count_of(UiAction::SwapLayout), 0);
    pad.press(SDL_CONTROLLER_BUTTON_RIGHTSTICK, false);
    frame();
    CHECK_EQ(count_of(UiAction::ToggleFilter), 0);
    CHECK_EQ(menu_opens(), 0);

    // Alone again, each one is its own shortcut -- on release, which is when
    // it is known that the other never joined it.
    pad.press(SDL_CONTROLLER_BUTTON_LEFTSTICK, true);
    frame();
    CHECK_EQ(count_of(UiAction::SwapLayout), 0);
    pad.press(SDL_CONTROLLER_BUTTON_LEFTSTICK, false);
    frame();
    CHECK_EQ(count_of(UiAction::SwapLayout), 1);
    CHECK_EQ(menu_opens(), 0);

    pad.press(SDL_CONTROLLER_BUTTON_RIGHTSTICK, true);
    frame();
    pad.press(SDL_CONTROLLER_BUTTON_RIGHTSTICK, false);
    frame();
    CHECK_EQ(count_of(UiAction::ToggleFilter), 1);

    // And the chord fires again, which is what closes the menu it opened.
    pad.press(SDL_CONTROLLER_BUTTON_LEFTSTICK, true);
    pad.press(SDL_CONTROLLER_BUTTON_RIGHTSTICK, true);
    frame();
    CHECK_EQ(menu_opens(), 1);

    // Start and Select are ordinary buttons again, and reach the game.
    pad.press(SDL_CONTROLLER_BUTTON_LEFTSTICK, false);
    pad.press(SDL_CONTROLLER_BUTTON_RIGHTSTICK, false);
    frame();
    pad.press(SDL_CONTROLLER_BUTTON_START, true);
    pad.press(SDL_CONTROLLER_BUTTON_BACK, true);
    frame();
    CHECK_EQ(menu_opens(), 0);
    CHECK_MSG(input.ds_state().pressed(DsButton::Start), "Start did not reach the game");
    CHECK_MSG(input.ds_state().pressed(DsButton::Select), "Select did not reach the game");

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

// A phone reports one tap as a finger and again as a mouse synthesized from
// that finger. The two arrive in different units -- touch normalized to the
// window, the mouse in pixels -- and a widget only answers a tap that both
// began and ended inside it, so a release carrying the wrong press position is
// a menu whose buttons do nothing.
void test_a_tap_reported_twice_still_knows_where_it_started() {
    InputManager input;
    input.initialize(Settings());

    constexpr float kWidth = 1280.0f;
    constexpr float kHeight = 800.0f;
    // Middle of the window, which is where a tile would be.
    constexpr float kFingerX = 0.5f;
    constexpr float kFingerY = 0.5f;

    const auto finger = [](uint32_t type, float x, float y) {
        SDL_Event event {};
        event.type = type;
        event.tfinger.touchId = 1;
        event.tfinger.fingerId = 7;
        event.tfinger.x = x;
        event.tfinger.y = y;
        return event;
    };
    const auto synthetic_mouse = [](uint32_t type, int x, int y) {
        SDL_Event event {};
        event.type = type;
        event.button.which = SDL_TOUCH_MOUSEID;
        event.button.button = SDL_BUTTON_LEFT;
        event.button.x = x;
        event.button.y = y;
        return event;
    };

    input.begin_frame();
    input.handle_event(finger(SDL_FINGERDOWN, kFingerX, kFingerY));
    input.handle_event(synthetic_mouse(SDL_MOUSEBUTTONDOWN, static_cast<int>(kFingerX * kWidth),
                                       static_cast<int>(kFingerY * kHeight)));
    input.handle_event(synthetic_mouse(SDL_MOUSEBUTTONUP, static_cast<int>(kFingerX * kWidth),
                                       static_cast<int>(kFingerY * kHeight)));
    input.handle_event(finger(SDL_FINGERUP, kFingerX, kFingerY));
    input.end_frame(LayoutRegistry().fallback(), kWidth, kHeight,
                    system_profile(SystemId::Ds).screens);

    // The tile the tap was aimed at: a generous box around where it landed.
    const PixelRect tile{kFingerX * kWidth - 100.0f, kFingerY * kHeight - 60.0f, 200.0f, 120.0f};
    int answered = 0;
    int releases = 0;
    for (const PointerEvent& pointer : input.pointer_events()) {
        if (pointer.released) ++releases;
        if (tapped(pointer, tile)) ++answered;
    }
    CHECK_MSG(releases == 1,
              "one tap arrived as " + std::to_string(releases) +
                  " releases, so every menu press counts more than once");
    CHECK_MSG(answered == 1,
              "a tap in the middle of a tile was not read as a tap on it, so the menu's buttons "
              "do nothing");

    // A real mouse alongside a real touchscreen, clicking somewhere else while
    // the finger is still down. Each source has to remember its own press: one
    // slot between them means the finger's release reports where the mouse went
    // down, in the wrong units on top of that.
    input.begin_frame();
    input.handle_event(finger(SDL_FINGERDOWN, kFingerX, kFingerY));
    SDL_Event click {};
    click.type = SDL_MOUSEBUTTONDOWN;
    click.button.which = 0;
    click.button.button = SDL_BUTTON_LEFT;
    click.button.x = 20;
    click.button.y = 20;
    input.handle_event(click);
    input.handle_event(finger(SDL_FINGERUP, kFingerX, kFingerY));
    input.end_frame(LayoutRegistry().fallback(), kWidth, kHeight,
                    system_profile(SystemId::Ds).screens);

    bool finger_answered = false;
    for (const PointerEvent& pointer : input.pointer_events()) {
        if (pointer.released && tapped(pointer, tile)) finger_answered = true;
    }
    CHECK_MSG(finger_answered,
              "a finger's release carried somebody else's press position, so the tile it was "
              "lifted from did not answer it");
    input.shutdown();
}

// Two fingers tapped together is the only way into the menu on a device with
// no buttons, so it has to keep working -- not once per session, and not until
// the window loses focus mid-touch, which on a handheld is what opening the
// system overlay does.
//
// It fires on the release, not on the landing. A hand resting on the glass
// while the other one draws is not a tap, and on the screen a stylus belongs
// to that is a thing people do. tests/test_touch_gesture.cpp covers the shape
// of the gesture itself; what is here is that it survives the things that
// used to break it.
void test_two_fingers_open_the_menu_more_than_once() {
    InputManager input;
    input.initialize(Settings());

    const auto finger = [](uint32_t type, SDL_FingerID id, float x, float y) {
        SDL_Event event {};
        event.type = type;
        event.tfinger.touchId = 1;
        event.tfinger.fingerId = id;
        event.tfinger.x = x;
        event.tfinger.y = y;
        return event;
    };
    const auto opened_the_menu = [&]() {
        for (const UiAction action : input.ui_actions()) {
            if (action == UiAction::ToggleMenu) return true;
        }
        return false;
    };
    const auto frame = [&](const std::vector<SDL_Event>& events) {
        input.begin_frame();
        for (const SDL_Event& event : events) input.handle_event(event);
        input.end_frame(LayoutRegistry().fallback(), 1280.0f, 800.0f,
                        system_profile(SystemId::Ds).screens);
        return opened_the_menu();
    };

    // Landing is not the gesture.
    CHECK_MSG(
        !frame({finger(SDL_FINGERDOWN, 1, 0.3f, 0.5f), finger(SDL_FINGERDOWN, 2, 0.6f, 0.5f)}),
        "the menu opened the moment a second finger landed, so a hand resting on the screen "
        "opens it");
    CHECK_MSG(frame({finger(SDL_FINGERUP, 2, 0.6f, 0.5f)}),
              "lifting the two fingers did not open the menu");

    // The first stays put -- a thumb resting on the screen. The gesture has to
    // be available again without lifting everything.
    frame({finger(SDL_FINGERDOWN, 3, 0.6f, 0.5f)});
    CHECK_MSG(frame({finger(SDL_FINGERUP, 3, 0.6f, 0.5f)}),
              "two fingers opened the menu once and then never again while one stayed down");

    // The window is taken away mid-touch and the releases go elsewhere. What was
    // down is not down any more, whatever this side was last told.
    SDL_Event lost {};
    lost.type = SDL_WINDOWEVENT;
    lost.window.event = SDL_WINDOWEVENT_FOCUS_LOST;
    frame({lost});
    frame({finger(SDL_FINGERDOWN, 4, 0.3f, 0.5f), finger(SDL_FINGERDOWN, 5, 0.6f, 0.5f)});
    CHECK_MSG(frame({finger(SDL_FINGERUP, 5, 0.6f, 0.5f)}),
              "the fingers left behind by a lost window were still counted, so the gesture never "
              "worked again");

    // And on a console played through its touch screen too, which it used to
    // refuse. No console this streams has a multi-touch screen -- a DS, a 3DS
    // and a GamePad are all single-touch, and the wire carries one point -- so
    // a second finger was never a press the game could have read. Refusing it
    // there left a device with no buttons no way into the menu at all, which
    // is to say no way to leave a session.
    frame({finger(SDL_FINGERUP, 4, 0.3f, 0.5f)});
    input.set_game_owns_controls(true);
    frame({finger(SDL_FINGERDOWN, 6, 0.3f, 0.5f), finger(SDL_FINGERDOWN, 7, 0.6f, 0.5f)});
    CHECK_MSG(frame({finger(SDL_FINGERUP, 7, 0.6f, 0.5f)}),
              "two fingers did not open the menu on a console whose game reads every control, "
              "leaving a device with no buttons no way out of the session");

    input.shutdown();
}

// A Wii U game reads both sticks being pressed and plays through its touch
// screen. A shell that swaps a layout under every L3, or opens a menu on a
// second finger, has taken the console out of the player's hands -- so where
// the game speaks for every control, this stops answering all of them and the
// menu moves to the keyboard.
void test_the_shell_lets_go_of_the_controls_where_the_game_wants_them() {
    VirtualPad pad;
    if (!pad.attach()) {
        test::skip("no virtual controller available");
        return;
    }
    InputManager input;
    input.initialize(Settings());
    SDL_Event event;
    while (SDL_PollEvent(&event) != 0) input.handle_event(event);

    const auto press_and_release = [&](const std::vector<SDL_GameControllerButton>& buttons) {
        std::vector<UiAction> seen;
        for (const bool down : {true, false}) {
            for (const SDL_GameControllerButton button : buttons) pad.press(button, down);
            input.begin_frame();
            SDL_Event pending;
            while (SDL_PollEvent(&pending) != 0) input.handle_event(pending);
            input.end_frame(LayoutRegistry().fallback(), 1280.0f, 800.0f,
                            system_profile(SystemId::Ds).screens);
            for (const UiAction action : input.ui_actions()) seen.push_back(action);
        }
        return seen;
    };
    const auto contains = [](const std::vector<UiAction>& actions, UiAction wanted) {
        return std::find(actions.begin(), actions.end(), wanted) != actions.end();
    };

    CHECK_MSG(contains(press_and_release({SDL_CONTROLLER_BUTTON_LEFTSTICK}), UiAction::SwapLayout),
              "L3 stopped swapping on a console whose game does not read it");

    input.set_game_owns_controls(true);
    CHECK_MSG(!contains(press_and_release({SDL_CONTROLLER_BUTTON_LEFTSTICK}), UiAction::SwapLayout),
              "L3 still swapped the layout on a console whose game reads it, so every press of it "
              "in a game moves the picture");
    CHECK_MSG(
        !contains(press_and_release({SDL_CONTROLLER_BUTTON_RIGHTSTICK}), UiAction::ToggleFilter),
        "R3 still toggled smoothing on a console whose game reads it");

    // Nor the pair. The menu is reached from the keyboard here, which a player
    // points Steam Input at -- and which is the only control a console with two
    // spoken-for sticks and a played-through touch screen has left.
    CHECK_MSG(!contains(press_and_release({SDL_CONTROLLER_BUTTON_LEFTSTICK,
                                           SDL_CONTROLLER_BUTTON_RIGHTSTICK}),
                        UiAction::ToggleMenu),
              "both sticks together still opened the menu on a console whose game reads them");
    // Letting go of the pair must not then fire the two singles it was made of.
    CHECK_MSG(input.ui_actions().empty() ||
                  (!contains(input.ui_actions(), UiAction::SwapLayout) &&
                   !contains(input.ui_actions(), UiAction::ToggleFilter)),
              "releasing both sticks fired the singles the chord was made of");

    input.shutdown();
    pad.detach();
}

// A pad has four shoulder buttons and two sticks that press; a DS has two
// shoulder buttons and no sticks at all. So on a handheld the triggers stand in
// for the shoulder buttons a player is likely to reach past, and the stick
// presses are this app's own shortcuts. On a console that reads all of them the
// same physical controls have somewhere of their own to go, and taking any of
// them for this app would be taking them from the game.
void test_the_extra_shoulders_and_stick_presses_go_where_the_console_has_room() {
    VirtualPad pad;
    if (!pad.attach()) {
        test::skip("no virtual controller available");
        return;
    }
    InputManager input;
    input.initialize(Settings());

    const auto read = [&]() {
        SDL_GameControllerUpdate();
        input.begin_frame();
        input.end_frame(LayoutRegistry().fallback(), 1280.0f, 800.0f,
                        system_profile(SystemId::Ds).screens);
        return input.ds_state();
    };
    const auto hold_triggers = [&](bool down) {
        const int16_t value = down ? 32000 : 0;
        SDL_JoystickSetVirtualAxis(pad.joystick, SDL_CONTROLLER_AXIS_TRIGGERLEFT, value);
        SDL_JoystickSetVirtualAxis(pad.joystick, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, value);
    };

    hold_triggers(true);
    DsInputState state = read();
    CHECK_MSG(state.pressed(DsButton::L) && state.pressed(DsButton::R),
              "the triggers stopped standing in for the shoulder buttons on a handheld");
    CHECK_MSG(!state.pressed(DsButton::Zl) && !state.pressed(DsButton::Zr),
              "a handheld was given buttons it does not have");

    input.set_game_owns_controls(true);
    state = read();
    CHECK_MSG(state.pressed(DsButton::Zl) && state.pressed(DsButton::Zr),
              "the triggers did not reach a console that has ZL and ZR of its own");
    CHECK_MSG(!state.pressed(DsButton::L) && !state.pressed(DsButton::R),
              "the triggers doubled as the shoulder buttons on a console where the shoulder "
              "buttons are their own controls, so one pull pressed two things");

    hold_triggers(false);
    pad.press(SDL_CONTROLLER_BUTTON_LEFTSTICK, true);
    pad.press(SDL_CONTROLLER_BUTTON_RIGHTSTICK, true);
    state = read();
    CHECK_MSG(state.pressed(DsButton::LeftStick) && state.pressed(DsButton::RightStick),
              "the stick presses did not reach a game that reads them");

    input.set_game_owns_controls(false);
    state = read();
    CHECK_MSG(!state.pressed(DsButton::LeftStick) && !state.pressed(DsButton::RightStick),
              "the stick presses reached a game on a console where they are this app's own "
              "shortcuts, so every shortcut is also a press in the game");

    input.shutdown();
    pad.detach();
}

namespace {

// A Switch's six-axis frame, which took a long time and several wrong answers
// to pin down. See switch_accel_to_sdl().
void test_a_switch_reading_becomes_the_sdl_reading_for_the_same_pose() {
    // The three poses are not made up: a Steam Deck, whose motion has been
    // correct throughout, was held against the console in each of them, and
    // what it read is what these assert. That is the whole point -- every
    // earlier version of this test asserted what the reading *ought* to be,
    // derived from the frame libnx was assumed to use, and a derivation
    // cannot catch a wrong assumption about the frame it starts from.

    // Resting flat on a table, face up. Up is out of the console's face, and
    // in the frame everything downstream uses -- a controller's, not a
    // phone's, see DsMotionState -- that is +y. The Deck read (0.30, 9.88,
    // 0.04) here.
    const SwitchSixAxis flat = switch_accel_to_sdl({0.00f, 0.00f, -9.81f});
    CHECK(test::nearly_equal(flat.x, 0.0));
    CHECK(test::nearly_equal(flat.y, 9.81));
    CHECK(test::nearly_equal(flat.z, 0.0));
    CHECK_MSG(std::abs(flat.y) > std::abs(flat.z),
              "gravity must land on the face-normal axis while the console lies flat, or the "
              "frame is a phone's rather than a controller's -- which is the mistake that made "
              "aiming work while up was a quarter turn out");

    // Held upright, screen facing you. Up is now towards the top edge, which
    // is -z. The Deck read (0.80, 0.67, -9.69).
    const SwitchSixAxis upright = switch_accel_to_sdl({0.00f, -9.61f, -2.06f});
    CHECK(test::nearly_equal(upright.y, 2.06));
    CHECK(test::nearly_equal(upright.z, -9.61));
    CHECK_MSG(std::abs(upright.z) > std::abs(upright.y),
              "gravity must not land on the face-normal axis while the console is held "
              "upright, because that is a console lying on a table");

    // Upright with the right-hand edge dipped: the pose the other two cannot
    // see, and the one that settles x. Both of them leave x near zero. Dip the
    // right-hand edge and the ground leans right, so libnx reads +x and this
    // must read -x. Getting it backwards mirrors roll, and mirrors every
    // rotation answer fitted against it. The Deck read (-5.72, 1.18, -7.80).
    const SwitchSixAxis rolled = switch_accel_to_sdl({0.60f, -0.74f, -0.34f});
    CHECK_MSG(rolled.x < 0.0f,
              "dipping the right-hand edge has to put up over to the left, or roll is mirrored");
    CHECK(test::nearly_equal(rolled.x, -0.60));
    CHECK(test::nearly_equal(rolled.y, 0.34));
    CHECK(test::nearly_equal(rolled.z, -0.74));
}

// Rotation, and the third retraction this problem has produced.
//
// The version of this test that stood while the answer was still unknown
// asserted that rotation was neither acceleration's conversion nor its
// negative -- "the two shortcuts that look right and are not". The second half
// of that was wrong. Rotation *is* acceleration's negative here, and it is not
// a coincidence: Cemu's DSU and SDL readers have exactly that relationship to
// each other, and so does Azahar's SDL path. Three independent places, all
// handing an accelerometer and a gyroscope from the same part over pointing
// opposite ways.
//
// So the assertion is inverted, and it is now a positive claim with three
// sources behind it rather than a hunch about what would be too convenient.
void test_rotation_is_accelerations_conversion_turned_around() {
    const SwitchSixAxis native = {0.4f, -1.1f, 2.3f};
    const SwitchSixAxis as_accel = switch_accel_to_sdl(native);
    const SwitchSixAxis as_gyro = switch_gyro_to_sdl(native);
    CHECK(test::nearly_equal(as_gyro.x, -as_accel.x));
    CHECK(test::nearly_equal(as_gyro.y, -as_accel.y));
    CHECK(test::nearly_equal(as_gyro.z, -as_accel.z));

    // Which for this console means a quarter turn about x and nothing else,
    // the same one acceleration gets: libnx lays its axes along the screen and
    // everything downstream lays them along the face. Worth stating outright,
    // because for a long time this read "no axis of it needed touching" -- an
    // answer that was consistent with the accelerometer it was fitted against
    // and a quarter turn from the truth, along with it.
    CHECK(test::nearly_equal(as_gyro.x, native.x));
    CHECK(test::nearly_equal(as_gyro.y, native.z));
    CHECK(test::nearly_equal(as_gyro.z, -native.y));
}

// What SDL has to be told before a pad is opened, and the half of it that
// costs a hardware round-trip to notice is missing.
//
// A face-button convention that is not applied is visible on the first press
// of the first menu. Background events are not visible at all until this app
// is behind another program's window -- which for a Wii U it always is, since
// Cemu's window is the one the player is looking at -- and then the pad
// simply reads as untouched. Not stale, not laggy: SDL declines to write the
// new value into the joystick's own state while unfocused, so polling it is
// no escape from it either.
//
// That was found on hardware, on the only configuration that exposes it:
// playing a Wii U on one device, where this app's own pad is the GamePad
// rather than a second machine's.
void test_a_pad_is_read_even_while_another_window_is_in_front() {
    apply_controller_hints();
    const char* background = SDL_GetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS);
    CHECK_MSG(background != nullptr && std::string(background) == "1",
              "SDL was not told to keep reading the pad while this app is in the background, so a "
              "Wii U played on one device reads a pad that is never pressed");
    const char* labels = SDL_GetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS);
    CHECK_MSG(labels != nullptr && std::string(labels) == std::string(kFaceButtonsByPosition),
              "SDL was not told to report face buttons by position");
}

}  // namespace

// A Wii U's GamePad is one player's controller, and it belongs to whichever
// device is showing its screen.
//
// Found on a Steam Deck showing the television with a Switch joined as the
// GamePad: every controller on the Deck, set up in Cemu as a Pro Controller,
// was also pressing the GamePad's buttons -- because the host merged its own
// pads into the GamePad as it does for a DS, which two people share.
void test_only_the_device_showing_the_gamepad_controls_it() {
    using namespace ds;

    const SystemProfile& wii_u = system_profile(SystemId::WiiU);
    CHECK(wii_u.pad_belongs_to_its_screen);
    CHECK(!system_profile(SystemId::Ds).pad_belongs_to_its_screen);
    CHECK(!system_profile(SystemId::ThreeDs).pad_belongs_to_its_screen);

    DsInputState deck;
    deck.set(DsButton::A, true);
    deck.circle_x = 20000;
    deck.touching = true;
    deck.touch_x = 10;
    deck.touch_y = 20;
    deck.motion.present = true;
    deck.motion.gyro_x = 1.0f;
    DsInputState switch_pad;
    switch_pad.set(DsButton::B, true);
    switch_pad.circle_y = -12000;
    switch_pad.motion.present = true;
    switch_pad.motion.gyro_y = 2.0f;

    // The Switch took the GamePad's screen: its controls and nothing else.
    const PadOwner companion_gamepad =
        console_pad_owner(wii_u.pad_belongs_to_its_screen, true, true);
    CHECK(companion_gamepad == PadOwner::Companion);
    const DsInputState to_gamepad = input_for_the_console_pad(deck, switch_pad, companion_gamepad);
    CHECK_MSG(!to_gamepad.pressed(DsButton::A), "the host's button reached the companion's GamePad");
    CHECK(to_gamepad.pressed(DsButton::B));
    CHECK_MSG(to_gamepad.circle_x == 0, "the host's stick reached the companion's GamePad");
    CHECK(to_gamepad.circle_y == -12000);
    CHECK_MSG(!to_gamepad.touching, "the host's touch reached the companion's GamePad");
    CHECK(to_gamepad.motion.gyro_y == 2.0f && to_gamepad.motion.gyro_x == 0.0f);

    // The other way round: the companion is the television and this device
    // holds the GamePad. Still one device, and now it is this one.
    const PadOwner host_gamepad = console_pad_owner(wii_u.pad_belongs_to_its_screen, true, false);
    CHECK(host_gamepad == PadOwner::ThisDevice);
    const DsInputState held_here = input_for_the_console_pad(deck, switch_pad, host_gamepad);
    CHECK(held_here.pressed(DsButton::A));
    CHECK(!held_here.pressed(DsButton::B));
    CHECK(held_here.circle_y == 0);
    CHECK(held_here.motion.gyro_x == 1.0f);

    // Nobody joined: this device's, whatever the console.
    CHECK(console_pad_owner(true, false, false) == PadOwner::ThisDevice);
    CHECK(console_pad_owner(false, false, false) == PadOwner::ThisDevice);

    // A DS is shared, and stays merged: either player presses anything.
    const PadOwner ds_pad = console_pad_owner(false, true, true);
    CHECK(ds_pad == PadOwner::Shared);
    const DsInputState shared = input_for_the_console_pad(deck, switch_pad, ds_pad);
    CHECK(shared.pressed(DsButton::A) && shared.pressed(DsButton::B));
    CHECK(shared.circle_x == 20000 && shared.circle_y == -12000);
    CHECK(shared.touching && shared.touch_x == 10);
    // The companion's motion wins when it has one, as before.
    CHECK(shared.motion.gyro_y == 2.0f);
}

// Holding a direction keeps moving through a menu: one step for the press,
// nothing more for a moment -- so a single press is a single step -- and then
// a steady run until it is let go.
void test_holding_a_direction_keeps_moving() {
    VirtualPad pad;
    if (!pad.attach()) {
        CHECK_MSG(false, "could not attach a virtual pad");
        return;
    }
    InputManager input;
    input.initialize(Settings());
    int downs = 0;
    const auto frame = [&]() {
        input.begin_frame();
        SDL_Event event;
        while (SDL_PollEvent(&event) != 0) {
            input.handle_event(event);
        }
        input.end_frame(LayoutRegistry().fallback(), 1280.0f, 800.0f,
                        system_profile(SystemId::Ds).screens);
        for (const UiAction action : input.ui_actions()) {
            if (action == UiAction::Down) ++downs;
        }
    };

    pad.press(SDL_CONTROLLER_BUTTON_DPAD_DOWN, true);
    const uint32_t pressed = SDL_GetTicks();
    while (SDL_GetTicks() - pressed < 300) {
        frame();
        SDL_Delay(10);
    }
    CHECK_MSG(downs == 1, "a press held briefly was more than one step: " + std::to_string(downs));
    while (SDL_GetTicks() - pressed < 900) {
        frame();
        SDL_Delay(10);
    }
    CHECK_MSG(downs >= 4, "holding the d-pad did not keep moving: " + std::to_string(downs));
    pad.press(SDL_CONTROLLER_BUTTON_DPAD_DOWN, false);
    frame();
    const int released_at = downs;
    for (int index = 0; index < 30; ++index) {
        frame();
        SDL_Delay(10);
    }
    CHECK_MSG(downs == released_at, "letting go did not stop it");

    input.shutdown();
    pad.detach();
}

int main() {
    test_only_the_device_showing_the_gamepad_controls_it();
    test_a_switch_reading_becomes_the_sdl_reading_for_the_same_pose();
    test_rotation_is_accelerations_conversion_turned_around();
    test_a_pad_is_read_even_while_another_window_is_in_front();
    if (SDL_Init(SDL_INIT_GAMECONTROLLER) != 0) {
        std::fprintf(stderr, "skipping: SDL game controller subsystem unavailable: %s\n",
                     SDL_GetError());
        return 0;
    }

    test_each_pad_is_read_on_its_own_in_order();
    test_holding_a_direction_keeps_moving();
    test_steam_input_pads_are_read_as_steam_presents_them();
    test_face_buttons_follow_ds_positions();
    test_shoulders_and_dpad_are_stable();
    test_the_stick_is_the_dpad_only_where_there_is_no_stick();
    test_stick_click_is_not_a_ds_button();
    test_the_left_stick_is_the_microphone_on_a_console_that_has_one();
    test_right_stick_reaches_the_c_stick();
    test_stick_click_toggles_filtering();
    test_left_stick_click_swaps_the_layout();
    test_the_stick_buttons_open_the_menu_together();
    test_the_right_stick_maps_where_it_is_told();
    test_face_buttons_reach_the_core();
    test_a_tap_reported_twice_still_knows_where_it_started();
    test_two_fingers_open_the_menu_more_than_once();
    test_the_shell_lets_go_of_the_controls_where_the_game_wants_them();
    test_the_extra_shoulders_and_stick_presses_go_where_the_console_has_room();

    SDL_Quit();
    return test::finish("controller-input");
}
