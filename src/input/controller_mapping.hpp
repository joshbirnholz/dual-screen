#pragma once

#include <string>
#include <vector>

#include <SDL.h>

#include "input/ds_input.hpp"

namespace ds {

// Everything in this app uses Nintendo *positional* controls, in menus and in
// game alike:
//
//        north = X
//   west = Y         east = A      <- confirms menus, and is DS A
//        south = B                 <- cancels menus, and is DS B
//
// so on an Xbox-layout pad the button marked B confirms and is DS A, and the
// button marked A cancels and is DS B. That is the same rule in both places,
// which is the point: the button that means "yes" in a menu is the button that
// means "A" in a game.
//
// To make that possible without guessing at each pad, SDL is asked to report
// face buttons by *position* rather than by printed label, so
// SDL_CONTROLLER_BUTTON_A is the south button on every controller. Nothing here
// is configurable -- remap the pad in Steam Input if you want something else.
constexpr const char* kFaceButtonsByPosition = "0";

// Everything SDL has to be told before a controller is opened. Called once at
// startup; a test asserts what it leaves behind, because one of these is not
// the kind of thing whose absence shows up anywhere but on hardware.
void apply_controller_hints();

// Which of the two lower face buttons means yes.
//
// East by default, which is what a Nintendo pad prints A on and what this app
// has always done. South is the other convention -- it is what a PlayStation
// prints the cross on, and what most people who grew up on one reach for.
//
// A position, not a letter, because the letter depends on the pad and the
// artwork depends on a separate setting: see PadGlyphStyle. The two together
// are what let a prompt say "circle" and mean it.
enum class ConfirmButton { East, South, Count };

const char* to_string(ConfirmButton button);
bool confirm_button_from_string(const std::string& text, ConfirmButton& out);

// Which SDL button confirms and which cancels, given that choice. Always the
// two lower face buttons and always different from each other.
SDL_GameControllerButton confirm_button_for(ConfirmButton choice);
SDL_GameControllerButton cancel_button_for(ConfirmButton choice);


// Menus: east confirms, south goes back.
constexpr SDL_GameControllerButton kMenuConfirmButton = SDL_CONTROLLER_BUTTON_B;  // east
constexpr SDL_GameControllerButton kMenuCancelButton = SDL_CONTROLLER_BUTTON_A;   // south
// The other two face buttons, named by where they sit rather than by what SDL
// calls them: a menu's Quit is the north button and its Reset the west one, the
// positions a Nintendo pad prints X and Y on.
constexpr SDL_GameControllerButton kMenuQuitButton = SDL_CONTROLLER_BUTTON_Y;    // north
constexpr SDL_GameControllerButton kMenuResetButton = SDL_CONTROLLER_BUTTON_X;   // west
// Settings on plus, where a Switch's own games keep their options.
constexpr SDL_GameControllerButton kMenuSettingsButton = SDL_CONTROLLER_BUTTON_START;    // plus
// Disconnect has no button: it is picked from the list, never pressed by
// accident on the way to something else.

// Maps a controller button to a DS button. Returns false for buttons the DS has
// no equivalent for.
bool ds_button_for_controller_button(SDL_GameControllerButton button, DsButton& out_button);

// Where a pad prints its letters. Used *only* to decide which letters on-screen
// prompts should name -- never to decide what a button does.
enum class PadLetterLayout {
    Xbox,      // A south, B east: Xbox, PlayStation, Steam Input's virtual pad
    Nintendo,  // A east, B south: Switch Pro, Joy-Cons
};

PadLetterLayout pad_letter_layout(SDL_GameControllerType type);

// The letters printed on the menu confirm/cancel buttons for a given pad.
struct MenuButtonLabels {
    const char* confirm = "B";
    const char* cancel = "A";
};
MenuButtonLabels menu_button_labels(PadLetterLayout layout);

// What the right stick does.
//
// A 3DS is the only console here with more than one sensible answer: it has a
// circle pad of its own, and the handful of games built for a Circle Pad Pro
// or a New 3DS read a second stick as well. Which of those a given game wants
// is the game's business rather than something that can be worked out from the
// ROM, so it is a per-game choice.
//
// A DS has no stick at all -- its left stick already stands in for the d-pad --
// so the one thing its right stick can be is a stylus, or nothing. See
// right_stick_modes() and SystemProfile::has_analog_stick.
enum class RightStickMode {
    CStick,     // a Circle Pad Pro / New 3DS C-Stick: a 3DS's default
    CirclePad,  // a second way to drive the circle pad, alongside the left stick
    DPad,       // pressed directions, the way the left stick works on a DS
    Off,        // ignored entirely, for a game that reads it as something unwanted
    // A pointer on the touch screen that the stick moves and R3 presses down,
    // for touching without a finger: a DS's default. R3 is the stylus here
    // rather than this app's smoothing shortcut. See StylusCursor.
    TouchCursor,
    // A stick drawn on the touch screen and pushed with this one: the screen
    // touched away from its centre as far as the stick is pushed, and let go
    // when it comes back -- for a game steered by dragging, as Super Mario 64
    // DS can be. Where it is and how big, ButtonMapping says.
    TouchAnalog,
};

// What clicking the left stick did on a DS, when that was a setting of its
// own: blow into the microphone, or swap screens. It is the Left Stick
// Button's row of a game's Button Mapping now -- see ButtonMapping -- and
// kept only because the wire still has room for it, for a peer from before.
enum class LeftStickPress {
    Microphone,  // held, it blows into the console's microphone
    SwapLayout,  // released, it swaps the two focus layouts
};

const char* to_string(RightStickMode mode);
// What the right stick can be on a console, in the order they are offered: a
// console with a stick of its own has everything, a DS a stylus or nothing.
std::vector<RightStickMode> right_stick_modes(bool console_has_stick);
// The one after `mode` among those, round to the first.
RightStickMode next_right_stick_mode(RightStickMode mode, bool console_has_stick);
// `mode` if the console offers it, otherwise the console's default: a 3DS's
// C-Stick, a DS's stylus.
RightStickMode right_stick_mode_for(RightStickMode mode, bool console_has_stick);
// Round-trips through settings files; unknown text falls back to the default.
const char* right_stick_mode_key(RightStickMode mode);
RightStickMode right_stick_mode_from_key(const std::string& key);

// Human-readable SDL controller type, for the diagnostic log.
const char* to_string(SDL_GameControllerType type);

}  // namespace ds
