#include "input/button_mapping.hpp"

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
        // to go. One with two takes them as a second L and R, since plenty of
        // people reach for the triggers first.
        case PadControl::Zl: return console.game_uses_every_control ? PadAction::Zl : PadAction::L;
        case PadControl::Zr: return console.game_uses_every_control ? PadAction::Zr : PadAction::R;
        case PadControl::Plus: return PadAction::Start;
        case PadControl::Minus: return PadAction::Select;
        case PadControl::LeftStick:
            if (console.game_uses_every_control) return PadAction::LeftStickButton;
            // Swapping screens is also in the quick menu, a press away, while
            // blowing has no other way in at all.
            return console.has_microphone ? PadAction::BlowIntoMic : PadAction::SwapScreens;
        case PadControl::RightStick:
            if (console.game_uses_every_control) return PadAction::RightStickButton;
            return console.right_stick == RightStickMode::TouchCursor ? PadAction::TouchCursor
                                                                      : PadAction::ScreenSmoothing;
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
        choices.push_back(PadAction::QuickMenu);
        choices.push_back(PadAction::SwapScreens);
        choices.push_back(PadAction::ScreenSmoothing);
    }
    if (console.has_microphone) choices.push_back(PadAction::BlowIntoMic);
    choices.push_back(PadAction::Nothing);
    return choices;
}

PadActionGroup pad_action_group(PadAction action) {
    switch (action) {
        case PadAction::TouchCursor: return PadActionGroup::TouchScreen;
        case PadAction::QuickMenu:
        case PadAction::SwapScreens:
        case PadAction::ScreenSmoothing:
        case PadAction::BlowIntoMic:
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
           action == PadAction::ScreenSmoothing;
}

const char* pad_control_name(PadControl control) {
    switch (control) {
        case PadControl::A: return "A";
        case PadControl::B: return "B";
        case PadControl::X: return "X";
        case PadControl::Y: return "Y";
        case PadControl::L: return "L";
        case PadControl::R: return "R";
        case PadControl::Zl: return "ZL";
        case PadControl::Zr: return "ZR";
        case PadControl::Plus: return "Plus";
        case PadControl::Minus: return "Minus";
        case PadControl::LeftStick: return "Left Stick Button";
        case PadControl::RightStick: return "Right Stick Button";
        case PadControl::Count: break;
    }
    return "?";
}

const char* pad_action_name(PadAction action) {
    switch (action) {
        case PadAction::Standard: return "Standard";
        case PadAction::A: return "A";
        case PadAction::B: return "B";
        case PadAction::X: return "X";
        case PadAction::Y: return "Y";
        case PadAction::L: return "L";
        case PadAction::R: return "R";
        case PadAction::Zl: return "ZL";
        case PadAction::Zr: return "ZR";
        case PadAction::Start: return "Start";
        case PadAction::Select: return "Select";
        case PadAction::Up: return "Up";
        case PadAction::Down: return "Down";
        case PadAction::Left: return "Left";
        case PadAction::Right: return "Right";
        case PadAction::LeftStickButton: return "Left Stick Button";
        case PadAction::RightStickButton: return "Right Stick Button";
        case PadAction::TouchCursor: return "Activate Touch Cursor";
        case PadAction::QuickMenu: return "Quick Menu";
        case PadAction::SwapScreens: return "Swap Screens";
        case PadAction::ScreenSmoothing: return "Screen Smoothing";
        case PadAction::BlowIntoMic: return "Blow into Mic";
        case PadAction::Nothing: return "Nothing";
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
        case PadAction::Count: break;
    }
    return "standard";
}

PadAction pad_action_from_key(const std::string& key) {
    for (int index = 0; index < static_cast<int>(PadAction::Count); ++index) {
        const PadAction action = static_cast<PadAction>(index);
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
