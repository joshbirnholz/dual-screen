#pragma once

#include <string>

namespace ds {

// Whether this device has a pointer at all.
//
// On means the ordinary one: it appears when the mouse moves and goes away once
// it has been still for a moment. There is no third "never hides" mode, because
// SteamOS hides an idle pointer by itself -- asking for one that stays put is
// asking for something the system will not give.
enum class CursorMode {
    Shown,
    Hidden,
};

// The Hide Cursor switch: "On" for Hidden.
const char* to_string(CursorMode mode);
CursorMode next_cursor_mode(CursorMode mode);
const char* cursor_mode_key(CursorMode mode);
CursorMode cursor_mode_from_key(const std::string& key);

// Whether this device has a mouse pointer for the Pointer setting to show or
// hide: a computer does, and a Steam Deck's trackpads drive one. A Switch, an
// Apple TV, a phone or a tablet has a finger or a remote instead, and nothing
// on screen that the setting could do anything to.
bool device_has_pointer();

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
    // Whether a menu is up. The setting is for a game: in a menu the pointer
    // is how a mouse chooses -- a Touch Point among other things -- so it
    // shows whatever the setting says.
    void set_in_menu(bool in_menu) { in_menu_ = in_menu; }

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

    bool visible() const { return (in_menu_ || mode_ == CursorMode::Shown) && !idle_hidden_; }

private:
    double idle_seconds_;
    double still_for_ = 0.0;
    CursorMode mode_ = CursorMode::Shown;
    bool in_menu_ = false;
    // Hidden to begin with: nothing has been touched yet, and a pointer sitting
    // over the game at startup is the thing being avoided.
    bool idle_hidden_ = true;
};

}  // namespace ds
