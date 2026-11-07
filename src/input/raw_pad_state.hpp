#pragma once

#include <SDL.h>

#include <cstdint>

namespace ds {

// One pad's controls, read raw in SDL's terms: every button SDL has a name
// for, and every axis. What InputManager::held_state_from() maps into the
// console's -- the same whether they came off SDL, off a Switch's own
// controllers, or off the touch controller drawn over the game.
struct RawPadState {
    bool buttons[SDL_CONTROLLER_BUTTON_MAX] = {};
    int16_t axes[SDL_CONTROLLER_AXIS_MAX] = {};
};

}  // namespace ds
