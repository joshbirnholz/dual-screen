#pragma once

#include <string>

namespace ds {

// What the pointer does. Automatic is what you want when SDL can tell a mouse
// from a finger; the other two exist because some setups present a touchscreen
// as an ordinary mouse, and then no amount of cleverness can tell them apart.
enum class CursorMode {
    AutoHide,      // appears when the mouse moves, hides once it is still
    AlwaysShown,
    AlwaysHidden,
};

const char* to_string(CursorMode mode);
CursorMode next_cursor_mode(CursorMode mode);
const char* cursor_mode_key(CursorMode mode);
CursorMode cursor_mode_from_key(const std::string& key);

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

    void set_mode(CursorMode mode) { mode_ = mode; }
    CursorMode mode() const { return mode_; }

    // The real mouse moved or was clicked.
    void notice_mouse_activity() {
        still_for_ = 0.0;
        idle_hidden_ = false;
    }

    // Advances the idle timer by one frame's worth of time.
    void update(double delta_seconds) {
        if (idle_hidden_) {
            return;
        }
        if (delta_seconds > 0.0) {
            still_for_ += delta_seconds;
        }
        if (still_for_ >= idle_seconds_) {
            idle_hidden_ = true;
        }
    }

    bool visible() const {
        switch (mode_) {
            case CursorMode::AlwaysShown: return true;
            case CursorMode::AlwaysHidden: return false;
            case CursorMode::AutoHide: break;
        }
        return !idle_hidden_;
    }

private:
    double idle_seconds_;
    double still_for_ = 0.0;
    CursorMode mode_ = CursorMode::AutoHide;
    // Hidden to begin with: nothing has been touched yet, and a pointer sitting
    // over the game at startup is the thing being avoided.
    bool idle_hidden_ = true;
};

}  // namespace ds
