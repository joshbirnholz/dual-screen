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
    // Move to the layout the current one pairs with, if it has one -- see
    // Layout::swap_layout_id. Only means anything with a game on screen; in a
    // menu every layout is already a tile away.
    SwapLayout,
    // Quit the app outright, from a key that means nothing else. Distinct from
    // MenuQuit below, which a face button raises and only a menu acts on.
    Quit,
    // What a menu's north and west face buttons ask for. Kept apart from Quit
    // because a face button is pressed constantly while playing, and an action
    // that ends the app has no business arriving from one: these mean something
    // only to a screen that is listening for them, and both go through a
    // confirmation before anything happens.
    MenuQuit,
    MenuReset,
    // Ends the two-device session, from the button a Nintendo pad prints + on.
    // Menu-only for the same reason as the two above: + is pressed constantly
    // while playing.
    MenuDisconnect,
};

// A touch/click that landed somewhere in the window, in output pixels. The UI
// hit-tests these itself; the emulated DS never sees them while a menu is open.
struct PointerEvent {
    float x = 0.0f;
    float y = 0.0f;
    bool pressed = false;   // went down this frame
    bool released = false;  // came up this frame
    // Where the same finger or button went down, carried on the release so a
    // widget can tell a tap aimed at it from one that landed elsewhere and slid
    // across. Equal to x/y on a press.
    float press_x = 0.0f;
    float press_y = 0.0f;
    // SDL reports touch normalized to the window and the mouse in pixels; both
    // are converted to pixels in end_frame(), where the output size is known.
    bool normalized = true;
};

// True when a tap both began and ended inside `rect`. A finger that came down
// somewhere else and lifted here was not aiming at this, and a finger that came
// down here and slid away was taking it back -- neither is a press.
inline bool tapped(const PointerEvent& pointer, const PixelRect& rect) {
    return pointer.released && rect.contains(pointer.x, pointer.y) &&
           rect.contains(pointer.press_x, pointer.press_y);
}

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
    // `display_width`/`display_height` are the size of the display the picture
    // actually lands on, when that is not the size being drawn at -- see
    // Compositor::set_display_size(). Touches are mapped back through the same
    // layout the compositor drew, so this has to be the same answer.
    void end_frame(const Layout& layout,
                   float output_width,
                   float output_height,
                   const SystemScreens& screens,
                   float display_width = 0.0f,
                   float display_height = 0.0f);

    const DsInputState& ds_state() const { return ds_state_; }
    const std::vector<UiAction>& ui_actions() const { return ui_actions_; }
    const std::vector<PointerEvent>& pointer_events() const { return pointer_events_; }
    bool quit_requested() const { return quit_requested_; }
    bool window_size_changed() const { return window_size_changed_; }

    // While a menu is open the DS gets no input at all, so a stray press in the
    // menu cannot leak into the game.
    void set_game_input_enabled(bool enabled);

    // Whether the left stick should also press the d-pad. True for a console
    // with no stick of its own, where reaching for the stick has to work;
    // false for one that has a stick, where doing both would hold a direction
    // down on the d-pad for as long as the stick is moved at all.
    void set_stick_is_dpad(bool is_dpad) { stick_is_dpad_ = is_dpad; }

    // What the right stick is read as. Only a console with a stick of its own
    // has more than one answer worth offering -- see RightStickMode.
    void set_right_stick_mode(RightStickMode mode) { right_stick_mode_ = mode; }
    RightStickMode right_stick_mode() const { return right_stick_mode_; }

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
    // Physically-held input, before the held-input gate is applied: the
    // buttons and the stick together, because they come off the same pads and
    // splitting them is how the circle pad came to be computed and dropped.
    DsInputState poll_held_state() const;
    // Updates the live pointer position; returns whether one is down.
    bool update_pointer(float output_width, float output_height);
    void emit_navigation_from_axes();
    void push_action(UiAction action);

    // Drops everything a touchscreen was in the middle of. See the focus-lost
    // case in handle_event().
    void forget_touches();

    struct FingerState {
        float x = 0.0f;
        float y = 0.0f;
        // Where this finger came down, so a release can say what its own press
        // was aimed at. Per finger rather than one slot for the whole app:
        // Android reports a tap twice, once as a finger and once as a mouse
        // synthesized from it, and a shared slot let the second overwrite the
        // first -- in the other one's units.
        float press_x = 0.0f;
        float press_y = 0.0f;
    };

    Settings settings_;
    bool stick_is_dpad_ = true;
    RightStickMode right_stick_mode_ = RightStickMode::CStick;
    std::map<SDL_JoystickID, SDL_GameController*> controllers_;

    DsInputState ds_state_;
    std::vector<UiAction> ui_actions_;
    std::vector<PointerEvent> pointer_events_;
    bool quit_requested_ = false;
    bool window_size_changed_ = false;
    bool game_input_enabled_ = true;

    // Keeps the press that dismissed a menu from also registering in the game.
    HeldInputGate held_input_gate_;
    // Whether the Start+Select menu chord has already been acted on. It is
    // held for as long as it takes to press two buttons, which is several
    // frames, and the menu should open once rather than flicker.
    // The stick buttons, tracked across frames because together they are the
    // menu chord and apart they are two separate shortcuts. Whether either
    // sends its own action is only known once it is released, since until then
    // the other one may still join it.
    bool left_stick_held_ = false;
    bool right_stick_held_ = false;
    bool stick_chord_used_ = false;
    bool stick_buttons_held(SDL_GameControllerButton button) const;

    // Touch tracking. The first finger down drives the DS touchscreen; a second
    // simultaneous finger opens the menu, which is the only way in on a device
    // with no buttons at all.
    std::map<SDL_FingerID, FingerState> fingers_;
    SDL_FingerID primary_finger_ = 0;
    bool has_primary_finger_ = false;
    bool mouse_down_ = false;
    float mouse_x_ = 0.0f;
    float mouse_y_ = 0.0f;
    float mouse_press_x_ = 0.0f;
    float mouse_press_y_ = 0.0f;
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
