#pragma once

#include <SDL.h>

#include <map>
#include <string>
#include <vector>

#include "config/settings.hpp"
#include "input/controller_mapping.hpp"
#include "input/ds_input.hpp"
#include "input/motion.hpp"
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
    // Opens the settings screen, from the button a Nintendo pad prints - on.
    // Menu-only for the same reason again -- and it is the settings of the
    // device running the game, wherever the press came from.
    MenuSettings,
    // Escape, while a game is being played: opens the quick menu, the same as
    // F1 or Tab. SteamOS has a system shortcut that sends Escape, which makes
    // it the key easiest to reach from a pad with no keyboard in sight.
    //
    // Its own action rather than a second ToggleMenu, because everywhere else
    // Escape already means back -- it raises Cancel alongside this -- and a
    // menu that took it as "toggle" would close itself when asked to go back
    // one step. Only the screens that play a game listen for this one.
    OpenMenu,
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
// Whether SDL's name for a controller says it is a television remote rather
// than a pad.
//
// Measured on an Apple TV rather than reasoned about, because reasoning about
// it was wrong once already. The first version of this asked
// SDL_GameControllerHasButton for an X button, on the argument that every real
// gamepad has one and the Siri Remote has three buttons and a touch surface.
// The hardware disagreed:
//
//     controller connected: Remote (SDL type: unknown, a pad)
//
// SDL answers yes for X, so the capability was no capability at all and every
// Apple TV got pad glyphs. The name is what actually distinguishes it: Apple
// reports the Siri Remote's vendor name as "Remote", and SDL passes that
// through.
//
// Matched loosely, and only ever consulted on a television. A pad called
// something with "remote" in it would be taken for a remote, which on an Apple
// TV costs two prompts; a remote not called "Remote" would be taken for a pad,
// which is the state this was in before and is no worse. The joystick's own
// button and axis counts are in the connection log, for the day one of those
// is a better answer than this.
bool controller_is_a_remote(const char* sdl_name);

// One controller's own state, apart from every other's -- for a console whose
// controllers each belong to one player. See input/pad_routing.hpp.
//
// Known by its id, never by its name -- see KnownPad for why. No keyboard:
// on a Wii U the keys are Cemu's to map directly, and none of this app's
// controllers carry them.
// One pad's controls, read raw in SDL's terms: every button SDL has a name
// for, and every axis. What held_state_from() maps into the console's -- the
// same whether they came off SDL or, on a Switch, off the console itself.
struct RawPadState {
    bool buttons[SDL_CONTROLLER_BUTTON_MAX] = {};
    int16_t axes[SDL_CONTROLLER_AXIS_MAX] = {};
};

struct PadReading {
    SDL_JoystickID id = 0;
    // What SDL calls it -- under Steam Input, the name the player gave it in
    // Steam. For showing only; see KnownPad::name.
    std::string name;
    // Its player slot on this device, from 0, or -1 for none -- see
    // KnownPad::slot.
    int slot = -1;
    // Its presses through the same mapping the merged state uses, and how it
    // is being moved when its own sensors can say (see MotionReader, which
    // switches them on).
    DsInputState held;
};

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
    // Hands every control the console has back to the game: the stick clicks
    // singly and together, and the second finger that would otherwise open the
    // menu. What is left is the keyboard, which no console has and which a
    // player can point Steam Input at. See SystemProfile::game_uses_every_control.
    void set_game_owns_controls(bool owns) { game_owns_controls_ = owns; }
    bool game_owns_controls() const { return game_owns_controls_; }

    // Which face button means yes. See ConfirmButton; changing it swaps the
    // pair, never leaves both meaning the same thing.
    void set_confirm_button(ConfirmButton choice) { confirm_button_ = choice; }
    ConfirmButton confirm_button() const { return confirm_button_; }

    void set_stick_is_dpad(bool is_dpad) { stick_is_dpad_ = is_dpad; }
    // Whether L3 is the console's microphone rather than one of this app's own
    // shortcuts. See SystemProfile::has_microphone, and left_stick_is_mic_ for
    // why this button and not another.
    void set_microphone_button(bool has_microphone) { left_stick_is_mic_ = has_microphone; }

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

    int connected_controllers() const;

    // How many of those are a pad rather than a television remote. SDL reports
    // an Apple TV's Siri Remote as a controller, so connected_controllers()
    // counts it and cannot tell the difference. See prompt_style_for().
    int connected_gamepads() const;

    // Whether the controller being played with is one Joy-Con held sideways
    // -- the last one pressed, or before any press, every one there is. The
    // Switch's own menus show its face buttons by position rather than by
    // letter then, since the letters are printed for holding it upright.
    // False anywhere but a Switch.
    bool single_joycon_in_use() const;


    // Rumbles every connected controller, or stops them -- for a companion
    // standing in for a Wii U GamePad, whose one motor this collapses onto
    // whatever this device actually has. On/off rather than a duration: the
    // far end says when it starts and when it stops, same as it would if
    // this were the GamePad's motor itself, so this only ever needs to be
    // told which of those just happened. Harmless to call with nothing
    // connected, or on a platform where SDL cannot rumble at all -- both
    // just do nothing.
    void set_rumble(bool active);

    // Each connected pad on its own, in order of the pads' player numbers --
    // which under Steam Input is Steam's slot order. Nothing held while game
    // input is switched off, the same as ds_state(): a menu is up.
    std::vector<PadReading> pad_readings() const;

    // Rumbles one pad, or stops it: a player's motor, routed to whichever pad
    // is that player. Kept going until told to stop -- see set_rumble() for
    // why SDL has to be reminded -- and forgotten if the pad goes away.
    void set_pad_rumble(SDL_JoystickID id, bool active);

    // One short buzz of `ms` on one pad -- or on every pad, for an id of -1
    // -- that ends by itself: the Controllers screen asking which pad is
    // which. Never renewed, and left alone on a pad whose motor a game
    // already has on.
    void ping_pad(SDL_JoystickID id, uint32_t ms);

    // Turns reading this device's own motion on or off -- see MotionReader,
    // and SessionStateMessage::motion_wanted for who decides. Off until
    // something asks, because a gyroscope left running costs battery on
    // exactly the handhelds this is for.
    void set_motion_enabled(bool enabled) { motion_.set_enabled(enabled); }
    // How this device is being held right now, as of the last poll. Absent
    // when motion is switched off, when nothing here has sensors, or before
    // the first reading has arrived.
    const DsMotionState& motion() const { return motion_.state(); }

    // Letters printed on the menu confirm/cancel buttons for the connected pad,
    // for on-screen prompts. Display only -- it never changes what a button does.
    // Whether `direction` (Up, Down, Left or Right) is being held long
    // enough to repeat: the press itself is false, the steps after it true.
    bool direction_repeating(UiAction direction) const;

    MenuButtonLabels menu_labels() const;

private:
    void open_controller(int device_index);
    void close_controller(SDL_JoystickID instance_id);
    // Physically-held input, before the held-input gate is applied: the
    // buttons and the stick together, because they come off the same pads and
    // splitting them is how the circle pad came to be computed and dropped.
    DsInputState poll_held_state() const;
    // One pad's share of that, through the same mapping.
    DsInputState held_state_of(SDL_GameController* controller) const;
    // The same mapping, from controls already read.
    DsInputState held_state_from(const RawPadState& raw) const;
    static RawPadState raw_state_of(SDL_GameController* controller);
    // One pad's reading with whatever the held-input gate is holding back
    // taken out -- see HeldInputGate.
    DsInputState gated(DsInputState held) const;
    // The keyboard's share.
    DsInputState keyboard_held_state() const;
    // Whether a pad SDL lists is actually there. Always, except on a Switch,
    // whose SDL lists all eight of the console's controller slots from the
    // start, filled or not -- see pad_connected() in the .cpp.
    bool pad_connected(SDL_JoystickID id) const;
    // Every pad actually plugged in, with the SDL pad that answers for it --
    // on a Switch, the handheld controls too, which SDL has none of its own
    // for. See kSwitchHandheldId in the .cpp.
    std::vector<std::pair<SDL_JoystickID, SDL_GameController*>> connected_pads() const;
    SDL_GameController* pad_controller(SDL_JoystickID id) const;
    // Runs one pad's motor for `ms`, or stops it. SDL_GameControllerRumble
    // everywhere but a Switch, where SDL cannot be trusted with it -- see
    // the .cpp -- and the console's own vibration calls stand in, with the
    // length kept here.
    void drive_motor(SDL_JoystickID id, SDL_GameController* controller, bool on, uint32_t ms);
    // Renews the rumble on every pad set_pad_rumble() left on, and on a
    // Switch stops any motor whose time is up.
    void refresh_pad_rumble();
    std::map<SDL_JoystickID, bool> pad_rumble_on_;
    uint32_t pad_rumble_refreshed_ms_ = 0;
    // When each motor driven on a Switch is due to stop; 0 for stopped.
    std::map<SDL_JoystickID, uint32_t> motor_until_;
    // Pads already said to be unable to rumble, so it is said once each.
    std::map<SDL_JoystickID, bool> said_no_rumble_;
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
    bool game_owns_controls_ = false;
    ConfirmButton confirm_button_ = ConfirmButton::East;
    // L3 reaches the game as the microphone instead of swapping layouts here.
    //
    // A DS has almost no spare buttons, and by the time this app has taken the
    // stick presses for itself there are none at all -- so a microphone that
    // needs holding has to take one back. L3 is the one to take: swapping
    // layouts is also in the quick menu, a press away, while blowing has no
    // other way in at all. A shortcut with an alternative loses to a
    // capability without one.
    bool left_stick_is_mic_ = false;
    RightStickMode right_stick_mode_ = RightStickMode::CStick;
    std::map<SDL_JoystickID, SDL_GameController*> controllers_;
    // See single_joycon_in_use(). -1 until anything is pressed.
    SDL_JoystickID last_pad_pressed_ = -1;
    MotionReader motion_;

    DsInputState ds_state_;
    std::vector<UiAction> ui_actions_;
    std::vector<PointerEvent> pointer_events_;
    // Which buttons have already been named in the log, one bit each, and
    // whether menu navigation by stick has been. Once is a mapping; every
    // press is noise. See SDL_CONTROLLERBUTTONDOWN.
    uint32_t said_buttons_ = 0;
    bool said_axis_navigation_ = false;
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
    // A two-finger tap in progress: two fingers down, neither having moved
    // far, neither having been down long. The way into the menu on a device
    // with no buttons. See the SDL_FINGER cases in input_manager.cpp.
    bool two_finger_tap_live_ = false;
    uint32_t two_finger_tap_began_ms_ = 0;

    // Where the active finger or mouse press currently is, in output pixels.
    bool pointer_active_ = false;
    float pointer_x_ = 0.0f;
    float pointer_y_ = 0.0f;

    // Stick-as-dpad edge detection for menu navigation.
    bool axis_up_ = false;
    bool axis_down_ = false;
    bool axis_left_ = false;
    bool axis_right_ = false;
    // Holding a direction keeps moving: after kRepeatDelayMs, once every
    // kRepeatEveryMs, from the stick, the d-pad or the arrow keys alike. The
    // first step is the press itself, raised where the press arrives; these
    // are the ones after it. Up, down, left, right.
    uint32_t direction_next_repeat_[4] = {};
    bool direction_held_[4] = {};
    // Whether a held direction has started repeating: the steps after the
    // first, which a list that goes round stops at its end rather than
    // going round on -- see direction_repeating().
    bool direction_repeated_[4] = {};
    void repeat_held_directions(const bool held[4]);
};

}  // namespace ds
