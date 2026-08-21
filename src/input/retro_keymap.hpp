#pragma once

#include <cstdint>

#include <SDL.h>

namespace ds {

// Translates a physical key into the constants a libretro core's software
// keyboard expects (RETROK_*/RETROKMOD_* from libretro.h), so a core's own
// on-screen keyboard -- a 3DS's swkbd, chiefly -- receives what was actually
// typed rather than getting nothing and falling back to a default the
// moment it opens. Returned as plain integers rather than the enum so a
// caller does not need libretro.h just to forward a key.
unsigned retro_keycode_for_sdl(SDL_Keycode key);

// The held modifier keys, in the same bitmask a core's keyboard callback
// expects. Read fresh from SDL's own state rather than tracked separately,
// so it can never drift from what is actually held.
uint16_t retro_key_modifiers();

// What typing this key actually produces, as a Unicode codepoint -- 0 for a
// key that types nothing on its own (an arrow, a function key, a modifier).
// SDL_Keycode names the physical key rather than what it types, so shift and
// caps lock are applied here against a US layout: the common case for a
// keyboard actually plugged into a Deck or a PC, and everything a 3DS's own
// swkbd realistically needs -- names, search terms, friend codes. A
// different physical layout still navigates and deletes correctly, since
// those never depended on this; only which shifted symbol a digit key types
// would be off.
uint32_t retro_character_for_sdl(SDL_Keycode key, uint16_t modifiers);

}  // namespace ds
