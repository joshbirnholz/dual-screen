#include "input/input_manager.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "util/log.hpp"

namespace ds {
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
        SDL_GameControllerClose(it->second);
    }
    controllers_.erase(it);
    log_info("controller disconnected");
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

            // Menus confirm on the east button and go back on the south one --
            // the same positions the game treats as DS A and B.
            if (button == kMenuConfirmButton) {
                push_action(UiAction::Confirm);
            } else if (button == kMenuCancelButton) {
                push_action(UiAction::Cancel);
            }

            switch (button) {
                case SDL_CONTROLLER_BUTTON_DPAD_UP: push_action(UiAction::Up); break;
                case SDL_CONTROLLER_BUTTON_DPAD_DOWN: push_action(UiAction::Down); break;
                case SDL_CONTROLLER_BUTTON_DPAD_LEFT: push_action(UiAction::Left); break;
                case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: push_action(UiAction::Right); break;
                case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: push_action(UiAction::PageUp); break;
                case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: push_action(UiAction::PageDown); break;
                // R3 toggles filtering. It reaches no DS button, so it is free
                // to mean this everywhere without stealing a press from a game.
                case SDL_CONTROLLER_BUTTON_RIGHTSTICK: push_action(UiAction::ToggleFilter); break;
                // L3 is free for the same reason R3 is: no DS or 3DS button
                // reaches it, so it can mean this without a game ever missing
                // a press.
                case SDL_CONTROLLER_BUTTON_LEFTSTICK: push_action(UiAction::SwapLayout); break;
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
            FingerState state;
            state.x = event.tfinger.x;
            state.y = event.tfinger.y;
            fingers_[event.tfinger.fingerId] = state;

            if (!has_primary_finger_) {
                has_primary_finger_ = true;
                primary_finger_ = event.tfinger.fingerId;
                PointerEvent pointer;
                pointer.x = state.x;
                pointer.y = state.y;
                pointer.pressed = true;
                pointer_events_.push_back(pointer);
            } else if (fingers_.size() >= 2 && !second_finger_menu_armed_) {
                // Two fingers at once is the touch-only escape hatch into the
                // menu — a device with no buttons still needs a way in.
                second_finger_menu_armed_ = true;
                push_action(UiAction::ToggleMenu);
            }
            break;
        }

        case SDL_FINGERMOTION: {
            const auto it = fingers_.find(event.tfinger.fingerId);
            if (it != fingers_.end()) {
                it->second.x = event.tfinger.x;
                it->second.y = event.tfinger.y;
            }
            break;
        }

        case SDL_FINGERUP: {
            fingers_.erase(event.tfinger.fingerId);
            if (has_primary_finger_ && event.tfinger.fingerId == primary_finger_) {
                has_primary_finger_ = false;
                PointerEvent pointer;
                pointer.x = event.tfinger.x;
                pointer.y = event.tfinger.y;
                pointer.released = true;
                pointer_events_.push_back(pointer);
            }
            if (fingers_.empty()) {
                second_finger_menu_armed_ = false;
            }
            break;
        }

        // Mouse events keep the app usable on a desktop, where they stand in
        // for the touchscreen.
        case SDL_MOUSEBUTTONDOWN:
            if (event.button.button == SDL_BUTTON_LEFT) {
                mouse_down_ = true;
                mouse_x_ = static_cast<float>(event.button.x);
                mouse_y_ = static_cast<float>(event.button.y);
                PointerEvent pointer;
                pointer.x = mouse_x_;
                pointer.y = mouse_y_;
                pointer.pressed = true;
                pointer.normalized = false;
                pointer_events_.push_back(pointer);
            }
            break;

        case SDL_MOUSEBUTTONUP:
            if (event.button.button == SDL_BUTTON_LEFT) {
                mouse_down_ = false;
                mouse_x_ = static_cast<float>(event.button.x);
                mouse_y_ = static_cast<float>(event.button.y);
                PointerEvent pointer;
                pointer.x = mouse_x_;
                pointer.y = mouse_y_;
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

        // The right stick stands in for a Circle Pad Pro or New 3DS's
        // C-Stick, the same way the left one is the circle pad.
        const int16_t raw_c_x = SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_RIGHTX);
        const int16_t raw_c_y = SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_RIGHTY);
        if (std::abs(raw_c_x) > std::abs(held.c_stick_x)) held.c_stick_x = raw_c_x;
        if (std::abs(raw_c_y) > std::abs(held.c_stick_y)) held.c_stick_y = raw_c_y;

        // The triggers double as L and R. The DS's shoulder buttons are
        // digital, but plenty of people reach for the triggers first, and a
        // pad that has both should honour either.
        const float trigger_deadzone = settings_.trigger_deadzone;
        const float left_trigger =
            SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT) / 32767.0f;
        const float right_trigger =
            SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) / 32767.0f;
        if (left_trigger > trigger_deadzone) held.set(DsButton::L, true);
        if (right_trigger > trigger_deadzone) held.set(DsButton::R, true);
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
                             const SystemScreens& screens) {
    // Normalize every pointer event into output pixels now that the size is known.
    for (PointerEvent& pointer : pointer_events_) {
        if (pointer.normalized) {
            pointer.x *= output_width;
            pointer.y *= output_height;
            pointer.normalized = false;
        }
    }

    emit_navigation_from_axes();

    const DsInputState held = poll_held_state();
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
        if (held_input_gate_.touching()) {
            DsTouchPoint point;
            if (map_output_point_to_ds(layout, output_width, output_height, pointer_x_, pointer_y_,
                                       point, screens)) {
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
