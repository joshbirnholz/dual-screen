#pragma once

#include <algorithm>
#include <chrono>
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
    // Where it went down, while it is down: a press only counts for what it
    // both started and ends on.
    float press_x = 0.0f;
    float press_y = 0.0f;
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

// Dragging a list with a finger, the way a phone's lists move. A press on the
// list starts a gesture; once it has moved further than a tap's wobble the
// list follows the finger pixel for pixel, and when the finger lifts while
// still moving the list carries on at that speed and slows to a stop. A
// finger put down on a list still gliding stops it, and that touch is not a
// tap on whatever was under it.
class ListDrag {
public:
    // Feeds one pass: its pointer events, and the list's offset in pixels,
    // moved while dragged or gliding and kept within [0, max_scroll]. Called
    // every pass the list is on screen, pointer events or not -- a glide
    // moves between them.
    void update(const std::vector<PointerEvent>& pointers, const PixelRect& list, float& scroll,
                float max_scroll) {
        const auto now = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(now - last_update_).count();
        if (!started_ || dt < 0.0f) dt = 0.0f;
        dt = std::min(dt, 0.1f);
        last_update_ = now;
        started_ = true;
        max_scroll = std::max(0.0f, max_scroll);

        for (const PointerEvent& pointer : pointers) {
            if (pointer.pressed) {
                active_ = list.contains(pointer.x, pointer.y);
                // Catching a list that is still gliding stops it, and is not
                // a tap on a row.
                moved_ = active_ && std::fabs(velocity_) > kStopSpeed;
                velocity_ = 0.0f;
                start_y_ = pointer.y;
                last_y_ = pointer.y;
                start_scroll_ = scroll;
            }
            if (pointer.released) {
                released_drag_ = active_ && moved_;
                // A finger that stopped before it lifted leaves the list where
                // it is; one still moving sets it gliding.
                if (!released_drag_) velocity_ = 0.0f;
                active_ = false;
            }
        }

        const LivePointer& live = live_pointer();
        if (active_ && live.down) {
            const float travel = start_y_ - live.y;
            if (std::fabs(travel) > kThresholdPixels) moved_ = true;
            if (moved_) {
                scroll = std::clamp(start_scroll_ + travel, 0.0f, max_scroll);
                // How fast the finger is going, smoothed over the last few
                // passes so one uneven frame does not decide the glide.
                if (dt > 0.0f) {
                    const float instant = (last_y_ - live.y) / dt;
                    velocity_ = velocity_ * 0.6f + instant * 0.4f;
                }
            }
            last_y_ = live.y;
        } else if (!active_ && velocity_ != 0.0f && dt > 0.0f) {
            scroll += velocity_ * dt;
            velocity_ *= std::exp(-kFriction * dt);
            if (scroll <= 0.0f || scroll >= max_scroll || std::fabs(velocity_) < kStopSpeed) {
                velocity_ = 0.0f;
            }
            scroll = std::clamp(scroll, 0.0f, max_scroll);
        }
    }

    // Whether this pass's release ended a drag (or stopped a glide), and so is
    // not a tap on a row.
    bool release_was_drag() const { return released_drag_; }
    // A finger moving the list, or the list still gliding after one.
    bool dragging() const { return (active_ && moved_) || velocity_ != 0.0f; }

    // Clears the last release, once it has been looked at.
    void settle() { released_drag_ = false; }
    // Stops a glide: the cursor has moved the list somewhere else.
    void stop() { velocity_ = 0.0f; }

private:
    static constexpr float kThresholdPixels = 14.0f;
    // How quickly a glide slows (per second, exponentially) and the speed, in
    // pixels a second, below which it is over.
    static constexpr float kFriction = 3.5f;
    static constexpr float kStopSpeed = 30.0f;

    bool started_ = false;
    std::chrono::steady_clock::time_point last_update_{};
    bool active_ = false;
    bool moved_ = false;
    bool released_drag_ = false;
    float start_y_ = 0.0f;
    float last_y_ = 0.0f;
    float start_scroll_ = 0.0f;
    float velocity_ = 0.0f;  // pixels a second, positive scrolling down the list
};

}  // namespace ds
