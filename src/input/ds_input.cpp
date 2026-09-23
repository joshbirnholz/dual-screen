#include "input/ds_input.hpp"

#include <cstdlib>

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
        case DsButton::Zl: return "ZL";
        case DsButton::Zr: return "ZR";
        case DsButton::LeftStick: return "L3";
        case DsButton::RightStick: return "R3";
        case DsButton::Count: break;
    }
    return "?";
}


PadOwner console_pad_owner(bool pad_belongs_to_its_screen, bool companion_connected,
                           bool companion_shows_pad_screen) {
    if (!companion_connected) {
        return PadOwner::ThisDevice;
    }
    if (!pad_belongs_to_its_screen) {
        return PadOwner::Shared;
    }
    return companion_shows_pad_screen ? PadOwner::Companion : PadOwner::ThisDevice;
}

DsInputState input_for_the_console_pad(const DsInputState& here, const DsInputState& companion,
                                       PadOwner owner) {
    switch (owner) {
        case PadOwner::ThisDevice: return here;
        case PadOwner::Companion: return companion;
        case PadOwner::Shared: break;
    }
    DsInputState combined = here;
    combined.buttons |= companion.buttons;
    // Whichever stick is pushed further wins, so a companion's idle stick does
    // not cancel out the host's, or the other way round.
    const auto further = [](int16_t mine, int16_t theirs) {
        return std::abs(static_cast<int>(theirs)) > std::abs(static_cast<int>(mine)) ? theirs : mine;
    };
    combined.circle_x = further(combined.circle_x, companion.circle_x);
    combined.circle_y = further(combined.circle_y, companion.circle_y);
    combined.c_stick_x = further(combined.c_stick_x, companion.c_stick_x);
    combined.c_stick_y = further(combined.c_stick_y, companion.c_stick_y);
    if (companion.touching && !combined.touching) {
        combined.touching = true;
        combined.touch_x = companion.touch_x;
        combined.touch_y = companion.touch_y;
    }
    // Motion is the one thing that is not merged. Two devices' buttons can be
    // pressed at once and the union is sensible; two devices' orientations
    // cannot be added up into anything. The companion's wins outright when it
    // has one.
    if (companion.motion.present) {
        combined.motion = companion.motion;
    }
    return combined;
}

const char* pad_owner_name(PadOwner owner) {
    switch (owner) {
        case PadOwner::Shared: return "both devices";
        case PadOwner::ThisDevice: return "this device";
        case PadOwner::Companion: return "the companion";
    }
    return "?";
}

}  // namespace ds
