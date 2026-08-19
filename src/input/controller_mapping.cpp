#include "input/controller_mapping.hpp"

namespace ds {

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

}  // namespace ds
