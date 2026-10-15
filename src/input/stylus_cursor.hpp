#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace ds {

// A stylus on the right stick: a pointer over the touch screen that the stick
// moves and R3 presses down -- see RightStickMode::TouchCursor. Kept in the
// touch screen's own pixels, so it is where it was whichever layout or device
// the screen is drawn on.
//
// The stick sets how fast it goes rather than where it is: a little push
// creeps, for picking out one small button, and a full one crosses the screen
// in under a second. It shows while it is being moved or pressed, and fades a
// little while after.
class StylusCursor {
public:
    // A full push crosses the screen's width in this long.
    static constexpr float kCrossingSeconds = 0.65f;
    // How long it stays after it was last moved or pressed, and how long it
    // takes to fade at the end of that.
    static constexpr float kShownSeconds = 2.5f;
    static constexpr float kFadeSeconds = 0.3f;

    // One frame of the stick and the button, `seconds` after the last.
    // `deadzone` is the fraction of a push that counts as none.
    void update(int16_t stick_x, int16_t stick_y, bool pressed, float deadzone, float seconds, int width,
                int height) {
        if (width <= 0 || height <= 0) return;
        if (width != width_ || height != height_) {
            // A new screen: the cursor starts in its middle.
            width_ = width;
            height_ = height;
            x_ = width * 0.5f;
            y_ = height * 0.5f;
        }
        seconds = std::clamp(seconds, 0.0f, 0.1f);
        float push_x = stick_x / 32767.0f;
        float push_y = stick_y / 32767.0f;
        const float length = std::sqrt(push_x * push_x + push_y * push_y);
        bool moved = false;
        if (length > deadzone && length > 0.0f) {
            // Past the deadzone, from nothing to full, and squared: fine
            // control near the middle, speed at the edge.
            const float past = std::min(1.0f, (length - deadzone) / std::max(0.01f, 1.0f - deadzone));
            const float speed = past * past * static_cast<float>(width) / kCrossingSeconds;
            push_x /= length;
            push_y /= length;
            x_ = std::clamp(x_ + push_x * speed * seconds, 0.0f, static_cast<float>(width - 1));
            y_ = std::clamp(y_ + push_y * speed * seconds, 0.0f, static_cast<float>(height - 1));
            moved = true;
        }
        pressed_ = pressed;
        if (moved || pressed) {
            idle_seconds_ = 0.0f;
        } else {
            idle_seconds_ = std::min(idle_seconds_ + seconds, kShownSeconds);
        }
    }

    // Put away: the next time it is moved it starts where it was, but it is
    // not drawn or pressed until then.
    void hide() {
        pressed_ = false;
        idle_seconds_ = kShownSeconds;
    }

    // Whether it is touching the screen, and where, in the screen's pixels.
    bool pressed() const { return pressed_; }
    int x() const { return static_cast<int>(x_); }
    int y() const { return static_cast<int>(y_); }
    // Where it is as a fraction of the screen, 0 to 1, for drawing.
    float u() const { return width_ > 0 ? (x_ + 0.5f) / static_cast<float>(width_) : 0.5f; }
    float v() const { return height_ > 0 ? (y_ + 0.5f) / static_cast<float>(height_) : 0.5f; }
    // How much of it is showing, 0 to 1.
    float opacity() const {
        if (width_ <= 0) return 0.0f;
        const float left = kShownSeconds - idle_seconds_;
        return std::clamp(left / kFadeSeconds, 0.0f, 1.0f);
    }

private:
    int width_ = 0;
    int height_ = 0;
    float x_ = 0.0f;
    float y_ = 0.0f;
    bool pressed_ = false;
    float idle_seconds_ = kShownSeconds;
};

}  // namespace ds
