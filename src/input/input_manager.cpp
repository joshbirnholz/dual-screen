#include "input/input_manager.hpp"

#include <algorithm>
#include <cmath>

#include "util/log.hpp"

namespace ds {
namespace {

// SDL names face buttons by physical position (A = south). The DS names them
// the Nintendo way (B = south). Mapping through position rather than letter is
// what makes a stock Xbox-layout pad feel right on a DS game.
struct ButtonMapping {
    SDL_GameControllerButton sdl;
    DsButton ds;
};

constexpr ButtonMapping kButtonMappings[] = {
    {SDL_CONTROLLER_BUTTON_A, DsButton::B},
    {SDL_CONTROLLER_BUTTON_B, DsButton::A},
    {SDL_CONTROLLER_BUTTON_X, DsButton::Y},
    {SDL_CONTROLLER_BUTTON_Y, DsButton::X},
    {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, DsButton::L},
    {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, DsButton::R},
    {SDL_CONTROLLER_BUTTON_START, DsButton::Start},
    {SDL_CONTROLLER_BUTTON_BACK, DsButton::Select},
    {SDL_CONTROLLER_BUTTON_DPAD_UP, DsButton::Up},
    {SDL_CONTROLLER_BUTTON_DPAD_DOWN, DsButton::Down},
    {SDL_CONTROLLER_BUTTON_DPAD_LEFT, DsButton::Left},
    {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, DsButton::Right},
};

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
    const char* name = SDL_GameControllerName(controller);
    log_info(std::string("controller connected: ") + (name != nullptr ? name : "unknown"));
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

void InputManager::push_action(UiAction action) { ui_actions_.push_back(action); }

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
            switch (static_cast<SDL_GameControllerButton>(event.cbutton.button)) {
                case SDL_CONTROLLER_BUTTON_DPAD_UP: push_action(UiAction::Up); break;
                case SDL_CONTROLLER_BUTTON_DPAD_DOWN: push_action(UiAction::Down); break;
                case SDL_CONTROLLER_BUTTON_DPAD_LEFT: push_action(UiAction::Left); break;
                case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: push_action(UiAction::Right); break;
                case SDL_CONTROLLER_BUTTON_A: push_action(UiAction::Confirm); break;
                case SDL_CONTROLLER_BUTTON_B: push_action(UiAction::Cancel); break;
                case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: push_action(UiAction::PageUp); break;
                case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: push_action(UiAction::PageDown); break;
                // R3 opens and closes the menu. Steam grabs Guide in Game Mode,
                // so that is not available to us.
                case SDL_CONTROLLER_BUTTON_RIGHTSTICK: push_action(UiAction::ToggleMenu); break;
                default: break;
            }
            break;
        }

        case SDL_KEYDOWN: {
            if (event.key.repeat != 0) break;
            switch (event.key.keysym.sym) {
                case SDLK_ESCAPE: push_action(UiAction::Cancel); break;
                case SDLK_F1:
                case SDLK_TAB: push_action(UiAction::ToggleMenu); break;
                case SDLK_F10: push_action(UiAction::Quit); break;
                case SDLK_SPACE: push_action(UiAction::Confirm); break;
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

uint32_t InputManager::poll_held_buttons() const {
    // Polled unconditionally, even while a menu is open: the gate needs the
    // true physical state to know what to suppress when the game resumes.
    DsInputState held;

    for (const auto& entry : controllers_) {
        SDL_GameController* controller = entry.second;
        if (controller == nullptr) continue;
        for (const ButtonMapping& mapping : kButtonMappings) {
            if (SDL_GameControllerGetButton(controller, mapping.sdl) != 0) {
                held.set(mapping.ds, true);
            }
        }
        // The left stick doubles as the d-pad; the DS has no analogue stick, and
        // players expect the stick to just work.
        const float axis_x = SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_LEFTX) / 32767.0f;
        const float axis_y = SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_LEFTY) / 32767.0f;
        const float deadzone = settings_.stick_deadzone;
        if (axis_y < -deadzone) held.set(DsButton::Up, true);
        if (axis_y > deadzone) held.set(DsButton::Down, true);
        if (axis_x < -deadzone) held.set(DsButton::Left, true);
        if (axis_x > deadzone) held.set(DsButton::Right, true);

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

    return held.buttons;
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

void InputManager::end_frame(const Layout& layout, float output_width, float output_height) {
    // Normalize every pointer event into output pixels now that the size is known.
    for (PointerEvent& pointer : pointer_events_) {
        if (pointer.normalized) {
            pointer.x *= output_width;
            pointer.y *= output_height;
            pointer.normalized = false;
        }
    }

    emit_navigation_from_axes();

    const uint32_t held_buttons = poll_held_buttons();
    const bool pointer_down = update_pointer(output_width, output_height);
    held_input_gate_.update(held_buttons, pointer_down);

    ds_state_.clear();
    if (game_input_enabled_) {
        ds_state_.buttons = held_input_gate_.buttons();
        if (held_input_gate_.touching()) {
            DsTouchPoint point;
            if (map_output_point_to_ds(layout, output_width, output_height, pointer_x_, pointer_y_,
                                       point)) {
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
