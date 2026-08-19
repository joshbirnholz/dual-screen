#include "input/ds_input.hpp"

namespace ds {

const char* to_string(DsButton button) {
    switch (button) {
        case DsButton::A: return "A";
        case DsButton::B: return "B";
        case DsButton::X: return "X";
        case DsButton::Y: return "Y";
        case DsButton::L: return "L";
        case DsButton::R: return "R";
        case DsButton::Start: return "Start";
        case DsButton::Select: return "Select";
        case DsButton::Up: return "Up";
        case DsButton::Down: return "Down";
        case DsButton::Left: return "Left";
        case DsButton::Right: return "Right";
        case DsButton::Count: break;
    }
    return "?";
}

}  // namespace ds
