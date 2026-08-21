#include "input/retro_keymap.hpp"

#include "libretro/libretro.h"

namespace ds {

unsigned retro_keycode_for_sdl(SDL_Keycode key) {
    // Printable ASCII lines up directly: both SDL_Keycode and RETROK_ use the
    // character's own value for these, a shared inheritance from SDL 1.2's
    // own keysym table.
    if (key > 0 && key < 128) {
        return static_cast<unsigned>(key);
    }
    switch (key) {
        case SDLK_CAPSLOCK: return RETROK_CAPSLOCK;
        case SDLK_F1: return RETROK_F1;
        case SDLK_F2: return RETROK_F2;
        case SDLK_F3: return RETROK_F3;
        case SDLK_F4: return RETROK_F4;
        case SDLK_F5: return RETROK_F5;
        case SDLK_F6: return RETROK_F6;
        case SDLK_F7: return RETROK_F7;
        case SDLK_F8: return RETROK_F8;
        case SDLK_F9: return RETROK_F9;
        case SDLK_F10: return RETROK_F10;
        case SDLK_F11: return RETROK_F11;
        case SDLK_F12: return RETROK_F12;
        case SDLK_F13: return RETROK_F13;
        case SDLK_F14: return RETROK_F14;
        case SDLK_F15: return RETROK_F15;
        case SDLK_PRINTSCREEN: return RETROK_PRINT;
        case SDLK_SCROLLLOCK: return RETROK_SCROLLOCK;
        case SDLK_PAUSE: return RETROK_PAUSE;
        case SDLK_INSERT: return RETROK_INSERT;
        case SDLK_HOME: return RETROK_HOME;
        case SDLK_PAGEUP: return RETROK_PAGEUP;
        // Delete is 127 in both tables, so it is already handled by the ASCII
        // passthrough above.
        case SDLK_END: return RETROK_END;
        case SDLK_PAGEDOWN: return RETROK_PAGEDOWN;
        case SDLK_RIGHT: return RETROK_RIGHT;
        case SDLK_LEFT: return RETROK_LEFT;
        case SDLK_DOWN: return RETROK_DOWN;
        case SDLK_UP: return RETROK_UP;
        case SDLK_NUMLOCKCLEAR: return RETROK_NUMLOCK;
        case SDLK_KP_DIVIDE: return RETROK_KP_DIVIDE;
        case SDLK_KP_MULTIPLY: return RETROK_KP_MULTIPLY;
        case SDLK_KP_MINUS: return RETROK_KP_MINUS;
        case SDLK_KP_PLUS: return RETROK_KP_PLUS;
        case SDLK_KP_ENTER: return RETROK_KP_ENTER;
        case SDLK_KP_1: return RETROK_KP1;
        case SDLK_KP_2: return RETROK_KP2;
        case SDLK_KP_3: return RETROK_KP3;
        case SDLK_KP_4: return RETROK_KP4;
        case SDLK_KP_5: return RETROK_KP5;
        case SDLK_KP_6: return RETROK_KP6;
        case SDLK_KP_7: return RETROK_KP7;
        case SDLK_KP_8: return RETROK_KP8;
        case SDLK_KP_9: return RETROK_KP9;
        case SDLK_KP_0: return RETROK_KP0;
        case SDLK_KP_PERIOD: return RETROK_KP_PERIOD;
        case SDLK_KP_EQUALS: return RETROK_KP_EQUALS;
        case SDLK_LCTRL: return RETROK_LCTRL;
        case SDLK_LSHIFT: return RETROK_LSHIFT;
        case SDLK_LALT: return RETROK_LALT;
        case SDLK_LGUI: return RETROK_LMETA;
        case SDLK_RCTRL: return RETROK_RCTRL;
        case SDLK_RSHIFT: return RETROK_RSHIFT;
        case SDLK_RALT: return RETROK_RALT;
        case SDLK_RGUI: return RETROK_RMETA;
        case SDLK_MODE: return RETROK_MODE;
        case SDLK_HELP: return RETROK_HELP;
        case SDLK_SYSREQ: return RETROK_SYSREQ;
        case SDLK_MENU: return RETROK_MENU;
        case SDLK_POWER: return RETROK_POWER;
        case SDLK_UNDO: return RETROK_UNDO;
        default: return RETROK_UNKNOWN;
    }
}

namespace {

// US layout's shifted symbol for each unshifted digit-row / punctuation key.
// Everything not listed here already types the same character shifted or
// not (letters are handled separately; unlisted keys have no shifted form).
uint32_t shifted_symbol(uint32_t unshifted) {
    switch (unshifted) {
        case '0': return ')';
        case '1': return '!';
        case '2': return '@';
        case '3': return '#';
        case '4': return '$';
        case '5': return '%';
        case '6': return '^';
        case '7': return '&';
        case '8': return '*';
        case '9': return '(';
        case '-': return '_';
        case '=': return '+';
        case '[': return '{';
        case ']': return '}';
        case '\\': return '|';
        case ';': return ':';
        case '\'': return '"';
        case ',': return '<';
        case '.': return '>';
        case '/': return '?';
        case '`': return '~';
        default: return unshifted;
    }
}

}  // namespace

uint32_t retro_character_for_sdl(SDL_Keycode key, uint16_t modifiers) {
    if (key < 32 || key > 126) {
        return 0;  // no text of its own -- an arrow, a function key, and so on
    }
    if (key >= 'a' && key <= 'z') {
        const bool shift = (modifiers & RETROKMOD_SHIFT) != 0;
        const bool caps = (modifiers & RETROKMOD_CAPSLOCK) != 0;
        return (shift != caps) ? static_cast<uint32_t>(key - 'a' + 'A') : static_cast<uint32_t>(key);
    }
    if (modifiers & RETROKMOD_SHIFT) {
        return shifted_symbol(static_cast<uint32_t>(key));
    }
    return static_cast<uint32_t>(key);
}

uint16_t retro_key_modifiers() {
    const SDL_Keymod mod = SDL_GetModState();
    uint16_t result = RETROKMOD_NONE;
    if (mod & KMOD_SHIFT) result |= RETROKMOD_SHIFT;
    if (mod & KMOD_CTRL) result |= RETROKMOD_CTRL;
    if (mod & KMOD_ALT) result |= RETROKMOD_ALT;
    if (mod & KMOD_GUI) result |= RETROKMOD_META;
    if (mod & KMOD_NUM) result |= RETROKMOD_NUMLOCK;
    if (mod & KMOD_CAPS) result |= RETROKMOD_CAPSLOCK;
    return result;
}

}  // namespace ds
