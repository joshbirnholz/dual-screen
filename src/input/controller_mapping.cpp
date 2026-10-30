#include "input/controller_mapping.hpp"

#include <cctype>

namespace ds {

void apply_controller_hints() {
    // SDL's default is to report face buttons by printed label; this app wants
    // positions, so that one rule -- east means A -- holds in menus and in
    // game on every pad. See kFaceButtonsByPosition.
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, kFaceButtonsByPosition);

    // And read the pad even while this app's window is not the one in front.
    //
    // SDL's default is not to, and it means it. SDL_PrivateJoystickButton and
    // its axis and sensor counterparts return *before* writing the new value
    // into the joystick's own state whenever this process has windows and none
    // of them has keyboard focus. So this is not a matter of missing events
    // and polling instead: the polled state is never updated either. Buttons
    // read as up, sticks stay wherever they were last allowed to be, and
    // sensors report nothing at all. Releases are still let through, which is
    // the one mercy -- nothing sticks down.
    //
    // That default is right for an ordinary application and wrong for this
    // one, whose whole job for a Wii U is to run beside another program and
    // hand it a pad. Cemu's window is the one in front by design -- it is
    // what the player is looking at -- so this app spends every Wii U session
    // in the background. A companion's pad arrives over a socket and never
    // noticed, which is why two devices always worked; on one device this
    // app's own pad is the GamePad, and it was reading a pad SDL had stopped
    // filling in.
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
}

bool ds_button_for_controller_button(SDL_GameControllerButton button, DsButton& out_button) {
    switch (button) {
        // Face buttons, by position. SDL is configured to report positions, so
        // BUTTON_A is the south button on every pad.
        case SDL_CONTROLLER_BUTTON_A: out_button = DsButton::B; return true;  // south
        case SDL_CONTROLLER_BUTTON_B: out_button = DsButton::A; return true;  // east
        case SDL_CONTROLLER_BUTTON_X: out_button = DsButton::Y; return true;  // west
        case SDL_CONTROLLER_BUTTON_Y: out_button = DsButton::X; return true;  // north

        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: out_button = DsButton::L; return true;
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: out_button = DsButton::R; return true;
        case SDL_CONTROLLER_BUTTON_START: out_button = DsButton::Start; return true;
        case SDL_CONTROLLER_BUTTON_BACK: out_button = DsButton::Select; return true;
        case SDL_CONTROLLER_BUTTON_DPAD_UP: out_button = DsButton::Up; return true;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: out_button = DsButton::Down; return true;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT: out_button = DsButton::Left; return true;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: out_button = DsButton::Right; return true;
        default: return false;
    }
}

const char* to_string(ConfirmButton button) {
    switch (button) {
        case ConfirmButton::East: return "East";
        case ConfirmButton::South: return "South";
        case ConfirmButton::Count: break;
    }
    return "East";
}

bool confirm_button_from_string(const std::string& text, ConfirmButton& out) {
    for (int index = 0; index < static_cast<int>(ConfirmButton::Count); ++index) {
        const ConfirmButton choice = static_cast<ConfirmButton>(index);
        if (text == to_string(choice)) {
            out = choice;
            return true;
        }
    }
    return false;
}

// SDL is asked to report face buttons by position, so BUTTON_B is the east
// one and BUTTON_A the south one on every pad -- see kFaceButtonsByPosition.
// These two names are the only place in this app where that is relied on
// rather than merely true.
SDL_GameControllerButton confirm_button_for(ConfirmButton choice) {
    return choice == ConfirmButton::South ? SDL_CONTROLLER_BUTTON_A : SDL_CONTROLLER_BUTTON_B;
}

SDL_GameControllerButton cancel_button_for(ConfirmButton choice) {
    return choice == ConfirmButton::South ? SDL_CONTROLLER_BUTTON_B : SDL_CONTROLLER_BUTTON_A;
}

PadLetterLayout pad_letter_layout(SDL_GameControllerType type) {
    switch (type) {
        case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO:
        case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
        case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
        case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
            return PadLetterLayout::Nintendo;
        default:
            return PadLetterLayout::Xbox;
    }
}

bool steam_pad_has_nintendo_letters(bool presented_by_steam, SDL_GameControllerType type,
                                    const char* name) {
    if (!presented_by_steam) return false;
    if (pad_letter_layout(type) == PadLetterLayout::Nintendo) return true;
    if (name == nullptr) return false;
    std::string lowered(name);
    for (char& c : lowered) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (const char* word : {"nintendo", "switch", "joy-con", "joycon"}) {
        if (lowered.find(word) != std::string::npos) return true;
    }
    return false;
}

SDL_GameControllerButton face_button_as_read(SDL_GameControllerButton button, bool presented_by_steam,
                                             SDL_GameControllerType type, const char* name) {
    if (!steam_pad_has_nintendo_letters(presented_by_steam, type, name)) return button;
    switch (button) {
        case SDL_CONTROLLER_BUTTON_A: return SDL_CONTROLLER_BUTTON_B;  // A: east
        case SDL_CONTROLLER_BUTTON_B: return SDL_CONTROLLER_BUTTON_A;  // B: south
        case SDL_CONTROLLER_BUTTON_X: return SDL_CONTROLLER_BUTTON_Y;  // X: north
        case SDL_CONTROLLER_BUTTON_Y: return SDL_CONTROLLER_BUTTON_X;  // Y: west
        default: return button;
    }
}

MenuButtonLabels menu_button_labels(PadLetterLayout layout) {
    MenuButtonLabels labels;
    if (layout == PadLetterLayout::Nintendo) {
        // A Nintendo pad already prints A on the east button, so the letters
        // read the same way round as the positions.
        labels.confirm = "A";
        labels.cancel = "B";
    } else {
        labels.confirm = "B";
        labels.cancel = "A";
    }
    return labels;
}

const char* to_string(SDL_GameControllerType type) {
    switch (type) {
        case SDL_CONTROLLER_TYPE_XBOX360: return "Xbox 360";
        case SDL_CONTROLLER_TYPE_XBOXONE: return "Xbox One";
        case SDL_CONTROLLER_TYPE_PS3: return "PS3";
        case SDL_CONTROLLER_TYPE_PS4: return "PS4";
        case SDL_CONTROLLER_TYPE_PS5: return "PS5";
        case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO: return "Switch Pro";
        case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_LEFT: return "Joy-Con (left)";
        case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT: return "Joy-Con (right)";
        case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_PAIR: return "Joy-Con (pair)";
        case SDL_CONTROLLER_TYPE_VIRTUAL: return "virtual";
        case SDL_CONTROLLER_TYPE_AMAZON_LUNA: return "Luna";
        case SDL_CONTROLLER_TYPE_GOOGLE_STADIA: return "Stadia";
        case SDL_CONTROLLER_TYPE_NVIDIA_SHIELD: return "Shield";
        case SDL_CONTROLLER_TYPE_UNKNOWN:
        default: return "unknown";
    }
}

const char* to_string(RightStickMode mode) {
    switch (mode) {
        case RightStickMode::CStick: return "C-Stick";
        case RightStickMode::CirclePad: return "Circle Pad";
        case RightStickMode::DPad: return "D-Pad";
        case RightStickMode::TouchCursor: return "Touch Cursor";
        case RightStickMode::TouchAnalog: return "Touch Analog Stick";
        case RightStickMode::Off: break;
    }
    return "Off";
}

std::vector<RightStickMode> right_stick_modes(bool console_has_stick) {
    if (!console_has_stick) return {RightStickMode::TouchCursor, RightStickMode::TouchAnalog, RightStickMode::Off};
    return {RightStickMode::CStick,      RightStickMode::CirclePad,   RightStickMode::DPad,
            RightStickMode::TouchCursor, RightStickMode::TouchAnalog, RightStickMode::Off};
}

RightStickMode next_right_stick_mode(RightStickMode mode, bool console_has_stick) {
    const std::vector<RightStickMode> modes = right_stick_modes(console_has_stick);
    for (std::size_t index = 0; index < modes.size(); ++index) {
        if (modes[index] == mode) return modes[(index + 1) % modes.size()];
    }
    return modes.front();
}

RightStickMode right_stick_mode_for(RightStickMode mode, bool console_has_stick) {
    const std::vector<RightStickMode> modes = right_stick_modes(console_has_stick);
    for (const RightStickMode offered : modes) {
        if (offered == mode) return mode;
    }
    return modes.front();
}

const char* right_stick_mode_key(RightStickMode mode) {
    switch (mode) {
        case RightStickMode::CStick: return "c_stick";
        case RightStickMode::CirclePad: return "circle_pad";
        case RightStickMode::DPad: return "dpad";
        case RightStickMode::TouchCursor: return "touch_cursor";
        case RightStickMode::TouchAnalog: return "touch_analog";
        case RightStickMode::Off: break;
    }
    return "off";
}

RightStickMode right_stick_mode_from_key(const std::string& key) {
    if (key == "circle_pad") return RightStickMode::CirclePad;
    if (key == "dpad") return RightStickMode::DPad;
    if (key == "touch_cursor") return RightStickMode::TouchCursor;
    if (key == "touch_analog") return RightStickMode::TouchAnalog;
    if (key == "off") return RightStickMode::Off;
    return RightStickMode::CStick;
}

}  // namespace ds
