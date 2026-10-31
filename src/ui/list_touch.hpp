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
    // Gone further from where it went down than a tap's wobble: scrolling
    // something, and so no longer holding down what it went down on.
    bool moved = false;
    // Went down while a popup was up, so on the popup or the scrim round it:
    // nothing behind it is held down.
    bool on_popup = false;
};
void set_live_pointer(bool down, float x, float y);
const LivePointer& live_pointer();
// How far a finger goes before it is moving rather than tapping, in pixels.
constexpr float kTapWobblePixels = 14.0f;

// Whether a finger (or the mouse button) is being held down on `rect`, as a
// Switch shows it: down inside it, still inside it, and not moved off to
// scroll. `behind_popups` for something drawn under a popup, which a finger
// that went down while one was up is not holding.
//
// A finger held on the other device's drawing of the same page counts too:
// see set_remote_held().
struct RemoteHeld {
    bool held = false;
    float x = 0.0f;
    float y = 0.0f;
};
const RemoteHeld& remote_held();
// Whether a popup -- an alert, a dropdown -- was up as the last frame was
// drawn: everything behind one is drawn without the cursor, the popup having
// it. A frame late, as a popup is drawn after what is behind it; see
// begin_menu_frame().
bool popup_up();
inline bool pointer_holding(const PixelRect& rect, bool behind_popups = false) {
    const LivePointer& live = live_pointer();
    if (live.down && !live.moved && !(behind_popups && live.on_popup) &&
        rect.contains(live.press_x, live.press_y) && rect.contains(live.x, live.y)) {
        return true;
    }
    const RemoteHeld& remote = remote_held();
    return remote.held && !(behind_popups && popup_up()) && rect.contains(remote.x, remote.y);
}
// Where on this device's drawing of the page a finger on the other device's
// is held -- a point on what it is held on -- or not held at all. See
// MenuCursorSync.
void set_remote_held(bool held, float x, float y);
// Said by a popup each time it is drawn, so a finger going down while it is
// up is known to be on it -- see LivePointer::on_popup.
void note_popup_drawn();

// Whether the cursor is drawn -- the ring round a row, a popup's highlighted
// button or choice. As on a Switch, a finger going down anywhere takes it
// away, and with it every menu's highlight: from then on nothing is
// highlighted but what a finger is held on, and a popup opens with none of
// its answers highlighted. A press of the pad brings it back, where it was.
bool menu_cursor_shown();
void set_menu_cursor_shown(bool shown);
// Once a frame, before it is drawn.
void begin_menu_frame();
// Said by whatever draws the cursor as a rounded ring -- a row's, a popup's
// button's, a dropdown's choice's, a tile's -- each time it does. The quick
// menu's sections, filled rather than ringed, do not say it.
void note_cursor_ring_drawn();
// Whether the frames just drawn had the cursor as a rounded ring: then a
// direction that moves nothing bumps it -- see UiSoundJudge.
bool cursor_ring_up();
// The cursor bumping against the end of where it can go, as a Switch's
// does: the ring jiggles a little towards `direction` -- Up, Down, Left or
// Right -- and settles back, over a fifth of a second.
void start_menu_bump(UiAction direction);
// Where the jiggle has the ring right now, in pixels from where it belongs,
// for a page drawn with text at `line` pixels a line: 0, 0 but while one
// is under way.
void menu_bump_offset(float line, float& dx, float& dy);
// Whether something on a page is drawn as under the cursor: `focused`,
// while the cursor is drawn at all and no popup is up over the page.
inline bool cursor_drawn(bool focused) { return focused && menu_cursor_shown() && !popup_up(); }
// Whether a popup's own button or choice is: `focused`, while the cursor is
// drawn at all.
inline bool popup_cursor_drawn(bool focused) { return focused && menu_cursor_shown(); }
// This pass's input as far as the cursor goes, before any menu hears it,
// and what the menus are to hear. A finger or the mouse going down hides
// the cursor. A direction, A or a shoulder while it is hidden brings it
// back where it was and is not otherwise heard -- a press only moves or
// chooses what can be seen -- while `in_menu`: playing, every press is the
// game's, and shows it only for when a menu is next up. B, and the menu's
// own button, show it too, and are heard.
std::vector<UiAction> menu_cursor_input(const std::vector<UiAction>& actions,
                                        const std::vector<PointerEvent>& pointers, bool in_menu);

// The mouse wheel this pass: how many notches it turned, positive up a list,
// and where the mouse was -- a list under it scrolls. Set once a pass beside
// the live pointer, zero when it did not turn.
struct LiveWheel {
    float notches = 0.0f;
    float x = 0.0f;
    float y = 0.0f;
};
void set_live_wheel(float notches, float x, float y);
const LiveWheel& live_wheel();

// How far one notch of the wheel moves a list, in pixels, at a list's row
// height: three rows, as a desktop's lists move.
inline float wheel_pixels(float notches, float row_height) { return -notches * row_height * 3.0f; }

// A tap on a row of a list chooses it, as a Switch's do: the finger held
// on it showed which it was, and one that slid off to scroll took it back.
// The cursor goes there too, for when a press of the pad brings it back.
// True when `index` is to be chosen, which is always.
inline bool tap_chooses(int& focus, int index) {
    focus = index;
    return true;
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
        const auto now = ListDrag::now();
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
        } else if (!active_ && wheel_turned(list)) {
            velocity_ = 0.0f;
            const float row = wheel_row_ > 0.0f ? wheel_row_ : list.h / 8.0f;
            scroll = std::clamp(scroll + wheel_pixels(live_wheel().notches, row), 0.0f, max_scroll);
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
    // How tall the list's rows are, so a notch of the wheel moves it a few.
    // Unset, a list's rows are taken to be an eighth of its height.
    void set_wheel_row(float row_height) { wheel_row_ = std::max(1.0f, row_height); }
    // What every ListDrag reads the time from, in place of the steady clock:
    // for a test, whose passes are frames apart however busy the machine is.
    using Clock = std::chrono::steady_clock::time_point (*)();
    static void set_clock(Clock clock) { clock_ = clock; }
    static std::chrono::steady_clock::time_point now() {
        return clock_ != nullptr ? clock_() : std::chrono::steady_clock::now();
    }
    // Whether the wheel turned over `list` this pass.
    static bool wheel_turned(const PixelRect& list) {
        const LiveWheel& wheel = live_wheel();
        return wheel.notches != 0.0f && list.contains(wheel.x, wheel.y);
    }

private:
    static constexpr float kThresholdPixels = kTapWobblePixels;
    // How quickly a glide slows (per second, exponentially) and the speed, in
    // pixels a second, below which it is over.
    static constexpr float kFriction = 3.5f;
    static constexpr float kStopSpeed = 30.0f;

    bool started_ = false;
    std::chrono::steady_clock::time_point last_update_{};
    static inline Clock clock_ = nullptr;
    bool active_ = false;
    bool moved_ = false;
    bool released_drag_ = false;
    float start_y_ = 0.0f;
    float last_y_ = 0.0f;
    float start_scroll_ = 0.0f;
    float velocity_ = 0.0f;  // pixels a second, positive scrolling down the list
    float wheel_row_ = 0.0f;
};

// Where a list is drawn, easing after where it is: the cursor moving off the
// part in view slides the list along to it, as a Switch's lists go, rather
// than jumping it there. A finger moving the list, or one setting it
// gliding, is followed exactly. Only the drawing lags: the list is where its
// scroll says for everything else.
class ScrollEase {
public:
    // Where to draw a list scrolled to `scroll` this pass; `follow` while a
    // finger is moving it. `content` says which list it is, on a page that
    // shows one list after another in the same place: a different one is
    // drawn where it is at once, as is a list not drawn a moment ago.
    // `settled` is how near counts as there, in the scroll's own measure.
    float shown(float scroll, bool follow, int content = 0, float settled = 0.5f) {
        const auto now = ListDrag::now();
        float dt = std::chrono::duration<float>(now - last_).count();
        last_ = now;
        const bool other = content != content_;
        content_ = content;
        if (!started_ || follow || other || dt < 0.0f || dt > 0.25f) {
            shown_ = scroll;
            started_ = true;
            return shown_;
        }
        shown_ += (scroll - shown_) * (1.0f - std::exp(-kRate * std::min(dt, 0.1f)));
        if (std::fabs(scroll - shown_) < settled) shown_ = scroll;
        return shown_;
    }
    // Whether the list is still sliding to where it is.
    bool moving(float scroll) const { return started_ && shown_ != scroll; }

private:
    // How quickly it closes the distance, per second, exponentially: most of
    // the way there in about a tenth of a second.
    static constexpr float kRate = 22.0f;
    bool started_ = false;
    int content_ = 0;
    float shown_ = 0.0f;
    std::chrono::steady_clock::time_point last_{};
};

// A page's scroll swapped for where it is drawn, while it is drawn.
class ShownScroll {
public:
    ShownScroll(float& scroll, ScrollEase& ease, bool follow, int content = 0, float settled = 0.5f)
        : scroll_(scroll), kept_(scroll) {
        scroll = ease.shown(scroll, follow, content, settled);
    }
    ~ShownScroll() { scroll_ = kept_; }
    ShownScroll(const ShownScroll&) = delete;
    ShownScroll& operator=(const ShownScroll&) = delete;

private:
    float& scroll_;
    float kept_;
};

}  // namespace ds
