#pragma once

#include <cstdint>

namespace ds {

// Every button any console this app plays has, independent of any particular
// controller or transport. A DS has the first twelve and no more; the four
// after them are a Wii U GamePad's, which has two more shoulder buttons and
// two sticks that press.
//
// Which physical control produces which of these is not fixed here -- see
// SystemProfile::game_uses_every_control. On a console that leaves its stick
// presses alone they are this app's shortcuts and the triggers stand in for
// the shoulder buttons; on one that uses everything they are the game's.
enum class DsButton {
    A, B, X, Y,
    L, R,
    Start, Select,
    Up, Down, Left, Right,
    Zl, Zr,
    LeftStick, RightStick,
    Count,
};

const char* to_string(DsButton button);

// A moment's worth of a device being moved: how fast it is turning, and which
// way gravity is pulling on it. Together those are enough to work out how a
// thing is being held, which is what a Wii U game asking for the GamePad's
// motion actually wants.
//
// Deliberately raw, and deliberately in SDL's units and axes: gyro in radians
// per second, acceleration in metres per second squared, right-handed with
// +x right, +y up and +z towards the player.
//
// Which is SDL's phrasing, and it is worth spelling out, because reading it
// the other sensible way cost this app several rounds on a Switch. SDL
// defines these axes for "phones held in portrait mode and game controllers
// held in front of you" -- and a controller's "up" is out of its top *face*,
// not towards its top edge. So for anything held like a controller, which is
// everything here:
//
//   flat on a table, face up     up is out of the face, so gravity is on +y
//   held upright, screen at you  up is towards the top edge, so -z
//
// A device with a screen on it is both a phone and a controller and has to
// pick one; this picks the controller, because that is what a Wii U GamePad
// is and what Cemu's sensor fusion expects. A Steam Deck read through DSU
// lands here already and has been correct throughout; a Switch read through
// libnx arrives in the phone reading of the same words and is turned a
// quarter turn on the way in -- see switch_accel_to_sdl().
//
// Nothing here integrates them
// into an orientation, because whatever consumes this already knows how --
// Cemu has its own sensor fusion, tuned to what a real GamePad reports, and
// two integrations disagreeing about which way "up" is would be far worse
// than none. See ExternalInput's own motion fields.
struct DsMotionState {
    // False on a device with no sensors, and on one whose sensors nothing has
    // asked for -- a phone's gyro costs battery to leave running, so it is
    // only turned on when the far end says it is being read. Everything below
    // is meaningless when this is false.
    bool present = false;
    float gyro_x = 0.0f;
    float gyro_y = 0.0f;
    float gyro_z = 0.0f;
    float accel_x = 0.0f;
    float accel_y = 0.0f;
    float accel_z = 0.0f;
    // When the sample was taken, by the sending device's own clock, in
    // microseconds. Sent rather than the interval between samples so that a
    // sample lost on the way -- these ride UDP like everything else on that
    // channel -- widens the next interval instead of vanishing from the
    // integration entirely. Unsigned subtraction gives the right answer
    // across the wrap this has every seventy-one minutes.
    uint32_t sampled_us = 0;

    bool operator==(const DsMotionState& other) const {
        return present == other.present && gyro_x == other.gyro_x && gyro_y == other.gyro_y &&
               gyro_z == other.gyro_z && accel_x == other.accel_x && accel_y == other.accel_y &&
               accel_z == other.accel_z && sampled_us == other.sampled_us;
    }
    bool operator!=(const DsMotionState& other) const { return !(*this == other); }
};

// One frame's worth of DS input. Touch is always in DS screen space
// (256x192), never in window or layout space — mapping happens before this
// struct is filled in, which is what lets a future network-delivered companion
// input stream drop straight into the same sink.
struct DsInputState {
    uint32_t buttons = 0;
    bool touching = false;
    int touch_x = 0;
    int touch_y = 0;
    // The circle pad, in libretro's analog range [-0x7fff, 0x7fff]. A DS has no
    // analog stick and ignores these; a 3DS is played with them, so a game like
    // A Link Between Worlds cannot be moved without them.
    int16_t circle_x = 0;
    int16_t circle_y = 0;
    // The right stick, standing in for a Circle Pad Pro or New 3DS's C-Stick.
    // Only the handful of 3DS games built for one of those read it at all; a
    // DS and a plain Old 3DS both ignore it, the same as the circle pad does
    // on a DS.
    int16_t c_stick_x = 0;
    int16_t c_stick_y = 0;
    // How the device is being held and moved -- see DsMotionState. Absent on
    // a device without sensors, or one nothing has asked for motion from.
    DsMotionState motion;

    bool pressed(DsButton button) const {
        return (buttons & (1u << static_cast<unsigned>(button))) != 0u;
    }
    void set(DsButton button, bool down) {
        const uint32_t mask = 1u << static_cast<unsigned>(button);
        buttons = down ? (buttons | mask) : (buttons & ~mask);
    }
    void clear() {
        buttons = 0;
        touching = false;
        touch_x = 0;
        touch_y = 0;
        circle_x = 0;
        circle_y = 0;
        c_stick_x = 0;
        c_stick_y = 0;
        motion = DsMotionState();
    }
};

// Seam: something that accepts DS button state and a touch point in DS
// coordinate space and forwards it into the running core.
//
// V1's only implementation reads local SDL controller and touch events. A
// future companion device's input arrives over the network and is submitted
// through this same interface, with nothing downstream aware of the difference.
class InputSink {
public:
    virtual ~InputSink() = default;
    virtual void submit(const DsInputState& state) = 0;
};

}  // namespace ds
