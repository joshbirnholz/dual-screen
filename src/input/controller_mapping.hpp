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
//   3. What the game does with them. Independent of both: a DS's buttons sit in
//      Nintendo positions, and matching that is a different question from
//      matching the letters printed on the pad.
//
// Only (3) is a matter of taste, so only (3) is configurable.

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

// What the *game* does with the face buttons.
enum class GameFaceMapping {
    // Match a DS physically: south is B, east is A, west is Y, north is X.
    // On an Xbox-layout pad this swaps A/B and X/Y relative to the printed
    // letters; on a Nintendo-layout pad the letters already agree.
    DsLayout,
    // Match the printed letters instead: the button marked A is DS A on every
    // pad, so on-screen prompts always name the button under your thumb.
    MatchLabels,
};

const char* to_string(GameFaceMapping mapping);
GameFaceMapping game_face_mapping_from_string(const std::string& text, GameFaceMapping fallback);

// Maps a reported controller button to a DS button. `layout` only matters for
// face buttons under GameFaceMapping::DsLayout. Returns false for buttons the
// DS has no equivalent for.
bool ds_button_for_controller_button(SDL_GameControllerButton button,
                                     GameFaceMapping mapping,
                                     PadLetterLayout layout,
                                     DsButton& out_button);

// Menus always confirm on the button marked A and go back on the one marked B.
constexpr SDL_GameControllerButton kMenuConfirmButton = SDL_CONTROLLER_BUTTON_A;
constexpr SDL_GameControllerButton kMenuCancelButton = SDL_CONTROLLER_BUTTON_B;

// Human-readable SDL controller type, for the diagnostic log.
const char* to_string(SDL_GameControllerType type);

}  // namespace ds
