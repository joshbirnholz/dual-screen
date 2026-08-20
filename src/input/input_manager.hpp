#pragma once

#include <SDL.h>

#include <map>
#include <string>
#include <vector>

#include "config/settings.hpp"
#include "input/controller_mapping.hpp"
#include "input/ds_input.hpp"
#include "input/held_input_gate.hpp"
#include "layout/layout.hpp"

namespace ds {

// Discrete UI intents, produced from the same devices that drive the game so
// the menus are navigable by controller and by touch without either being a
// special case.
enum class UiAction {
    Up,
    Down,
    Left,
    Right,
    PageUp,
    PageDown,
    Confirm,
    Cancel,
    ToggleMenu,
    // Smooth or sharp screen scaling. Works wherever you are -- in a game or
    // in a menu -- so the difference can be judged against what is on screen.
    ToggleFilter,
    Quit,
};

// A touch/click that landed somewhere in the window, in output pixels. The UI
// hit-tests these itself; the emulated DS never sees them while a menu is open.
struct PointerEvent {
    float x = 0.0f;
    float y = 0.0f;
    bool pressed = false;   // went down this frame
    bool released = false;  // came up this frame
    // SDL reports touch normalized to the window and the mouse in pixels; both
    // are converted to pixels in end_frame(), where the output size is known.
    bool normalized = true;
};

// Collects SDL input and turns it into DS input plus UI intents.
class InputManager {
public:
    ~InputManager();

    void initialize(const Settings& settings);
    void shutdown();

    // Call once per frame before pumping events.
    void begin_frame();
    // Feed every SDL event through here.
    void handle_event(const SDL_Event& event);
    // Call after the event pump. `layout` is the layout actually being drawn,
    // which is what decides where a touch on the bottom screen lands in DS
    // coordinates.
    void end_frame(const Layout& layout,
                   float output_width,
                   float output_height,
                   const SystemScreens& screens);

    const DsInputState& ds_state() const { return ds_state_; }
    const std::vector<UiAction>& ui_actions() const { return ui_actions_; }
    const std::vector<PointerEvent>& pointer_events() const { return pointer_events_; }
    bool quit_requested() const { return quit_requested_; }
    bool window_size_changed() const { return window_size_changed_; }

    // While a menu is open the DS gets no input at all, so a stray press in the
    // menu cannot leak into the game.
    void set_game_input_enabled(bool enabled);

    // Whether a finger or the mouse button is currently down, and where. UI
    // screens use this for drag scrolling; the discrete press/release events in
    // pointer_events() only describe taps.
    bool pointer_active() const { return pointer_active_; }
    float pointer_x() const { return pointer_x_; }
    float pointer_y() const { return pointer_y_; }

    int connected_controllers() const { return static_cast<int>(controllers_.size()); }

    // Letters printed on the menu confirm/cancel buttons for the connected pad,
    // for on-screen prompts. Display only -- it never changes what a button does.
    MenuButtonLabels menu_labels() const;

private:
    void open_controller(int device_index);
    void close_controller(SDL_JoystickID instance_id);
    // Physically-held DS buttons, before the held-input gate is applied.
    uint32_t poll_held_buttons() const;
    // Updates the live pointer position; returns whether one is down.
    bool update_pointer(float output_width, float output_height);
    void emit_navigation_from_axes();
    void push_action(UiAction action);

    struct FingerState {
        float x = 0.0f;
        float y = 0.0f;
    };

    Settings settings_;
    std::map<SDL_JoystickID, SDL_GameController*> controllers_;

    DsInputState ds_state_;
    std::vector<UiAction> ui_actions_;
    std::vector<PointerEvent> pointer_events_;
    bool quit_requested_ = false;
    bool window_size_changed_ = false;
    bool game_input_enabled_ = true;

    // Keeps the press that dismissed a menu from also registering in the game.
    HeldInputGate held_input_gate_;

    // Touch tracking. The first finger down drives the DS touchscreen; a second
    // simultaneous finger opens the menu, which is the only way in on a device
    // with no buttons at all.
    std::map<SDL_FingerID, FingerState> fingers_;
    SDL_FingerID primary_finger_ = 0;
    bool has_primary_finger_ = false;
    bool mouse_down_ = false;
    float mouse_x_ = 0.0f;
    float mouse_y_ = 0.0f;
    bool second_finger_menu_armed_ = false;

    // Where the active finger or mouse press currently is, in output pixels.
    bool pointer_active_ = false;
    float pointer_x_ = 0.0f;
    float pointer_y_ = 0.0f;

    // Stick-as-dpad edge detection for menu navigation.
    bool axis_up_ = false;
    bool axis_down_ = false;
    bool axis_left_ = false;
    bool axis_right_ = false;
};

}  // namespace ds
