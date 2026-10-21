#include "input/button_mapping.hpp"

#include <algorithm>
#include <cmath>

namespace ds {

PadConsole pad_console_for(SystemId system, RightStickMode right_stick) {
    const SystemProfile& profile = system_profile(system);
    PadConsole console;
    console.has_zl_zr = system == SystemId::ThreeDs;
    console.has_microphone = profile.has_microphone;
    console.game_uses_every_control = profile.game_uses_every_control;
    console.right_stick = right_stick;
    return console;
}

PadAction standard_pad_action(PadControl control, const PadConsole& console) {
    switch (control) {
        case PadControl::A: return PadAction::A;
        case PadControl::B: return PadAction::B;
        case PadControl::X: return PadAction::X;
        case PadControl::Y: return PadAction::Y;
        case PadControl::L: return PadAction::L;
        case PadControl::R: return PadAction::R;
        // A console with four shoulder buttons has somewhere for the triggers
        // to go -- a New 3DS's ZL and ZR among them. A DS has none, so they
        // do nothing until given something.
        case PadControl::Zl:
            return console.has_zl_zr || console.game_uses_every_control ? PadAction::Zl : PadAction::Nothing;
        case PadControl::Zr:
            return console.has_zl_zr || console.game_uses_every_control ? PadAction::Zr : PadAction::Nothing;
        case PadControl::Plus: return PadAction::Start;
        case PadControl::Minus: return PadAction::Select;
        case PadControl::Up: return PadAction::Up;
        case PadControl::Down: return PadAction::Down;
        case PadControl::Left: return PadAction::Left;
        case PadControl::Right: return PadAction::Right;
        case PadControl::LeftStick:
            if (console.game_uses_every_control) return PadAction::LeftStickButton;
            // Swapping screens is also in the quick menu, a press away, while
            // blowing has no other way in at all.
            return console.has_microphone ? PadAction::BlowIntoMic : PadAction::SwapScreens;
        case PadControl::RightStick:
            if (console.game_uses_every_control) return PadAction::RightStickButton;
            return console.right_stick == RightStickMode::TouchCursor ? PadAction::TouchCursor
                                                                      : PadAction::Nothing;
        case PadControl::Count: break;
    }
    return PadAction::Nothing;
}

PadAction ButtonMapping::resolved(PadControl control, const PadConsole& console) const {
    const PadAction action = (*this)[control];
    return action == PadAction::Standard ? standard_pad_action(control, console) : action;
}

bool ButtonMapping::is_standard(const PadConsole& console) const {
    for (std::size_t index = 0; index < kPadControls; ++index) {
        const PadControl control = static_cast<PadControl>(index);
        if (resolved(control, console) != standard_pad_action(control, console)) return false;
    }
    return true;
}

std::vector<PadAction> pad_action_choices(const PadConsole& console) {
    std::vector<PadAction> choices = {PadAction::A, PadAction::B, PadAction::X, PadAction::Y,
                                      PadAction::L, PadAction::R};
    if (console.has_zl_zr || console.game_uses_every_control) {
        choices.push_back(PadAction::Zl);
        choices.push_back(PadAction::Zr);
    }
    for (const PadAction action : {PadAction::Start, PadAction::Select, PadAction::Up, PadAction::Down,
                                   PadAction::Left, PadAction::Right}) {
        choices.push_back(action);
    }
    if (console.game_uses_every_control) {
        choices.push_back(PadAction::LeftStickButton);
        choices.push_back(PadAction::RightStickButton);
    }
    if (console.right_stick == RightStickMode::TouchCursor) choices.push_back(PadAction::TouchCursor);
    if (!console.game_uses_every_control) {
        choices.push_back(PadAction::TouchPoint);
        choices.push_back(PadAction::QuickMenu);
        choices.push_back(PadAction::SwapScreens);
        choices.push_back(PadAction::NextLayout);
    }
    if (console.has_microphone) choices.push_back(PadAction::BlowIntoMic);
    choices.push_back(PadAction::Nothing);
    return choices;
}

std::vector<PadAction> turbo_choices(const PadConsole& console) {
    std::vector<PadAction> choices;
    for (const PadAction action : pad_action_choices(console)) {
        DsButton button = DsButton::A;
        if (pad_action_group(action) == PadActionGroup::Console && ds_button_for_pad_action(action, button)) {
            choices.push_back(action);
        }
    }
    return choices;
}

bool touch_analog_stick_spot(int16_t stick_x, int16_t stick_y, float deadzone, const TouchSpot& centre,
                             float radius, float aspect, TouchSpot& out) {
    const float push_x = stick_x / 32767.0f;
    const float push_y = stick_y / 32767.0f;
    const float length = std::sqrt(push_x * push_x + push_y * push_y);
    if (length <= deadzone || length <= 0.0f) return false;
    // From the centre itself at the deadzone's edge, so a push starts as a
    // touch on the stick and drags out from there.
    const float past = std::min(1.0f, (length - deadzone) / std::max(0.01f, 1.0f - deadzone));
    const float reach = past * radius;
    out.u = std::clamp(centre.u + push_x / length * reach / std::max(0.1f, aspect), 0.0f, 1.0f);
    out.v = std::clamp(centre.v + push_y / length * reach, 0.0f, 1.0f);
    return true;
}

bool turbo_down_at(uint32_t ms) {
    constexpr uint32_t kHalfPeriodMs = 33;
    return (ms / kHalfPeriodMs) % 2 == 0;
}

PadActionGroup pad_action_group(PadAction action) {
    switch (action) {
        case PadAction::TouchCursor:
        case PadAction::TouchPoint: return PadActionGroup::TouchScreen;
        case PadAction::QuickMenu:
        case PadAction::SwapScreens:
        case PadAction::NextLayout:
        case PadAction::ScreenSmoothing:
        case PadAction::BlowIntoMic:
        case PadAction::Turbo:
        case PadAction::Nothing: return PadActionGroup::Dualscreen;
        default: return PadActionGroup::Console;
    }
}

bool ds_button_for_pad_action(PadAction action, DsButton& out) {
    switch (action) {
        case PadAction::A: out = DsButton::A; return true;
        case PadAction::B: out = DsButton::B; return true;
        case PadAction::X: out = DsButton::X; return true;
        case PadAction::Y: out = DsButton::Y; return true;
        case PadAction::L: out = DsButton::L; return true;
        case PadAction::R: out = DsButton::R; return true;
        case PadAction::Zl: out = DsButton::Zl; return true;
        case PadAction::Zr: out = DsButton::Zr; return true;
        case PadAction::Start: out = DsButton::Start; return true;
        case PadAction::Select: out = DsButton::Select; return true;
        case PadAction::Up: out = DsButton::Up; return true;
        case PadAction::Down: out = DsButton::Down; return true;
        case PadAction::Left: out = DsButton::Left; return true;
        case PadAction::Right: out = DsButton::Right; return true;
        // melonDS DS listens for its microphone on L3.
        case PadAction::LeftStickButton:
        case PadAction::BlowIntoMic: out = DsButton::LeftStick; return true;
        // Carried as R3 to the app, which touches with it -- see
        // App::Impl::apply_stylus_cursor().
        case PadAction::RightStickButton:
        case PadAction::TouchCursor: out = DsButton::RightStick; return true;
        default: return false;
    }
}

bool pad_action_is_shortcut(PadAction action) {
    return action == PadAction::QuickMenu || action == PadAction::SwapScreens ||
           action == PadAction::NextLayout || action == PadAction::ScreenSmoothing;
}

const char* pad_control_name(PadControl control) {
    switch (control) {
        case PadControl::A: return "A Button";
        case PadControl::B: return "B Button";
        case PadControl::X: return "X Button";
        case PadControl::Y: return "Y Button";
        case PadControl::L: return "L Button";
        case PadControl::R: return "R Button";
        case PadControl::Zl: return "ZL Button";
        case PadControl::Zr: return "ZR Button";
        case PadControl::Plus: return "Plus Button";
        case PadControl::Minus: return "Minus Button";
        case PadControl::LeftStick: return "Left Stick Button";
        case PadControl::RightStick: return "Right Stick Button";
        case PadControl::Up: return "Up Button";
        case PadControl::Down: return "Down Button";
        case PadControl::Left: return "Left Button";
        case PadControl::Right: return "Right Button";
        case PadControl::Count: break;
    }
    return "?";
}

const char* pad_action_name(PadAction action) {
    switch (action) {
        case PadAction::Standard: return "Standard";
        case PadAction::A: return "A Button";
        case PadAction::B: return "B Button";
        case PadAction::X: return "X Button";
        case PadAction::Y: return "Y Button";
        case PadAction::L: return "L Button";
        case PadAction::R: return "R Button";
        case PadAction::Zl: return "ZL Button";
        case PadAction::Zr: return "ZR Button";
        case PadAction::Start: return "Start Button";
        case PadAction::Select: return "Select Button";
        case PadAction::Up: return "Up Button";
        case PadAction::Down: return "Down Button";
        case PadAction::Left: return "Left Button";
        case PadAction::Right: return "Right Button";
        case PadAction::LeftStickButton: return "Left Stick Button";
        case PadAction::RightStickButton: return "Right Stick Button";
        case PadAction::TouchCursor: return "Activate Touch Cursor";
        case PadAction::QuickMenu: return "Open Menu";
        case PadAction::SwapScreens: return "Swap Screens";
        case PadAction::ScreenSmoothing: return "Screen Smoothing";
        case PadAction::BlowIntoMic: return "Blow into Mic";
        case PadAction::Nothing: return "Disabled";
        case PadAction::NextLayout: return "Next Screen Layout";
        case PadAction::Turbo: return "Turbo";
        case PadAction::TouchPoint: return "Touch Point";
        case PadAction::Count: break;
    }
    return "?";
}

const char* pad_action_key(PadAction action) {
    switch (action) {
        case PadAction::Standard: return "standard";
        case PadAction::A: return "a";
        case PadAction::B: return "b";
        case PadAction::X: return "x";
        case PadAction::Y: return "y";
        case PadAction::L: return "l";
        case PadAction::R: return "r";
        case PadAction::Zl: return "zl";
        case PadAction::Zr: return "zr";
        case PadAction::Start: return "start";
        case PadAction::Select: return "select";
        case PadAction::Up: return "up";
        case PadAction::Down: return "down";
        case PadAction::Left: return "left";
        case PadAction::Right: return "right";
        case PadAction::LeftStickButton: return "l3";
        case PadAction::RightStickButton: return "r3";
        case PadAction::TouchCursor: return "touch_cursor";
        case PadAction::QuickMenu: return "quick_menu";
        case PadAction::SwapScreens: return "swap_screens";
        case PadAction::ScreenSmoothing: return "screen_smoothing";
        case PadAction::BlowIntoMic: return "microphone";
        case PadAction::Nothing: return "nothing";
        case PadAction::NextLayout: return "next_layout";
        case PadAction::Turbo: return "turbo";
        case PadAction::TouchPoint: return "touch_point";
        case PadAction::Count: break;
    }
    return "standard";
}

PadAction pad_action_from_key(const std::string& key) {
    for (int index = 0; index < static_cast<int>(PadAction::Count); ++index) {
        const PadAction action = static_cast<PadAction>(index);
        if (action == PadAction::ScreenSmoothing) continue;
        if (key == pad_action_key(action)) return action;
    }
    return PadAction::Standard;
}

const char* pad_control_key(PadControl control) {
    switch (control) {
        case PadControl::A: return "a";
        case PadControl::B: return "b";
        case PadControl::X: return "x";
        case PadControl::Y: return "y";
        case PadControl::L: return "l";
        case PadControl::R: return "r";
        case PadControl::Zl: return "zl";
        case PadControl::Zr: return "zr";
        case PadControl::Plus: return "plus";
        case PadControl::Minus: return "minus";
        case PadControl::LeftStick: return "l3";
        case PadControl::RightStick: return "r3";
        case PadControl::Up: return "up";
        case PadControl::Down: return "down";
        case PadControl::Left: return "left";
        case PadControl::Right: return "right";
        case PadControl::Count: break;
    }
    return "";
}

bool pad_control_from_key(const std::string& key, PadControl& out) {
    for (std::size_t index = 0; index < kPadControls; ++index) {
        const PadControl control = static_cast<PadControl>(index);
        if (key == pad_control_key(control)) {
            out = control;
            return true;
        }
    }
    return false;
}

}  // namespace ds
