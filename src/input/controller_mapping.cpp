#include "input/controller_mapping.hpp"

namespace ds {

PadLetterLayout pad_letter_layout(SDL_GameControllerType type) {
    switch (type) {
        case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO:
        case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
        case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
        case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
            return PadLetterLayout::Nintendo;
        default:
            // Everything else prints its letters in Xbox positions -- including
            // the virtual pad Steam Input presents.
            return PadLetterLayout::Xbox;
    }
}

bool face_position_for_button(SDL_GameControllerButton button,
                              PadLetterLayout layout,
                              FacePosition& out_position) {
    const bool nintendo = (layout == PadLetterLayout::Nintendo);
    switch (button) {
        case SDL_CONTROLLER_BUTTON_A:
            out_position = nintendo ? FacePosition::East : FacePosition::South;
            return true;
        case SDL_CONTROLLER_BUTTON_B:
            out_position = nintendo ? FacePosition::South : FacePosition::East;
            return true;
        case SDL_CONTROLLER_BUTTON_X:
            out_position = nintendo ? FacePosition::North : FacePosition::West;
            return true;
        case SDL_CONTROLLER_BUTTON_Y:
            out_position = nintendo ? FacePosition::West : FacePosition::North;
            return true;
        default:
            return false;
    }
}

const char* to_string(GameFaceMapping mapping) {
    return mapping == GameFaceMapping::DsLayout ? "ds_layout" : "match_labels";
}

GameFaceMapping game_face_mapping_from_string(const std::string& text, GameFaceMapping fallback) {
    if (text == "ds_layout") return GameFaceMapping::DsLayout;
    if (text == "match_labels") return GameFaceMapping::MatchLabels;
    // Accepted for the short-lived settings key this replaced.
    if (text == "positions") return GameFaceMapping::DsLayout;
    if (text == "labels") return GameFaceMapping::MatchLabels;
    return fallback;
}

bool ds_button_for_controller_button(SDL_GameControllerButton button,
                                     GameFaceMapping mapping,
                                     PadLetterLayout layout,
                                     DsButton& out_button) {
    // Shoulders, d-pad, Start and Select are unambiguous.
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

    if (mapping == GameFaceMapping::MatchLabels) {
        // SDL hands us letters, and the DS uses the same four.
        switch (button) {
            case SDL_CONTROLLER_BUTTON_A: out_button = DsButton::A; return true;
            case SDL_CONTROLLER_BUTTON_B: out_button = DsButton::B; return true;
            case SDL_CONTROLLER_BUTTON_X: out_button = DsButton::X; return true;
            case SDL_CONTROLLER_BUTTON_Y: out_button = DsButton::Y; return true;
            default: return false;
        }
    }

    // DsLayout: go through physical position, because that is what "matches a
    // DS" means. A DS has B at south, A at east, Y at west and X at north.
    FacePosition position = FacePosition::South;
    if (!face_position_for_button(button, layout, position)) {
        return false;
    }
    switch (position) {
        case FacePosition::South: out_button = DsButton::B; return true;
        case FacePosition::East:  out_button = DsButton::A; return true;
        case FacePosition::West:  out_button = DsButton::Y; return true;
        case FacePosition::North: out_button = DsButton::X; return true;
    }
    return false;
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
