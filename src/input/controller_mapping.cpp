#include "input/controller_mapping.hpp"

namespace ds {

const char* to_string(FaceButtonConvention convention) {
    return convention == FaceButtonConvention::Labels ? "labels" : "positions";
}

FaceButtonConvention face_button_convention_from_string(const std::string& text,
                                                        FaceButtonConvention fallback) {
    if (text == "labels") return FaceButtonConvention::Labels;
    if (text == "positions") return FaceButtonConvention::Positions;
    return fallback;
}

const char* sdl_button_labels_hint(FaceButtonConvention convention) {
    return convention == FaceButtonConvention::Labels ? "1" : "0";
}

bool ds_button_for_controller_button(SDL_GameControllerButton button,
                                     FaceButtonConvention convention,
                                     DsButton& out_button) {
    // Shoulders, d-pad, Start and Select mean the same thing either way.
    switch (button) {
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: out_button = DsButton::L; return true;
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: out_button = DsButton::R; return true;
        case SDL_CONTROLLER_BUTTON_START: out_button = DsButton::Start; return true;
        case SDL_CONTROLLER_BUTTON_BACK: out_button = DsButton::Select; return true;
        case SDL_CONTROLLER_BUTTON_DPAD_UP: out_button = DsButton::Up; return true;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: out_button = DsButton::Down; return true;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT: out_button = DsButton::Left; return true;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: out_button = DsButton::Right; return true;
        default: break;
    }

    if (convention == FaceButtonConvention::Labels) {
        // SDL is handing us labels, and DS buttons carry the same four letters.
        switch (button) {
            case SDL_CONTROLLER_BUTTON_A: out_button = DsButton::A; return true;
            case SDL_CONTROLLER_BUTTON_B: out_button = DsButton::B; return true;
            case SDL_CONTROLLER_BUTTON_X: out_button = DsButton::X; return true;
            case SDL_CONTROLLER_BUTTON_Y: out_button = DsButton::Y; return true;
            default: return false;
        }
    }

    // SDL is handing us positions (A = south). A DS has B at south, A at east,
    // Y at west and X at north.
    switch (button) {
        case SDL_CONTROLLER_BUTTON_A: out_button = DsButton::B; return true;  // south
        case SDL_CONTROLLER_BUTTON_B: out_button = DsButton::A; return true;  // east
        case SDL_CONTROLLER_BUTTON_X: out_button = DsButton::Y; return true;  // west
        case SDL_CONTROLLER_BUTTON_Y: out_button = DsButton::X; return true;  // north
        default: return false;
    }
}

SDL_GameControllerButton confirm_button(FaceButtonConvention convention) {
    // Whichever SDL button ends up being the player's "A".
    return convention == FaceButtonConvention::Labels ? SDL_CONTROLLER_BUTTON_A
                                                      : SDL_CONTROLLER_BUTTON_B;
}

SDL_GameControllerButton cancel_button(FaceButtonConvention convention) {
    return convention == FaceButtonConvention::Labels ? SDL_CONTROLLER_BUTTON_B
                                                      : SDL_CONTROLLER_BUTTON_A;
}

const char* confirm_button_label(FaceButtonConvention convention) {
    // In Positions mode SDL's names are positional, so the button we use for
    // confirm is the east one - printed "B" on an Xbox-layout pad.
    return convention == FaceButtonConvention::Labels ? "A" : "B";
}

const char* cancel_button_label(FaceButtonConvention convention) {
    return convention == FaceButtonConvention::Labels ? "B" : "A";
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
