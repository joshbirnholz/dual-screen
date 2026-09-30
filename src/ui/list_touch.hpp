#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

#include "input/input_manager.hpp"

namespace ds {

// Where the finger or mouse is right now, and whether it is down. Screens are
// handed presses and releases; a list being dragged also needs where the
// finger has got to in between. Module state rather than an argument threaded
// through every screen, the same shape as prompt_style(): it is one fact about
// the machine, set once a pass by whoever reads the input.
struct LivePointer {
    bool down = false;
    float x = 0.0f;
    float y = 0.0f;
};
void set_live_pointer(bool down, float x, float y);
const LivePointer& live_pointer();

// A tap on a row of a list: the first highlights it, and only a tap on the
// row already highlighted chooses it -- so a finger landing on the wrong row
// moves the cursor there rather than doing something. True when `index` is
// to be chosen; otherwise it becomes the focus.
inline bool tap_chooses(int& focus, int index) {
    if (focus == index) return true;
    focus = index;
    return false;
}

// Dragging a list with a finger. A press on the list starts a gesture; once it
// has moved further than a tap's wobble it scrolls the list with the finger,
// and its release is not a tap.
class ListDrag {
public:
    // Feeds one pass's pointer events. `scroll` is the list's offset in
    // pixels, moved while dragging and kept within [0, max_scroll].
    void update(const std::vector<PointerEvent>& pointers, const PixelRect& list, float& scroll,
                float max_scroll) {
        for (const PointerEvent& pointer : pointers) {
            if (pointer.pressed) {
                active_ = list.contains(pointer.x, pointer.y);
                moved_ = false;
                start_y_ = pointer.y;
                start_scroll_ = scroll;
            }
            if (pointer.released) {
                released_drag_ = active_ && moved_;
                active_ = false;
            }
        }
        const LivePointer& live = live_pointer();
        if (active_ && live.down) {
            const float travel = start_y_ - live.y;
            if (std::fabs(travel) > kThresholdPixels) moved_ = true;
            if (moved_) scroll = std::clamp(start_scroll_ + travel, 0.0f, std::max(0.0f, max_scroll));
        }
    }

    // Whether this release ended a drag, and so is not a tap on a row.
    bool release_was_drag() const { return released_drag_; }
    bool dragging() const { return active_ && moved_; }

    // Clears the last release, once it has been looked at.
    void settle() { released_drag_ = false; }

private:
    static constexpr float kThresholdPixels = 14.0f;
    bool active_ = false;
    bool moved_ = false;
    bool released_drag_ = false;
    float start_y_ = 0.0f;
    float start_scroll_ = 0.0f;
};

}  // namespace ds
