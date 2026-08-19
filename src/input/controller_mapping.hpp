#pragma once

#include <SDL.h>

#include <string>

#include "input/ds_input.hpp"

namespace ds {

// Face buttons involve three separate decisions, and collapsing them is how
// this went wrong once already:
//
//   1. How SDL reports them. Pinned to "by printed label" (see kUseButtonLabels
//      below), so SDL_CONTROLLER_BUTTON_A is always the button marked "A",
//      whatever pad it is.
//   2. What confirms in a menu. Always the button marked "A", which is what
//      every platform's own UI does.
//   3. What the game does with them. A DS's buttons sit in Nintendo positions,
//      so the game always matches those positions -- on an Xbox-layout pad that
//      means the button marked A is DS B, because that is where B sits on a DS.
//
// None of it is configurable: anyone who wants something else can remap the pad
// in Steam Input, which is a better place for it than a second set of bindings
// here that has to be kept consistent with the first.

// The value SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS is pinned to. SDL's own
// default is already "1", but leaving it implicit is what made the mapping
// depend on the SDL version.
constexpr const char* kUseButtonLabels = "1";

// Where the pad prints its letters. SDL hands us letters; this says where those
// letters physically sit.
enum class PadLetterLayout {
    // A south, B east, X west, Y north. Xbox, PlayStation, Steam Input's
    // virtual pad, and almost everything else.
    Xbox,
    // A east, B south, X north, Y west. Switch Pro, Joy-Cons -- the same
    // arrangement a DS uses.
    Nintendo,
};

PadLetterLayout pad_letter_layout(SDL_GameControllerType type);

// Where a face button physically sits on the pad.
enum class FacePosition { South, East, West, North };

// Resolves a reported (labelled) button to its physical position. Returns false
// for anything that is not a face button.
bool face_position_for_button(SDL_GameControllerButton button,
                              PadLetterLayout layout,
                              FacePosition& out_position);

// Maps a reported controller button to a DS button, placing face buttons where
// a DS puts them: south is B, east is A, west is Y, north is X. `layout` says
// where this pad prints its letters, which is what makes that correct on an
// Xbox-layout pad and on a Switch pad alike. Returns false for buttons the DS
// has no equivalent for.
bool ds_button_for_controller_button(SDL_GameControllerButton button,
                                     PadLetterLayout layout,
                                     DsButton& out_button);

// Menus always confirm on the button marked A and go back on the one marked B.
constexpr SDL_GameControllerButton kMenuConfirmButton = SDL_CONTROLLER_BUTTON_A;
constexpr SDL_GameControllerButton kMenuCancelButton = SDL_CONTROLLER_BUTTON_B;

// Human-readable SDL controller type, for the diagnostic log.
const char* to_string(SDL_GameControllerType type);

}  // namespace ds
