#pragma once

#include <SDL.h>

#include <string>

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

// Menus: east confirms, south goes back.
constexpr SDL_GameControllerButton kMenuConfirmButton = SDL_CONTROLLER_BUTTON_B;  // east
constexpr SDL_GameControllerButton kMenuCancelButton = SDL_CONTROLLER_BUTTON_A;   // south
// The other two face buttons, named by where they sit rather than by what SDL
// calls them: a menu's Quit is the north button and its Reset the west one, the
// positions a Nintendo pad prints X and Y on.
constexpr SDL_GameControllerButton kMenuQuitButton = SDL_CONTROLLER_BUTTON_Y;    // north
constexpr SDL_GameControllerButton kMenuResetButton = SDL_CONTROLLER_BUTTON_X;   // west
constexpr SDL_GameControllerButton kMenuDisconnectButton = SDL_CONTROLLER_BUTTON_START;  // plus

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
// A DS has no stick at all -- its left stick already stands in for the d-pad,
// and the right one has nothing to be -- so the choice is not offered there.
// See SystemProfile::has_analog_stick.
enum class RightStickMode {
    CStick,     // a Circle Pad Pro / New 3DS C-Stick: the default
    CirclePad,  // a second way to drive the circle pad, alongside the left stick
    DPad,       // pressed directions, the way the left stick works on a DS
    Off,        // ignored entirely, for a game that reads it as something unwanted
};

const char* to_string(RightStickMode mode);
RightStickMode next_right_stick_mode(RightStickMode mode);
// Round-trips through settings files; unknown text falls back to the default.
const char* right_stick_mode_key(RightStickMode mode);
RightStickMode right_stick_mode_from_key(const std::string& key);

// Human-readable SDL controller type, for the diagnostic log.
const char* to_string(SDL_GameControllerType type);

}  // namespace ds
