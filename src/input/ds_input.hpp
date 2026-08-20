#pragma once

#include <cstdint>

namespace ds {

// The DS button set, independent of any particular controller or transport.
enum class DsButton {
    A, B, X, Y,
    L, R,
    Start, Select,
    Up, Down, Left, Right,
    Count,
};

const char* to_string(DsButton button);

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
