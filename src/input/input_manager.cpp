#include "input/input_manager.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "util/log.hpp"

namespace ds {
namespace {

// How far either finger may travel and still be a tap, as a fraction of the
// display -- SDL reports touches normalised, so this is the same gesture on a
// phone and on a tablet.
constexpr float kTwoFingerTapSlip = 0.03f;
// And how long it may last. Long enough not to punish a deliberate press,
// short enough that two fingers resting on the glass are not it.
constexpr uint32_t kTwoFingerTapMs = 700;

}  // namespace

namespace {

DsButton ds_button_for_key(SDL_Keycode key, bool& found) {
    found = true;
    switch (key) {
        case SDLK_z: return DsButton::B;
        case SDLK_x: return DsButton::A;
        case SDLK_a: return DsButton::Y;
        case SDLK_s: return DsButton::X;
        case SDLK_q: return DsButton::L;
        case SDLK_w: return DsButton::R;
        case SDLK_RETURN: return DsButton::Start;
        case SDLK_RSHIFT: return DsButton::Select;
        case SDLK_UP: return DsButton::Up;
        case SDLK_DOWN: return DsButton::Down;
        case SDLK_LEFT: return DsButton::Left;
        case SDLK_RIGHT: return DsButton::Right;
        default: break;
    }
    found = false;
    return DsButton::A;
}

}  // namespace

InputManager::~InputManager() { shutdown(); }

void InputManager::initialize(const Settings& settings) {
    settings_ = settings;
    settings_.stick_deadzone = std::clamp(settings_.stick_deadzone, 0.05f, 0.9f);
    settings_.trigger_deadzone = std::clamp(settings_.trigger_deadzone, 0.05f, 0.9f);

    for (int index = 0; index < SDL_NumJoysticks(); ++index) {
        if (SDL_IsGameController(index)) {
            open_controller(index);
        }
    }
}

void InputManager::shutdown() {
    // Before the controllers close: switching a sensor off asks the
    // controller about itself, which a closed one cannot answer.
    motion_.set_enabled(false);
    for (auto& entry : controllers_) {
        if (entry.second != nullptr) {
            SDL_GameControllerClose(entry.second);
        }
    }
    controllers_.clear();
}

void InputManager::open_controller(int device_index) {
    SDL_GameController* controller = SDL_GameControllerOpen(device_index);
    if (controller == nullptr) {
        log_warn(std::string("could not open controller: ") + SDL_GetError());
        return;
    }
    SDL_Joystick* joystick = SDL_GameControllerGetJoystick(controller);
    const SDL_JoystickID id = SDL_JoystickInstanceID(joystick);
    if (controllers_.count(id) != 0) {
        SDL_GameControllerClose(controller);
        return;
    }
    controllers_[id] = controller;
    // Sensors are per controller and have to be switched on before SDL will
    // report them, so one arriving mid-session has to be told -- see
    // MotionReader::controller_added().
    motion_.controller_added(controller);

    // Logged in full because a mis-mapped pad is otherwise very hard to
    // diagnose remotely: this one line says what SDL thinks the pad is, which
    // decides how its face buttons are reported.
    const char* name = SDL_GameControllerName(controller);
    log_info(std::string("controller connected: ") + (name != nullptr ? name : "unknown") +
             " (SDL type: " + to_string(SDL_GameControllerGetType(controller)) + ")");

    char* mapping = SDL_GameControllerMapping(controller);
    if (mapping != nullptr) {
        log_debug(std::string("controller mapping: ") + mapping);
        SDL_free(mapping);
    }

    if (controllers_.size() > 1) {
        // Normal under Steam Input, which adds a virtual pad alongside the real
        // device. Their inputs are merged and duplicate menu actions dropped.
        log_info("now tracking " + std::to_string(controllers_.size()) +
                 " controllers; duplicate presses are merged");
    }
}

void InputManager::close_controller(SDL_JoystickID instance_id) {
    const auto it = controllers_.find(instance_id);
    if (it == controllers_.end()) {
        return;
    }
    if (it->second != nullptr) {
        motion_.controller_removed(it->second);
        SDL_GameControllerClose(it->second);
    }
    controllers_.erase(it);
    log_info("controller disconnected");
}

void InputManager::set_rumble(bool active) {
    // SDL has no "rumble until told otherwise" mode, only a duration -- so a
    // short one is used and refreshed by every repeated "on" this is called
    // with while the far end's motor stays on, which is what makes a run of
    // such calls feel continuous rather than pulsed. If an "off" is ever
    // lost in transit this stops on its own within a quarter second instead
    // of buzzing forever, the same self-healing shape as everything else
    // this link tolerates dropping.
    constexpr uint32_t kRumbleMs = 250;
    const uint16_t strength = active ? 0xffffu : 0u;
    const uint32_t duration = active ? kRumbleMs : 0u;
    for (const auto& [instance_id, controller] : controllers_) {
        if (controller == nullptr) continue;
        SDL_GameControllerRumble(controller, strength, strength, duration);
    }
}

void InputManager::set_game_input_enabled(bool enabled) {
    if (enabled && !game_input_enabled_) {
        // Anything already held when the game starts listening again was meant
        // for the menu -- most obviously the A press that chose "Resume".
        held_input_gate_.arm();
    } else if (!enabled) {
        held_input_gate_.reset();
    }
    game_input_enabled_ = enabled;
}

void InputManager::begin_frame() {
    ui_actions_.clear();
    pointer_events_.clear();
    window_size_changed_ = false;
}

void InputManager::forget_touches() {
    fingers_.clear();
    has_primary_finger_ = false;
    two_finger_tap_live_ = false;
    mouse_down_ = false;
    pointer_active_ = false;
}

void InputManager::push_action(UiAction action) {
    // Deduplicated within the frame. Steam Input presents a virtual pad while
    // the physical device often stays visible too, so a single press arrives
    // as two identical button-down events. Acting on both is actively wrong in
    // a menu: two Confirms in the browser step into a folder and straight back
    // out of it, which looks like the button doing nothing at all. No menu
    // wants the same discrete action twice in one frame regardless.
    if (std::find(ui_actions_.begin(), ui_actions_.end(), action) != ui_actions_.end()) {
        return;
    }
    ui_actions_.push_back(action);
}

void InputManager::handle_event(const SDL_Event& event) {
    switch (event.type) {
        case SDL_QUIT:
            quit_requested_ = true;
            break;

        case SDL_WINDOWEVENT:
            if (event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
                event.window.event == SDL_WINDOWEVENT_RESIZED) {
                window_size_changed_ = true;
            }
            // Whatever was being touched when the window went away will never
            // be lifted here: the release goes wherever the focus went. A
            // finger left down forever is a finger the count never loses, and
            // the gestures that ask how many are down stop working for the rest
            // of the session.
            if (event.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                forget_touches();
            }
            break;

        // The same on a phone, where the window is taken away rather than
        // merely unfocused.
        case SDL_APP_WILLENTERBACKGROUND:
            forget_touches();
            break;

        // Controllers are opened and closed as they appear, so hot-plugging a
        // pad mid-session (common on a Deck docked to a TV) just works.
        case SDL_CONTROLLERDEVICEADDED:
            open_controller(event.cdevice.which);
            break;
        case SDL_CONTROLLERDEVICEREMOVED:
            close_controller(event.cdevice.which);
            break;

        case SDL_CONTROLLERBUTTONDOWN: {
            const SDL_GameControllerButton button =
                static_cast<SDL_GameControllerButton>(event.cbutton.button);

            // Every press is logged under --verbose with the name SDL gives it,
            // so a pad whose buttons arrive as the wrong thing can be
            // identified from a log rather than guessed at.
            const char* sdl_name = SDL_GameControllerGetStringForButton(button);
            log_debug(std::string("controller button down: ") +
                      (sdl_name != nullptr ? sdl_name : "?") + " (" +
                      std::to_string(static_cast<int>(button)) + ")");

            // Which of the two lower face buttons means yes is the player's
            // to choose -- east by default, south for anyone who grew up on a
            // PlayStation. The other one of the pair is always the other
            // answer, so the two can never both mean the same thing.
            if (button == confirm_button_for(confirm_button_)) {
                push_action(UiAction::Confirm);
            } else if (button == cancel_button_for(confirm_button_)) {
                push_action(UiAction::Cancel);
            } else if (button == kMenuQuitButton) {
                push_action(UiAction::MenuQuit);
            } else if (button == kMenuResetButton) {
                push_action(UiAction::MenuReset);
            } else if (button == kMenuDisconnectButton) {
                push_action(UiAction::MenuDisconnect);
            } else if (button == kMenuSettingsButton) {
                push_action(UiAction::MenuSettings);
            }

            switch (button) {
                case SDL_CONTROLLER_BUTTON_DPAD_UP: push_action(UiAction::Up); break;
                case SDL_CONTROLLER_BUTTON_DPAD_DOWN: push_action(UiAction::Down); break;
                case SDL_CONTROLLER_BUTTON_DPAD_LEFT: push_action(UiAction::Left); break;
                case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: push_action(UiAction::Right); break;
                case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: push_action(UiAction::PageUp); break;
                case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: push_action(UiAction::PageDown); break;
                // L3 and R3 are not acted on here. Alone they are still the
                // layout swap and the smoothing toggle, but together they are
                // the menu, and which of the three a press turns out to be is
                // not known until it is released -- see end_frame().
#ifdef __ANDROID__
                // A pad's own home button, which on Android reaches the app.
                // Elsewhere the system takes it first -- Steam on a Deck, the
                // console itself on a Switch -- so there is nothing to bind.
                case SDL_CONTROLLER_BUTTON_GUIDE: push_action(UiAction::ToggleMenu); break;
#endif
                default: break;
            }
            break;
        }

        case SDL_KEYDOWN: {
            if (event.key.repeat != 0) break;

            // Logged for the same reason controller buttons are: a Steam Input
            // layout that sends keystrokes instead of gamepad buttons is
            // otherwise invisible from inside the app.
            log_debug(std::string("key down: ") + SDL_GetKeyName(event.key.keysym.sym));

            switch (event.key.keysym.sym) {
                case SDLK_ESCAPE:
                case SDLK_BACKSPACE: push_action(UiAction::Cancel); break;
                // F1 is the menu key. No controller button opens the menu:
                // bind F1 in Steam Input instead, which is the only way that
                // survives Steam's own claim on the pad.
                case SDLK_F1:
                case SDLK_TAB: push_action(UiAction::ToggleMenu); break;
                // The keyboard equivalent, so a Steam Input layout that sends
                // keystrokes rather than pad buttons can reach it too.
                case SDLK_F2: push_action(UiAction::ToggleFilter); break;
                case SDLK_F3: push_action(UiAction::SwapLayout); break;
                // Keyboard equivalents of the menu's north and west face
                // buttons, in the same undiscoverable-but-there spirit as the
                // function keys above: without them a machine with no pad
                // could not reset or quit a game from the menu at all.
                case SDLK_F5: push_action(UiAction::MenuReset); break;
                case SDLK_F6: push_action(UiAction::MenuQuit); break;
                case SDLK_F4: push_action(UiAction::MenuSettings); break;
                case SDLK_F7: push_action(UiAction::MenuDisconnect); break;
                case SDLK_F10: push_action(UiAction::Quit); break;
                // Return doubles as DS Start in game, but game input is off
                // while a menu is open, so there is no conflict.
                case SDLK_SPACE:
                case SDLK_RETURN:
                case SDLK_KP_ENTER: push_action(UiAction::Confirm); break;
                default: break;
            }
            // Arrow keys double as menu navigation; they are also DS d-pad keys,
            // and the menu/game split decides which one actually applies.
            switch (event.key.keysym.sym) {
                case SDLK_UP: push_action(UiAction::Up); break;
                case SDLK_DOWN: push_action(UiAction::Down); break;
                case SDLK_LEFT: push_action(UiAction::Left); break;
                case SDLK_RIGHT: push_action(UiAction::Right); break;
                case SDLK_PAGEUP: push_action(UiAction::PageUp); break;
                case SDLK_PAGEDOWN: push_action(UiAction::PageDown); break;
                default: break;
            }
            break;
        }

        case SDL_FINGERDOWN: {
            // A desktop that turns the mouse into a finger for us: the real
            // mouse events are handled below, and taking both would count every
            // click twice.
            if (event.tfinger.touchId == SDL_MOUSE_TOUCHID) break;
            FingerState state;
            state.x = event.tfinger.x;
            state.y = event.tfinger.y;
            state.press_x = state.x;
            state.press_y = state.y;
            fingers_[event.tfinger.fingerId] = state;

            if (!has_primary_finger_) {
                has_primary_finger_ = true;
                primary_finger_ = event.tfinger.fingerId;
                PointerEvent pointer;
                pointer.x = state.x;
                pointer.y = state.y;
                pointer.press_x = state.press_x;
                pointer.press_y = state.press_y;
                pointer.pressed = true;
                pointer_events_.push_back(pointer);
            } else if (fingers_.size() == 2) {
                // Two fingers tapped together is the way into the menu on a
                // device with no buttons, and it is the only way in there: a
                // button drawn over the game would sit on the picture for the
                // whole session.
                //
                // Not gated on game_owns_controls_, which covers the stick
                // clicks a Wii U game reads. No console this streams has a
                // multi-touch screen -- a DS, a 3DS and a GamePad are all
                // single-touch, and the wire carries one point -- so a second
                // finger is never something the game could have wanted.
                //
                // Armed here and fired on release, rather than the moment the
                // second finger lands. A finger resting while the other one
                // draws is not a tap, and on the screen a stylus belongs to
                // that is a thing people do.
                two_finger_tap_live_ = true;
                two_finger_tap_began_ms_ = SDL_GetTicks();
            } else if (fingers_.size() > 2) {
                // Three is not two.
                two_finger_tap_live_ = false;
            }
            break;
        }

        case SDL_FINGERMOTION: {
            if (event.tfinger.touchId == SDL_MOUSE_TOUCHID) break;
            const auto it = fingers_.find(event.tfinger.fingerId);
            if (it != fingers_.end()) {
                it->second.x = event.tfinger.x;
                it->second.y = event.tfinger.y;
                // A tap does not travel. Anything that does is a drag, a
                // pinch or a hand resting on the glass, and none of those
                // means "open the menu".
                if (two_finger_tap_live_ &&
                    (std::abs(it->second.x - it->second.press_x) > kTwoFingerTapSlip ||
                     std::abs(it->second.y - it->second.press_y) > kTwoFingerTapSlip)) {
                    two_finger_tap_live_ = false;
                }
            }
            break;
        }

        case SDL_FINGERUP: {
            if (event.tfinger.touchId == SDL_MOUSE_TOUCHID) break;
            const auto lifted = fingers_.find(event.tfinger.fingerId);
            const bool known = lifted != fingers_.end();
            const float press_x = known ? lifted->second.press_x : event.tfinger.x;
            const float press_y = known ? lifted->second.press_y : event.tfinger.y;
            if (known) fingers_.erase(lifted);
            if (has_primary_finger_ && event.tfinger.fingerId == primary_finger_) {
                has_primary_finger_ = false;
                PointerEvent pointer;
                pointer.x = event.tfinger.x;
                pointer.y = event.tfinger.y;
                pointer.press_x = press_x;
                pointer.press_y = press_y;
                pointer.released = true;
                pointer_events_.push_back(pointer);
            }
            // Lifting the first of the two is the tap. Checked as the count
            // falls below two rather than once every last finger has gone: a
            // finger that stays put, or one whose release was lost, would
            // otherwise leave the gesture armed forever.
            if (two_finger_tap_live_ && fingers_.size() < 2) {
                two_finger_tap_live_ = false;
                if (SDL_GetTicks() - two_finger_tap_began_ms_ <= kTwoFingerTapMs) {
                    push_action(UiAction::ToggleMenu);
                }
            }
            break;
        }

        // Mouse events keep the app usable on a desktop, where they stand in
        // for the touchscreen.
        // A phone reports one tap twice: as a finger, and again as a mouse
        // synthesized from that finger. The finger is the real one -- it is the
        // only one that says how many are down -- so the synthetic mouse is
        // dropped rather than counted a second time in the other's units.
        case SDL_MOUSEBUTTONDOWN:
            if (event.button.which == SDL_TOUCH_MOUSEID) break;
            if (event.button.button == SDL_BUTTON_LEFT) {
                mouse_down_ = true;
                mouse_x_ = static_cast<float>(event.button.x);
                mouse_y_ = static_cast<float>(event.button.y);
                mouse_press_x_ = mouse_x_;
                mouse_press_y_ = mouse_y_;
                PointerEvent pointer;
                pointer.x = mouse_x_;
                pointer.y = mouse_y_;
                pointer.press_x = mouse_press_x_;
                pointer.press_y = mouse_press_y_;
                pointer.pressed = true;
                pointer.normalized = false;
                pointer_events_.push_back(pointer);
            }
            break;

        case SDL_MOUSEBUTTONUP:
            if (event.button.which == SDL_TOUCH_MOUSEID) break;
            if (event.button.button == SDL_BUTTON_LEFT) {
                mouse_down_ = false;
                mouse_x_ = static_cast<float>(event.button.x);
                mouse_y_ = static_cast<float>(event.button.y);
                PointerEvent pointer;
                pointer.x = mouse_x_;
                pointer.y = mouse_y_;
                pointer.press_x = mouse_press_x_;
                pointer.press_y = mouse_press_y_;
                pointer.released = true;
                pointer.normalized = false;
                pointer_events_.push_back(pointer);
            }
            break;

        case SDL_MOUSEMOTION:
            mouse_x_ = static_cast<float>(event.motion.x);
            mouse_y_ = static_cast<float>(event.motion.y);
            break;

        default:
            break;
    }
}

void InputManager::emit_navigation_from_axes() {
    float x = 0.0f;
    float y = 0.0f;
    for (const auto& entry : controllers_) {
        if (entry.second == nullptr) continue;
        const float axis_x =
            SDL_GameControllerGetAxis(entry.second, SDL_CONTROLLER_AXIS_LEFTX) / 32767.0f;
        const float axis_y =
            SDL_GameControllerGetAxis(entry.second, SDL_CONTROLLER_AXIS_LEFTY) / 32767.0f;
        if (std::fabs(axis_x) > std::fabs(x)) x = axis_x;
        if (std::fabs(axis_y) > std::fabs(y)) y = axis_y;
    }

    const float deadzone = settings_.stick_deadzone;
    const bool up = y < -deadzone;
    const bool down = y > deadzone;
    const bool left = x < -deadzone;
    const bool right = x > deadzone;

    // Edge-triggered, so holding the stick does not scroll the menu away.
    if (up && !axis_up_) push_action(UiAction::Up);
    if (down && !axis_down_) push_action(UiAction::Down);
    if (left && !axis_left_) push_action(UiAction::Left);
    if (right && !axis_right_) push_action(UiAction::Right);
    axis_up_ = up;
    axis_down_ = down;
    axis_left_ = left;
    axis_right_ = right;
}

MenuButtonLabels InputManager::menu_labels() const {
    for (const auto& entry : controllers_) {
        if (entry.second != nullptr) {
            return menu_button_labels(pad_letter_layout(SDL_GameControllerGetType(entry.second)));
        }
    }
    return MenuButtonLabels();  // Xbox letters: the common case, and Steam Input's
}

bool InputManager::stick_buttons_held(SDL_GameControllerButton button) const {
    for (const auto& entry : controllers_) {
        if (entry.second != nullptr && SDL_GameControllerGetButton(entry.second, button) != 0) {
            return true;
        }
    }
    return false;
}

DsInputState InputManager::poll_held_state() const {
    // Polled unconditionally, even while a menu is open: the gate needs the
    // true physical state to know what to suppress when the game resumes.
    DsInputState held;

    for (const auto& entry : controllers_) {
        SDL_GameController* controller = entry.second;
        if (controller == nullptr) continue;

        for (int index = 0; index < SDL_CONTROLLER_BUTTON_MAX; ++index) {
            const SDL_GameControllerButton button = static_cast<SDL_GameControllerButton>(index);
            DsButton ds_button = DsButton::A;
            if (ds_button_for_controller_button(button, ds_button) &&
                SDL_GameControllerGetButton(controller, button) != 0) {
                held.set(ds_button, true);
            }
        }
        // The left stick is always the circle pad. Whether it is *also* the
        // d-pad depends on the console: a DS has no stick, so reaching for one
        // has to work, but a 3DS has both and pressing both at once is what
        // made its circle pad unusable -- a core that reads the d-pad sees a
        // direction held down for as long as the stick is off centre, and
        // walking becomes the eight directions the d-pad has.
        const int16_t raw_x = SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_LEFTX);
        const int16_t raw_y = SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_LEFTY);
        const float axis_x = raw_x / 32767.0f;
        const float axis_y = raw_y / 32767.0f;
        const float deadzone = settings_.stick_deadzone;
        const int16_t deadzone_threshold = static_cast<int16_t>(deadzone * 32767.0f);
        if (stick_is_dpad_) {
            if (axis_y < -deadzone) held.set(DsButton::Up, true);
            if (axis_y > deadzone) held.set(DsButton::Down, true);
            if (axis_x < -deadzone) held.set(DsButton::Left, true);
            if (axis_x > deadzone) held.set(DsButton::Right, true);
        }

        // The strongest deflection across every pad wins, so two controllers do
        // not average each other out into standing still.
        if (std::abs(raw_x) > std::abs(held.circle_x)) held.circle_x = raw_x;
        if (std::abs(raw_y) > std::abs(held.circle_y)) held.circle_y = raw_y;

        // What the right stick is depends on what the game wants -- a C-Stick,
        // a second circle pad, a d-pad, or nothing. See RightStickMode.
        const int16_t raw_c_x = SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_RIGHTX);
        const int16_t raw_c_y = SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_RIGHTY);
        switch (right_stick_mode_) {
            case RightStickMode::CStick:
                if (std::abs(raw_c_x) > std::abs(held.c_stick_x)) held.c_stick_x = raw_c_x;
                if (std::abs(raw_c_y) > std::abs(held.c_stick_y)) held.c_stick_y = raw_c_y;
                break;
            case RightStickMode::CirclePad:
                // Into the same axes as the left stick, competing on
                // deflection like a second pad does: two sticks that averaged
                // each other out would leave a player standing still while
                // pushing one of them.
                if (std::abs(raw_c_x) > std::abs(held.circle_x)) held.circle_x = raw_c_x;
                if (std::abs(raw_c_y) > std::abs(held.circle_y)) held.circle_y = raw_c_y;
                break;
            case RightStickMode::DPad:
                if (raw_c_y < -deadzone_threshold) held.set(DsButton::Up, true);
                if (raw_c_y > deadzone_threshold) held.set(DsButton::Down, true);
                if (raw_c_x < -deadzone_threshold) held.set(DsButton::Left, true);
                if (raw_c_x > deadzone_threshold) held.set(DsButton::Right, true);
                break;
            case RightStickMode::Off:
                break;
        }

        const float trigger_deadzone = settings_.trigger_deadzone;
        const float left_trigger =
            SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT) / 32767.0f;
        const float right_trigger =
            SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) / 32767.0f;
        if (game_owns_controls_) {
            // A console with four shoulder buttons and two sticks that press
            // has somewhere for all of them to go, and the game reads them all.
            // See set_game_owns_controls().
            if (left_trigger > trigger_deadzone) held.set(DsButton::Zl, true);
            if (right_trigger > trigger_deadzone) held.set(DsButton::Zr, true);
            if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_LEFTSTICK) != 0) {
                held.set(DsButton::LeftStick, true);
            }
            if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_RIGHTSTICK) != 0) {
                held.set(DsButton::RightStick, true);
            }
        } else {
            // The triggers double as L and R. The DS's shoulder buttons are
            // digital, but plenty of people reach for the triggers first, and a
            // pad that has both should honour either. The stick presses are
            // this app's own shortcuts here, so they are not the game's.
            if (left_trigger > trigger_deadzone) held.set(DsButton::L, true);
            if (right_trigger > trigger_deadzone) held.set(DsButton::R, true);
            // Except this one, on a console that has a microphone: it goes to
            // the game as L3, which is where melonDS listens for it.
            if (left_stick_is_mic_ &&
                SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_LEFTSTICK) != 0) {
                held.set(DsButton::LeftStick, true);
            }
        }
    }

    const Uint8* keys = SDL_GetKeyboardState(nullptr);
    if (keys != nullptr) {
        static const SDL_Keycode kKeys[] = {SDLK_z, SDLK_x, SDLK_a, SDLK_s, SDLK_q, SDLK_w,
                                            SDLK_RETURN, SDLK_RSHIFT, SDLK_UP, SDLK_DOWN,
                                            SDLK_LEFT, SDLK_RIGHT};
        for (const SDL_Keycode key : kKeys) {
            bool found = false;
            const DsButton button = ds_button_for_key(key, found);
            if (found && keys[SDL_GetScancodeFromKey(key)] != 0) {
                held.set(button, true);
            }
        }
    }

    return held;
}

bool InputManager::update_pointer(float output_width, float output_height) {
    float point_x = 0.0f;
    float point_y = 0.0f;
    bool have_point = false;

    if (has_primary_finger_) {
        const auto it = fingers_.find(primary_finger_);
        if (it != fingers_.end()) {
            // SDL touch coordinates are normalized to the window.
            point_x = it->second.x * output_width;
            point_y = it->second.y * output_height;
            have_point = true;
        }
    } else if (mouse_down_) {
        point_x = mouse_x_;
        point_y = mouse_y_;
        have_point = true;
    }

    // Kept up to date whether or not the game is listening, because the menus
    // need it for drag scrolling.
    pointer_active_ = have_point;
    pointer_x_ = point_x;
    pointer_y_ = point_y;
    return have_point;
}

void InputManager::end_frame(const Layout& layout,
                             float output_width,
                             float output_height,
                             const SystemScreens& screens,
                             float display_width,
                             float display_height) {
    // Normalize every pointer event into output pixels now that the size is known.
    for (PointerEvent& pointer : pointer_events_) {
        if (pointer.normalized) {
            pointer.x *= output_width;
            pointer.y *= output_height;
            pointer.press_x *= output_width;
            pointer.press_y *= output_height;
            pointer.normalized = false;
        }
    }

    emit_navigation_from_axes();

    // Once per frame, alongside the buttons, so that the reading sent with a
    // pad update is the one taken at the same moment as the buttons on it.
    motion_.poll();

    const DsInputState held = poll_held_state();

    // L3 and R3 together open the menu. Neither reaches a DS or 3DS button, so
    // the pair costs a game nothing -- unlike Start and Select, which plenty
    // of DS games read together as a soft reset, a poor thing to do to someone
    // on their way into the menu.
    //
    // Apart they keep their own meanings, but a press only becomes one of
    // those once it is released without the other having joined it. Otherwise
    // reaching for the chord would swap the layout on the way in.
    const bool left = stick_buttons_held(SDL_CONTROLLER_BUTTON_LEFTSTICK);
    const bool right = stick_buttons_held(SDL_CONTROLLER_BUTTON_RIGHTSTICK);
    if (left && right && !stick_chord_used_) {
        // Still marked used either way, so that letting go of the pair does not
        // then fire the two singles below on a console that allows them.
        stick_chord_used_ = true;
        if (!game_owns_controls_) {
            push_action(UiAction::ToggleMenu);
        }
    }
    // Only where the game does not want them. See set_game_owns_controls().
    if (!game_owns_controls_) {
        if (left_stick_held_ && !left && !stick_chord_used_ && !left_stick_is_mic_) {
            push_action(UiAction::SwapLayout);
        }
        if (right_stick_held_ && !right && !stick_chord_used_) {
            push_action(UiAction::ToggleFilter);
        }
    }
    if (!left && !right) {
        stick_chord_used_ = false;
    }
    left_stick_held_ = left;
    right_stick_held_ = right;

    const bool pointer_down = update_pointer(output_width, output_height);
    // Off centre by enough that the menu would have moved on it, which is the
    // deflection worth holding back after the menu closes.
    const int16_t deadzone_threshold = static_cast<int16_t>(settings_.stick_deadzone * 32767.0f);
    const bool stick_off_centre =
        std::abs(held.circle_x) > deadzone_threshold || std::abs(held.circle_y) > deadzone_threshold;
    const bool c_stick_off_centre = std::abs(held.c_stick_x) > deadzone_threshold ||
                                    std::abs(held.c_stick_y) > deadzone_threshold;
    held_input_gate_.update(held.buttons, pointer_down, stick_off_centre, c_stick_off_centre);

    ds_state_.clear();
    if (game_input_enabled_) {
        ds_state_.buttons = held_input_gate_.buttons();
        // Not gated the way buttons and sticks are. The gate exists to stop
        // the press that dismissed a menu also landing in the game, and
        // there is no equivalent for how the device is being held: it was
        // being held that way through the menu too, and a game handed a
        // frozen orientation for a moment after resuming would lurch when it
        // caught up.
        ds_state_.motion = motion_.state();
        // The sticks reach the game as themselves. Every layer below this one
        // -- the wire, the core's analog callback -- already carries them;
        // nothing filled them in, so a 3DS could be handed a stick that was
        // always centred.
        if (held_input_gate_.stick_live()) {
            ds_state_.circle_x = held.circle_x;
            ds_state_.circle_y = held.circle_y;
        }
        if (held_input_gate_.c_stick_live()) {
            ds_state_.c_stick_x = held.c_stick_x;
            ds_state_.c_stick_y = held.c_stick_y;
        }
        // Not where the interface has claimed the pixels -- see
        // set_ui_reserved_rect(). Checked before the mapping rather than
        // after, because the mapping's job is to say where in the console's
        // screen a point is, not whether it should have been asked.
        // One finger only. Every console this streams has a single-touch
        // screen, so a second finger was never going to be a second press --
        // and while two are down the player is talking to this app, not to
        // the game.
        if (held_input_gate_.touching() && fingers_.size() < 2) {
            DsTouchPoint point;
            if (map_output_point_to_ds(layout, output_width, output_height, pointer_x_, pointer_y_,
                                       point, screens, display_width, display_height)) {
                ds_state_.touching = true;
                ds_state_.touch_x = point.x;
                ds_state_.touch_y = point.y;
            }
        }
    }

    for (const UiAction action : ui_actions_) {
        if (action == UiAction::Quit) {
            quit_requested_ = true;
        }
    }
}

}  // namespace ds
