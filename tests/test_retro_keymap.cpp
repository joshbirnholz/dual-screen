// The SDL-to-libretro keycode/character translation a core's own on-screen
// keyboard (a 3DS's swkbd, chiefly) reads from. Values are checked against
// the real RETROK_*/RETROKMOD_* constants rather than guessed at, the same
// reason the rest of this suite avoids hardcoding a protocol's numbers twice.

#include "test_support.hpp"

#include "input/retro_keymap.hpp"
#include "libretro/libretro.h"

using namespace ds;

namespace {

void test_ascii_passes_through() {
    // Both tables share the ASCII range for printable characters and the
    // handful of control codes libretro names -- that shared inheritance is
    // exactly what the direct passthrough relies on.
    CHECK_EQ(retro_keycode_for_sdl(SDLK_a), static_cast<unsigned>(RETROK_a));
    CHECK_EQ(retro_keycode_for_sdl(SDLK_z), static_cast<unsigned>(RETROK_z));
    CHECK_EQ(retro_keycode_for_sdl(SDLK_0), static_cast<unsigned>(RETROK_0));
    CHECK_EQ(retro_keycode_for_sdl(SDLK_SPACE), static_cast<unsigned>(RETROK_SPACE));
    CHECK_EQ(retro_keycode_for_sdl(SDLK_RETURN), static_cast<unsigned>(RETROK_RETURN));
    CHECK_EQ(retro_keycode_for_sdl(SDLK_BACKSPACE), static_cast<unsigned>(RETROK_BACKSPACE));
    CHECK_EQ(retro_keycode_for_sdl(SDLK_ESCAPE), static_cast<unsigned>(RETROK_ESCAPE));
    CHECK_EQ(retro_keycode_for_sdl(SDLK_TAB), static_cast<unsigned>(RETROK_TAB));
    CHECK_EQ(retro_keycode_for_sdl(SDLK_DELETE), static_cast<unsigned>(RETROK_DELETE));
}

void test_special_keys_are_translated() {
    // These live at SDL's scancode-flagged values (1<<30 | scancode), nothing
    // like RETROK's small integers, so passthrough alone would get every one
    // of them wrong.
    CHECK_EQ(retro_keycode_for_sdl(SDLK_UP), static_cast<unsigned>(RETROK_UP));
    CHECK_EQ(retro_keycode_for_sdl(SDLK_DOWN), static_cast<unsigned>(RETROK_DOWN));
    CHECK_EQ(retro_keycode_for_sdl(SDLK_LEFT), static_cast<unsigned>(RETROK_LEFT));
    CHECK_EQ(retro_keycode_for_sdl(SDLK_RIGHT), static_cast<unsigned>(RETROK_RIGHT));
    CHECK_EQ(retro_keycode_for_sdl(SDLK_F1), static_cast<unsigned>(RETROK_F1));
    CHECK_EQ(retro_keycode_for_sdl(SDLK_F12), static_cast<unsigned>(RETROK_F12));
    CHECK_EQ(retro_keycode_for_sdl(SDLK_LSHIFT), static_cast<unsigned>(RETROK_LSHIFT));
    CHECK_EQ(retro_keycode_for_sdl(SDLK_RCTRL), static_cast<unsigned>(RETROK_RCTRL));
    CHECK_EQ(retro_keycode_for_sdl(SDLK_HOME), static_cast<unsigned>(RETROK_HOME));
    CHECK_EQ(retro_keycode_for_sdl(SDLK_KP_1), static_cast<unsigned>(RETROK_KP1));
}

void test_an_unmapped_key_is_unknown() {
    // A key genuinely nothing above claims -- rather than silently aliasing
    // it to whatever value 0 happens to be reused for.
    CHECK_EQ(retro_keycode_for_sdl(SDLK_AUDIOMUTE), static_cast<unsigned>(RETROK_UNKNOWN));
}

void test_character_follows_shift_and_caps() {
    // Letters: SDL_Keycode is always lowercase regardless of shift, so the
    // actual typed case has to be worked out here.
    CHECK_EQ(retro_character_for_sdl(SDLK_a, RETROKMOD_NONE), static_cast<uint32_t>('a'));
    CHECK_EQ(retro_character_for_sdl(SDLK_a, RETROKMOD_SHIFT), static_cast<uint32_t>('A'));
    CHECK_EQ(retro_character_for_sdl(SDLK_a, RETROKMOD_CAPSLOCK), static_cast<uint32_t>('A'));
    // Shift and caps lock together cancel out, the same as a real keyboard.
    CHECK_EQ(retro_character_for_sdl(SDLK_a, RETROKMOD_SHIFT | RETROKMOD_CAPSLOCK),
             static_cast<uint32_t>('a'));

    // Digits and punctuation: unshifted types itself, shifted types the US
    // layout's symbol for that key.
    CHECK_EQ(retro_character_for_sdl(SDLK_1, RETROKMOD_NONE), static_cast<uint32_t>('1'));
    CHECK_EQ(retro_character_for_sdl(SDLK_1, RETROKMOD_SHIFT), static_cast<uint32_t>('!'));
    CHECK_EQ(retro_character_for_sdl(SDLK_MINUS, RETROKMOD_SHIFT), static_cast<uint32_t>('_'));
    // Caps lock has no bearing on digits -- only letters have a case to flip.
    CHECK_EQ(retro_character_for_sdl(SDLK_1, RETROKMOD_CAPSLOCK), static_cast<uint32_t>('1'));

    // A key with nothing to type produces no character at all.
    CHECK_EQ(retro_character_for_sdl(SDLK_UP, RETROKMOD_NONE), static_cast<uint32_t>(0));
    CHECK_EQ(retro_character_for_sdl(SDLK_F1, RETROKMOD_SHIFT), static_cast<uint32_t>(0));
}

}  // namespace

int main() {
    test_ascii_passes_through();
    test_special_keys_are_translated();
    test_an_unmapped_key_is_unknown();
    test_character_follows_shift_and_caps();
    return test::finish("retro_keymap");
}
