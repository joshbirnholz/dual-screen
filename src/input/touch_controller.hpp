#pragma once

#include <SDL.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

#include "core/system.hpp"
#include "input/raw_pad_state.hpp"
#include "layout/layout.hpp"

namespace ds {

// A controller drawn over the game, for a phone or tablet with no pad
// connected: a d-pad, the face buttons, the shoulder buttons, Start and Select,
// and on a console that has them the sticks -- see Settings::touch_controls.
//
// It is a pad like any other as far as the rest of the app goes. Its presses
// come out as a RawPadState, in SDL's terms, and go through the same Button
// Mapping a real pad's do; on a Wii U it is one more controller the
// Controllers screen can make the GamePad or a player. The only thing it does
// that a pad does not is take fingers: one that lands on a control is that
// control's until it lifts, and never reaches the game's touch screen or
// the menus. Every other finger is a touch as before.
//
// Positions are worked out here, from the size of the window, rather than
// handed in, so the same arrangement serves the companion and a game played
// on the device itself. Everything is in output pixels.
class TouchController {
public:
    // Which of the optional controls this console has.
    struct Set {
        // A 3DS's circle pad, or a Wii U GamePad's left stick.
        bool left_stick = false;
        // A GamePad's right stick.
        bool right_stick = false;
        // ZL and ZR, a GamePad's.
        bool zl_zr = false;
        // + and - rather than Start and Select, as a GamePad prints them.
        bool plus_minus = false;

        bool operator==(const Set& other) const {
            return left_stick == other.left_stick && right_stick == other.right_stick &&
                   zl_zr == other.zl_zr && plus_minus == other.plus_minus;
        }
        bool operator!=(const Set& other) const { return !(*this == other); }
    };

    enum class Kind {
        // One button, round or a capsule.
        Button,
        // Four directions and the diagonals between them.
        DPad,
        // An analog stick.
        Stick,
        // The quick menu, which a device with no buttons otherwise only
        // reaches with two fingers.
        Menu,
    };

    struct Control {
        Kind kind = Kind::Button;
        // A Button's own button, in SDL's terms -- by position, as a real pad
        // reports them: SDL's B is the east button, a Nintendo pad's A.
        SDL_GameControllerButton button = SDL_CONTROLLER_BUTTON_INVALID;
        // Or a trigger, held all the way: ZL and ZR.
        SDL_GameControllerAxis trigger = SDL_CONTROLLER_AXIS_INVALID;
        // A Stick's side.
        bool right = false;
        // What is printed on it.
        const char* label = "";
        // Where it is. A round control is the circle in `rect`; a capsule
        // is wider than tall.
        PixelRect rect;
        // Whether it is one of the four face buttons, which a finger can
        // slide between, as a thumb rolls from B to A.
        bool face = false;

        float centre_x() const { return rect.x + rect.w * 0.5f; }
        float centre_y() const { return rect.y + rect.h * 0.5f; }
        float radius() const { return (rect.w < rect.h ? rect.w : rect.h) * 0.5f; }
    };

    // Shown only on a device held upright, with the game in the top half of
    // the window and these in the bottom half -- see game_height().
    //
    // Lays the controls out for a window `width` by `height`. Fingers already
    // down keep the controls they have, which a turned phone cannot move out
    // from under them in any way that matters.
    void arrange(float width, float height, const Set& set);
    const Set& set() const { return set_; }

    // Shown, it takes fingers and draws; hidden, it lets go of every one.
    void set_shown(bool shown);
    bool shown() const { return shown_; }

    // A finger, in output pixels. Down returns whether a control took it --
    // and so whether nothing else should see it. Motion and up return
    // whether it was one of these controls' fingers.
    bool finger_down(SDL_FingerID finger, float x, float y);
    bool finger_motion(SDL_FingerID finger, float x, float y);
    bool finger_up(SDL_FingerID finger);
    // Every finger let go of: the window lost focus, say.
    void release_all();
    // Whether any finger is on a control.
    bool held() const { return !fingers_.empty(); }

    // What is held, as a pad would report it.
    RawPadState raw() const;
    // Whether the menu control was tapped since the last call.
    bool take_menu_press();

    const std::vector<Control>& controls() const { return controls_; }
    // For drawing: whether a control is being pressed, and for a stick how
    // far it is pushed, -1 to 1 each way.
    bool pressed(std::size_t index) const;
    void stick_push(std::size_t index, float& x, float& y) const;
    // Where a stick is centred for drawing: under the finger that took it,
    // where that finger landed, or its own place while nobody holds it.
    void stick_centre(std::size_t index, float& x, float& y) const;
    // And for a d-pad, which directions: dpad_directions()'s bits.
    unsigned dpad_held(std::size_t index) const;

    // Which way a d-pad is pushed by a finger at (dx, dy) from its centre,
    // for a pad of `radius`: four bits, up, down, left, right. Nothing in
    // the middle, and a diagonal only across the 45 degrees around one.
    static unsigned dpad_directions(float dx, float dy, float radius);

    // How much of a window `height` tall, from the top, the game is drawn in
    // while these are shown: the top half. The controls keep to the rest.
    static float game_height(float height);

private:
    struct Finger {
        std::size_t control = 0;
        float x = 0.0f;
        float y = 0.0f;
        // Where it landed: a stick's centre, wherever that was.
        float anchor_x = 0.0f;
        float anchor_y = 0.0f;
    };
    // The control under (x, y), if any, as an index into controls_.
    bool control_at(float x, float y, std::size_t& index) const;
    // The stick whose side of the controls (x, y) is on, if that side has
    // one: a finger landing anywhere there that is not on a button is the
    // stick, centred where it lands.
    bool stick_side_at(float x, float y, std::size_t& index) const;
    // Places every control for a thumb span of `s` pixels.
    void lay_out(float s);
    // The face button nearest (x, y) while a finger slides over them --
    // within reach of the cluster, or none.
    bool face_at(float x, float y, std::size_t& index) const;

    std::vector<Control> controls_;
    std::map<SDL_FingerID, Finger> fingers_;
    Set set_;
    float width_ = 0.0f;
    float height_ = 0.0f;
    bool shown_ = false;
    bool menu_pressed_ = false;
};

// The controls `system`'s own controller has: a DS's d-pad and buttons, a
// 3DS's circle pad as well, and a Wii U GamePad's two sticks, ZL and ZR, and
// + and -.
TouchController::Set touch_controls_for(SystemId system);

// DUALSCREEN_TOUCH_CONTROLS=1 shows the touch controller on any device with
// no pad, Settings or not -- for trying it on a desktop, and in the tests.
bool touch_controls_forced();

}  // namespace ds
