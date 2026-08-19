#pragma once

#include <cstdint>

namespace ds {

// Suppresses inputs that were already held at the moment the game started
// listening again, until they are physically released.
//
// Without this, dismissing a menu leaks into the game: the button state is
// polled, not edge-triggered, so the A press that chose "Resume" is still down
// on the next frame and the game sees it as a fresh press. The same applies to
// a finger still on the screen.
//
// Kept separate from the SDL polling so the behaviour can be tested directly.
class HeldInputGate {
public:
    // The game has started listening. Whatever is held on the next update() is
    // held over from the menu and gets suppressed.
    void arm() { arming_ = true; }

    // The game has stopped listening; nothing is carried over.
    void reset() {
        arming_ = false;
        masked_buttons_ = 0;
        masked_touch_ = false;
    }

    // Feeds one frame of physically-held state. Call this every frame, even
    // while the game is not listening, so that arming captures the real state.
    void update(uint32_t held_buttons, bool touch_down) {
        if (arming_) {
            masked_buttons_ = held_buttons;
            masked_touch_ = touch_down;
            arming_ = false;
        }
        // A mask bit survives only as long as the input stays held.
        masked_buttons_ &= held_buttons;
        if (!touch_down) {
            masked_touch_ = false;
        }
        buttons_ = held_buttons & ~masked_buttons_;
        touching_ = touch_down && !masked_touch_;
    }

    // What the game should actually see this frame.
    uint32_t buttons() const { return buttons_; }
    bool touching() const { return touching_; }

    bool is_masking() const { return masked_buttons_ != 0 || masked_touch_; }

private:
    bool arming_ = false;
    uint32_t masked_buttons_ = 0;
    bool masked_touch_ = false;
    uint32_t buttons_ = 0;
    bool touching_ = false;
};

}  // namespace ds
