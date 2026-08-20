#pragma once

namespace ds {

// Shows the mouse pointer while it is being moved and hides it again once it
// has been still for a moment.
//
// The pointer is not part of the console fiction, but it is how a trackpad
// reaches the touchscreen, so it cannot simply be gone: an arrow you cannot
// see is one you cannot aim. It appears the instant the mouse moves and
// disappears a couple of seconds after it stops.
//
// A finger on a touchscreen does not count as mouse movement. SDL reports
// touches as synthetic mouse events, and a tap that made an arrow appear in
// the corner of the screen would be exactly the wrong behaviour.
//
// Kept separate from the SDL polling so it can be tested without a mouse.
class CursorVisibility {
public:
    explicit CursorVisibility(double idle_seconds = 2.0) : idle_seconds_(idle_seconds) {}

    // The real mouse moved or was clicked.
    void notice_mouse_activity() {
        still_for_ = 0.0;
        visible_ = true;
    }

    // Advances the idle timer by one frame's worth of time.
    void update(double delta_seconds) {
        if (!visible_) {
            return;
        }
        if (delta_seconds > 0.0) {
            still_for_ += delta_seconds;
        }
        if (still_for_ >= idle_seconds_) {
            visible_ = false;
        }
    }

    bool visible() const { return visible_; }

private:
    double idle_seconds_;
    double still_for_ = 0.0;
    // Hidden to begin with: nothing has been touched yet, and a pointer sitting
    // over the game at startup is the thing being avoided.
    bool visible_ = false;
};

}  // namespace ds
