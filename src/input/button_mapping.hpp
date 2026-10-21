#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "core/system.hpp"
#include "input/controller_mapping.hpp"
#include "input/ds_input.hpp"

namespace ds {

// The controls on a player's pad that Button Mapping can give another job.
// Named for where they sit on a Nintendo pad -- A is the east face button --
// the same positions the rest of this app reads them by (see
// kFaceButtonsByPosition). Sent over the wire by number, so new ones go at
// the end; kPadControlRows is the order the page lists them in.
//
// The sticks are not among them: the left stick moves as it does, and what
// the right stick does is Right Stick's. Their presses are, since those are
// buttons like any other, and so is each of the d-pad's directions.
enum class PadControl : uint8_t {
    A, B, X, Y,
    L, R, Zl, Zr,
    Plus, Minus,
    LeftStick, RightStick,
    Up, Down, Left, Right,
    Count,
};
inline constexpr std::size_t kPadControls = static_cast<std::size_t>(PadControl::Count);
// The page's rows, top to bottom: the face buttons, the d-pad, the
// shoulders, Plus and Minus, the stick presses.
inline constexpr std::array<PadControl, kPadControls> kPadControlRows = {
    PadControl::A,  PadControl::B,     PadControl::X,         PadControl::Y,
    PadControl::Up, PadControl::Down,  PadControl::Left,      PadControl::Right,
    PadControl::L,  PadControl::R,     PadControl::Zl,        PadControl::Zr,
    PadControl::Plus, PadControl::Minus, PadControl::LeftStick, PadControl::RightStick,
};

// What a control does. Saved by key and sent over the wire by number, so the
// numbers are part of the protocol: new ones go at the end.
enum class PadAction : uint8_t {
    // Whatever the console's standard mapping gives this control -- see
    // standard_pad_action(). What a control is until it is changed, and
    // what Revert to Default puts back.
    Standard = 0,
    // The console's own buttons.
    A, B, X, Y,
    L, R, Zl, Zr,
    Start, Select,
    Up, Down, Left, Right,
    // A stick's press as the console's own: a Wii U's, which the game reads.
    LeftStickButton, RightStickButton,
    // The Touch Cursor's tip -- see RightStickMode::TouchCursor. Touches
    // nothing while the right stick is something else.
    TouchCursor,
    // This app's own.
    QuickMenu,
    SwapScreens,
    // No longer chosen: Screen Smoothing is the quick menu's. A game that
    // kept it has it read as Standard -- see pad_action_from_key().
    ScreenSmoothing,
    // A DS's microphone, held.
    BlowIntoMic,
    Nothing,
    // The next of the layouts the quick menu offers, round to the first.
    NextLayout,
    // Another of the console's buttons pressed over and over while this one
    // is held -- which, ButtonMapping::turbo_of says.
    Turbo,
    // The touch screen touched at one spot while held -- where,
    // ButtonMapping::touch_points says.
    TouchPoint,
    Count,
};

// What the console being played makes of a pad: which of its buttons it has,
// and which of this app's shortcuts its standard mapping keeps.
struct PadConsole {
    // A 3DS's ZL and ZR, which a New 3DS has and a Circle Pad Pro adds.
    bool has_zl_zr = false;
    // A DS's microphone, which L3 blows into as standard.
    bool has_microphone = false;
    // Every control goes to the game -- see SystemProfile::game_uses_every_control.
    bool game_uses_every_control = false;
    // R3 is the Touch Cursor's tip while the right stick is the cursor.
    RightStickMode right_stick = RightStickMode::CStick;
};

// What `system` makes of a pad, with its right stick doing `right_stick`.
PadConsole pad_console_for(SystemId system, RightStickMode right_stick);

// The console's standard mapping, which a game has until it is changed: the
// buttons where a Nintendo pad has them, the triggers as L and R, Plus and
// Minus as Start and Select, and the stick presses this app's shortcuts --
// on a console with nowhere else to put them.
PadAction standard_pad_action(PadControl control, const PadConsole& console);

// A spot on the touch screen, as a fraction of it across and down: the same
// spot whatever size the console's screen is.
struct TouchSpot {
    float u = 0.5f;
    float v = 0.5f;
    bool operator==(const TouchSpot& other) const { return u == other.u && v == other.v; }
};

// A game's Button Mapping: what each control does, Standard for those never
// changed.
struct ButtonMapping {
    std::array<PadAction, kPadControls> actions{};
    // For a control that is Turbo, the console button it presses, or
    // TouchPoint for its spot touched over and over; Standard while none has
    // been chosen, which presses nothing.
    std::array<PadAction, kPadControls> turbo_of{};
    // For a control that is Touch Point, where it touches. Kept when the
    // control is given another job, to be where it was if it is put back.
    std::array<TouchSpot, kPadControls> touch_points{};

    PadAction& operator[](PadControl control) { return actions[static_cast<std::size_t>(control)]; }
    PadAction operator[](PadControl control) const { return actions[static_cast<std::size_t>(control)]; }
    PadAction& turbo(PadControl control) { return turbo_of[static_cast<std::size_t>(control)]; }
    PadAction turbo(PadControl control) const { return turbo_of[static_cast<std::size_t>(control)]; }
    TouchSpot& spot(PadControl control) { return touch_points[static_cast<std::size_t>(control)]; }
    const TouchSpot& spot(PadControl control) const { return touch_points[static_cast<std::size_t>(control)]; }
    // What `control` does, Standard looked up.
    PadAction resolved(PadControl control, const PadConsole& console) const;
    // Nothing changed from the standard mapping -- what a game has until it
    // is, and after Revert to Default.
    bool is_standard(const PadConsole& console) const;

    bool operator==(const ButtonMapping& other) const {
        return actions == other.actions && turbo_of == other.turbo_of && touch_points == other.touch_points;
    }
    bool operator!=(const ButtonMapping& other) const { return !(*this == other); }
};

// What Button Mapping's picker offers, in its order: the console's buttons,
// then the touch screen's and this app's, then Nothing -- one list. Only what the console has -- a DS has no ZL
// and no Touch Cursor tip without a Touch Cursor.
std::vector<PadAction> pad_action_choices(const PadConsole& console);
// What Turbo can press: the console's own buttons.
std::vector<PadAction> turbo_choices(const PadConsole& console);
// Whether Turbo's press is down at `ms` milliseconds: down and up 15 times a
// second, a little slower than a game reads its buttons, so none is missed.
bool turbo_down_at(uint32_t ms);

// What kind of thing a choice is: one of the console's buttons, a touch, or
// this app's own.
enum class PadActionGroup { Console, TouchScreen, Dualscreen };
PadActionGroup pad_action_group(PadAction action);

// The console button `action` presses, if it is one. Blowing into the
// microphone is L3 and the Touch Cursor's tip R3, which is how the cores and
// the Touch Cursor read them.
bool ds_button_for_pad_action(PadAction action, DsButton& out);
// Whether `action` is one of this app's own, raised once per press rather
// than held -- see InputManager::end_frame().
bool pad_action_is_shortcut(PadAction action);

// Names, for Button Mapping: the control as the pad prints it, and the action
// as the picker lists it.
const char* pad_control_name(PadControl control);
const char* pad_action_name(PadAction action);
// Round-trips through a game's config file; unknown text is Standard.
const char* pad_action_key(PadAction action);
PadAction pad_action_from_key(const std::string& key);
const char* pad_control_key(PadControl control);
bool pad_control_from_key(const std::string& key, PadControl& out);

}  // namespace ds
