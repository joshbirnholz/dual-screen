#include "input/touch_controller.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace ds {

namespace {

constexpr float kPi = 3.14159265358979f;

// A finger slightly off a control still means it: a thumb cannot see what it
// covers.
constexpr float kReach = 1.2f;

PixelRect circle(float centre_x, float centre_y, float radius) {
    return PixelRect{centre_x - radius, centre_y - radius, radius * 2.0f, radius * 2.0f};
}

TouchController::Control button(SDL_GameControllerButton which, const char* label, const PixelRect& rect,
                                bool face = false) {
    TouchController::Control control;
    control.kind = TouchController::Kind::Button;
    control.button = which;
    control.label = label;
    control.rect = rect;
    control.face = face;
    return control;
}

TouchController::Control trigger(SDL_GameControllerAxis which, const char* label, const PixelRect& rect) {
    TouchController::Control control;
    control.kind = TouchController::Kind::Button;
    control.trigger = which;
    control.label = label;
    control.rect = rect;
    return control;
}

bool round_control(const TouchController::Control& control) {
    return std::fabs(control.rect.w - control.rect.h) < 0.5f;
}

}  // namespace

void TouchController::arrange(float width, float height, const Set& set) {
    if (width == width_ && height == height_ && set == set_ && !controls_.empty()) return;
    width_ = width;
    height_ = height;
    set_ = set;
    controls_.clear();
    if (width <= 0.0f || height <= 0.0f) return;

    const bool landscape = width >= height;
    // Everything is sized from the short side, which is what a thumb spans.
    const float s = landscape ? height : width;
    // A wide phone's corners are rounded and one side may have a notch, so
    // the controls keep further in from its ends.
    const float mx = s * (landscape && width / height > 1.9f ? 0.11f : 0.05f);
    const float my = s * (landscape ? 0.05f : 0.08f);

    const float face_radius = s * 0.068f;
    const float face_spread = s * 0.115f;
    const float shoulder_w = s * 0.30f;
    const float shoulder_h = s * 0.10f;
    const float small = s * 0.045f;

    // Where each cluster goes: the d-pad and face buttons, the sticks, and
    // the row of shoulder buttons above them.
    float dpad_x = 0.0f, dpad_y = 0.0f, dpad_r = 0.0f;
    float face_x = 0.0f, face_y = 0.0f;
    float left_stick_x = 0.0f, left_stick_y = 0.0f;
    float right_stick_x = 0.0f, right_stick_y = 0.0f;
    const float stick_r = s * 0.135f;
    float shoulder_y = 0.0f;
    float select_x = 0.0f, select_y = 0.0f, start_x = 0.0f, start_y = 0.0f;
    float menu_x = 0.0f, menu_y = 0.0f;

    if (landscape) {
        shoulder_y = my;
        const float upper = height * (set.zl_zr ? 0.46f : 0.42f);
        const float lower = height - my - s * 0.14f;
        const float middle = height * 0.64f;
        if (set.left_stick) {
            left_stick_x = mx + s * 0.16f;
            left_stick_y = upper;
            dpad_r = s * 0.13f;
            dpad_x = mx + s * 0.32f;
            dpad_y = lower;
        } else {
            dpad_r = s * 0.17f;
            dpad_x = mx + s * 0.19f;
            dpad_y = middle;
        }
        face_x = width - mx - s * 0.19f;
        if (set.right_stick) {
            face_y = upper;
            right_stick_x = width - mx - s * 0.32f;
            right_stick_y = lower;
        } else {
            face_y = set.left_stick ? height * 0.50f : middle;
        }
        // Inside the shoulder buttons, as a Switch has - and + inside its
        // Joy-Con's top edges.
        select_x = mx + shoulder_w + s * 0.03f + (set.plus_minus ? small : small * 2.2f);
        select_y = my + shoulder_h * 0.5f;
        start_x = width - select_x;
        start_y = select_y;
        menu_x = width - mx - small;
        menu_y = height - my - small;
    } else {
        // Select, Start and the menu along the foot, and everything else
        // above them: there is no room between the two clusters on a phone
        // held upright.
        const float bottom = height - my;
        select_x = width * 0.5f - s * 0.12f;
        start_x = width * 0.5f + s * 0.12f;
        select_y = bottom - small;
        start_y = select_y;
        menu_x = width - mx - small;
        menu_y = select_y;
        const float base = bottom - s * 0.12f;
        float cluster_top = 0.0f;
        if (set.left_stick || set.right_stick) {
            dpad_r = s * 0.12f;
            dpad_x = mx + s * 0.16f;
            dpad_y = base - s * 0.15f;
            left_stick_x = dpad_x;
            left_stick_y = base - s * 0.48f;
            face_x = width - mx - s * 0.19f;
            face_y = set.right_stick ? base - s * 0.48f : base - s * 0.30f;
            right_stick_x = width - mx - s * 0.16f;
            right_stick_y = base - s * 0.15f;
            cluster_top = std::min(left_stick_y - stick_r, face_y - face_spread - face_radius);
        } else {
            dpad_r = s * 0.17f;
            dpad_x = mx + s * 0.19f;
            dpad_y = base - s * 0.20f;
            face_x = width - mx - s * 0.19f;
            face_y = dpad_y;
            cluster_top = std::min(dpad_y - dpad_r, face_y - face_spread - face_radius);
        }
        shoulder_y = cluster_top - s * 0.03f - shoulder_h * (set.zl_zr ? 2.15f : 1.0f);
    }

    {
        Control dpad;
        dpad.kind = Kind::DPad;
        dpad.rect = circle(dpad_x, dpad_y, dpad_r);
        controls_.push_back(dpad);
    }
    // By position, Nintendo's letters: A to the east, B south, X north, Y
    // west -- and SDL names the positions by an Xbox pad's letters.
    controls_.push_back(button(SDL_CONTROLLER_BUTTON_B, "A",
                               circle(face_x + face_spread, face_y, face_radius), true));
    controls_.push_back(button(SDL_CONTROLLER_BUTTON_A, "B",
                               circle(face_x, face_y + face_spread, face_radius), true));
    controls_.push_back(button(SDL_CONTROLLER_BUTTON_Y, "X",
                               circle(face_x, face_y - face_spread, face_radius), true));
    controls_.push_back(button(SDL_CONTROLLER_BUTTON_X, "Y",
                               circle(face_x - face_spread, face_y, face_radius), true));

    controls_.push_back(button(SDL_CONTROLLER_BUTTON_LEFTSHOULDER, "L",
                               PixelRect{mx, shoulder_y, shoulder_w, shoulder_h}));
    controls_.push_back(button(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, "R",
                               PixelRect{width - mx - shoulder_w, shoulder_y, shoulder_w, shoulder_h}));
    if (set.zl_zr) {
        const float below = shoulder_y + shoulder_h * 1.15f;
        controls_.push_back(trigger(SDL_CONTROLLER_AXIS_TRIGGERLEFT, "ZL",
                                    PixelRect{mx, below, shoulder_w, shoulder_h}));
        controls_.push_back(trigger(SDL_CONTROLLER_AXIS_TRIGGERRIGHT, "ZR",
                                    PixelRect{width - mx - shoulder_w, below, shoulder_w, shoulder_h}));
    }
    controls_.push_back(button(SDL_CONTROLLER_BUTTON_BACK, set.plus_minus ? "\xE2\x88\x92" : "Select",
                               set.plus_minus ? circle(select_x, select_y, small)
                                              : PixelRect{select_x - small * 2.2f, select_y - small * 0.8f,
                                                          small * 4.4f, small * 1.6f}));
    controls_.push_back(button(SDL_CONTROLLER_BUTTON_START, set.plus_minus ? "+" : "Start",
                               set.plus_minus ? circle(start_x, start_y, small)
                                              : PixelRect{start_x - small * 2.2f, start_y - small * 0.8f,
                                                          small * 4.4f, small * 1.6f}));
    if (set.left_stick) {
        Control stick;
        stick.kind = Kind::Stick;
        stick.rect = circle(left_stick_x, left_stick_y, stick_r);
        controls_.push_back(stick);
    }
    if (set.right_stick) {
        Control stick;
        stick.kind = Kind::Stick;
        stick.right = true;
        stick.rect = circle(right_stick_x, right_stick_y, stick_r);
        controls_.push_back(stick);
    }
    {
        Control menu;
        menu.kind = Kind::Menu;
        menu.rect = circle(menu_x, menu_y, small);
        controls_.push_back(menu);
    }

    // A finger already down keeps a control of the same place in the list;
    // one whose control is gone is let go.
    for (auto it = fingers_.begin(); it != fingers_.end();) {
        if (it->second.control >= controls_.size()) {
            it = fingers_.erase(it);
        } else {
            ++it;
        }
    }
}

void TouchController::set_shown(bool shown) {
    if (shown == shown_) return;
    shown_ = shown;
    if (!shown_) release_all();
}

bool TouchController::control_at(float x, float y, std::size_t& index) const {
    // The nearest one the finger is within reach of, so two controls whose
    // reach overlaps share the gap between them.
    float best = 0.0f;
    bool found = false;
    for (std::size_t i = 0; i < controls_.size(); ++i) {
        const Control& control = controls_[i];
        float distance = 0.0f;
        if (round_control(control)) {
            const float dx = x - control.centre_x();
            const float dy = y - control.centre_y();
            distance = std::sqrt(dx * dx + dy * dy) / control.radius();
            if (distance > kReach) continue;
        } else {
            // A capsule: within its rect grown by a little each way.
            const float grow = control.rect.h * 0.25f;
            const PixelRect reach{control.rect.x - grow, control.rect.y - grow, control.rect.w + grow * 2.0f,
                                  control.rect.h + grow * 2.0f};
            if (!reach.contains(x, y)) continue;
            const float dx = std::fabs(x - control.centre_x()) / (control.rect.w * 0.5f);
            const float dy = std::fabs(y - control.centre_y()) / (control.rect.h * 0.5f);
            distance = std::max(dx, dy);
        }
        if (!found || distance < best) {
            best = distance;
            index = i;
            found = true;
        }
    }
    return found;
}

bool TouchController::face_at(float x, float y, std::size_t& index) const {
    float best = 0.0f;
    bool found = false;
    for (std::size_t i = 0; i < controls_.size(); ++i) {
        const Control& control = controls_[i];
        if (!control.face) continue;
        const float dx = x - control.centre_x();
        const float dy = y - control.centre_y();
        const float distance = std::sqrt(dx * dx + dy * dy) / control.radius();
        if (distance > kReach * 1.25f) continue;
        if (!found || distance < best) {
            best = distance;
            index = i;
            found = true;
        }
    }
    return found;
}

bool TouchController::finger_down(SDL_FingerID finger, float x, float y) {
    if (!shown_) return false;
    std::size_t index = 0;
    if (!control_at(x, y, index)) return false;
    fingers_[finger] = Finger{index, x, y};
    return true;
}

bool TouchController::finger_motion(SDL_FingerID finger, float x, float y) {
    const auto it = fingers_.find(finger);
    if (it == fingers_.end()) return false;
    it->second.x = x;
    it->second.y = y;
    // A thumb rolling across the face buttons presses whichever it is on;
    // off them altogether, it presses none until it comes back.
    if (controls_[it->second.control].face) {
        std::size_t index = it->second.control;
        if (face_at(x, y, index)) it->second.control = index;
    }
    return true;
}

bool TouchController::finger_up(SDL_FingerID finger) {
    const auto it = fingers_.find(finger);
    if (it == fingers_.end()) return false;
    if (controls_[it->second.control].kind == Kind::Menu) menu_pressed_ = true;
    fingers_.erase(it);
    return true;
}

void TouchController::release_all() {
    fingers_.clear();
    menu_pressed_ = false;
}

bool TouchController::take_menu_press() {
    const bool pressed = menu_pressed_;
    menu_pressed_ = false;
    return pressed;
}

unsigned TouchController::dpad_directions(float dx, float dy, float radius) {
    if (radius <= 0.0f || std::sqrt(dx * dx + dy * dy) < radius * 0.2f) return 0;
    // Each direction answers for 135 degrees around its own, so the 45
    // degrees between two neighbours are both of them.
    const float angle = std::atan2(dy, dx) * 180.0f / kPi;  // 0 right, 90 down
    const auto within = [angle](float centre) {
        float difference = std::fabs(angle - centre);
        if (difference > 180.0f) difference = 360.0f - difference;
        return difference < 67.5f;
    };
    unsigned bits = 0;
    if (within(-90.0f)) bits |= 1u;  // up
    if (within(90.0f)) bits |= 2u;   // down
    if (within(180.0f)) bits |= 4u;  // left
    if (within(0.0f)) bits |= 8u;    // right
    return bits;
}

RawPadState TouchController::raw() const {
    RawPadState raw;
    for (const auto& [id, finger] : fingers_) {
        (void)id;
        const Control& control = controls_[finger.control];
        const float dx = finger.x - control.centre_x();
        const float dy = finger.y - control.centre_y();
        switch (control.kind) {
            case Kind::Button:
                if (control.face) {
                    // Off the cluster, nothing: see finger_motion().
                    std::size_t index = 0;
                    if (!face_at(finger.x, finger.y, index)) break;
                }
                if (control.button != SDL_CONTROLLER_BUTTON_INVALID) raw.buttons[control.button] = true;
                if (control.trigger != SDL_CONTROLLER_AXIS_INVALID) raw.axes[control.trigger] = 32767;
                break;
            case Kind::DPad: {
                const unsigned bits = dpad_directions(dx, dy, control.radius());
                if (bits & 1u) raw.buttons[SDL_CONTROLLER_BUTTON_DPAD_UP] = true;
                if (bits & 2u) raw.buttons[SDL_CONTROLLER_BUTTON_DPAD_DOWN] = true;
                if (bits & 4u) raw.buttons[SDL_CONTROLLER_BUTTON_DPAD_LEFT] = true;
                if (bits & 8u) raw.buttons[SDL_CONTROLLER_BUTTON_DPAD_RIGHT] = true;
                break;
            }
            case Kind::Stick: {
                float x = 0.0f;
                float y = 0.0f;
                stick_push(finger.control, x, y);
                const auto axis = [](float value) {
                    return static_cast<int16_t>(std::clamp(value, -1.0f, 1.0f) * 32767.0f);
                };
                const SDL_GameControllerAxis ax = control.right ? SDL_CONTROLLER_AXIS_RIGHTX : SDL_CONTROLLER_AXIS_LEFTX;
                const SDL_GameControllerAxis ay = control.right ? SDL_CONTROLLER_AXIS_RIGHTY : SDL_CONTROLLER_AXIS_LEFTY;
                // The strongest push wins, as between two pads.
                if (std::abs(axis(x)) > std::abs(raw.axes[ax])) raw.axes[ax] = axis(x);
                if (std::abs(axis(y)) > std::abs(raw.axes[ay])) raw.axes[ay] = axis(y);
                break;
            }
            case Kind::Menu: break;
        }
    }
    return raw;
}

bool TouchController::pressed(std::size_t index) const {
    if (index >= controls_.size()) return false;
    for (const auto& [id, finger] : fingers_) {
        (void)id;
        if (finger.control != index) continue;
        if (controls_[index].face) {
            std::size_t under = 0;
            if (!face_at(finger.x, finger.y, under)) continue;
        }
        return true;
    }
    return false;
}

unsigned TouchController::dpad_held(std::size_t index) const {
    if (index >= controls_.size() || controls_[index].kind != Kind::DPad) return 0;
    const Control& control = controls_[index];
    unsigned bits = 0;
    for (const auto& [id, finger] : fingers_) {
        (void)id;
        if (finger.control != index) continue;
        bits |= dpad_directions(finger.x - control.centre_x(), finger.y - control.centre_y(), control.radius());
    }
    return bits;
}

void TouchController::stick_push(std::size_t index, float& x, float& y) const {
    x = 0.0f;
    y = 0.0f;
    if (index >= controls_.size() || controls_[index].kind != Kind::Stick) return;
    const Control& control = controls_[index];
    for (const auto& [id, finger] : fingers_) {
        (void)id;
        if (finger.control != index) continue;
        // All the way over at three quarters of the way to the rim, so a
        // thumb does not have to find the edge to run.
        const float full = control.radius() * 0.75f;
        x = (finger.x - control.centre_x()) / full;
        y = (finger.y - control.centre_y()) / full;
        const float length = std::sqrt(x * x + y * y);
        if (length > 1.0f) {
            x /= length;
            y /= length;
        }
        return;
    }
}

TouchController::Set touch_controls_for(SystemId system) {
    TouchController::Set set;
    switch (system) {
        case SystemId::Ds: break;
        case SystemId::ThreeDs: set.left_stick = true; break;
        case SystemId::WiiU:
            set.left_stick = true;
            set.right_stick = true;
            set.zl_zr = true;
            set.plus_minus = true;
            break;
    }
    return set;
}

bool touch_controls_forced() {
    static const bool forced = [] {
        const char* value = std::getenv("DUALSCREEN_TOUCH_CONTROLS");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    return forced;
}

}  // namespace ds
