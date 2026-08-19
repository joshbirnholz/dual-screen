#pragma once

#include <SDL.h>

#include <string>

#include "input/ds_input.hpp"

namespace ds {

// How SDL is reporting the pad's face buttons, and therefore how they map onto
// the DS.
//
// This is not a detail we get to ignore. SDL's own default is to report face
// buttons by their *printed label*, so on a Nintendo-layout pad
// SDL_CONTROLLER_BUTTON_A is the east button, while on an Xbox-layout pad it is
// the south one. Code that assumes either one silently mis-maps half the pads
// in the world, so the choice is made explicitly here and pushed into SDL
// through SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS.
enum class FaceButtonConvention {
    // SDL reports by label: the button printed "A" is SDL_CONTROLLER_BUTTON_A
    // on every pad. DS A/B/X/Y then match the letters the player is looking at,
    // which is also what a DS game's own on-screen prompts say.
    Labels,
    // SDL reports by position, Xbox-style (A = south). The DS's physical layout
    // is applied on top: south is B, east is A. This is what someone used to
    // holding a real DS will expect from an Xbox-layout pad.
    Positions,
};

const char* to_string(FaceButtonConvention convention);
FaceButtonConvention face_button_convention_from_string(const std::string& text,
                                                        FaceButtonConvention fallback);

// The value SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS must carry for this
// convention. Must be set before controllers are opened.
const char* sdl_button_labels_hint(FaceButtonConvention convention);

// Maps an SDL controller button to a DS button. Returns false for buttons the
// DS has no equivalent for.
bool ds_button_for_controller_button(SDL_GameControllerButton button,
                                     FaceButtonConvention convention,
                                     DsButton& out_button);

// Which SDL button confirms and cancels in menus. Chosen so it is always the
// one the player would call "A" and "B" on their own pad.
SDL_GameControllerButton confirm_button(FaceButtonConvention convention);
SDL_GameControllerButton cancel_button(FaceButtonConvention convention);

// Letters to print in on-screen hints for those two buttons.
const char* confirm_button_label(FaceButtonConvention convention);
const char* cancel_button_label(FaceButtonConvention convention);

// Human-readable SDL controller type, for the diagnostic log.
const char* to_string(SDL_GameControllerType type);

}  // namespace ds
