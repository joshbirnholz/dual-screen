#pragma once

#include <cstdint>
#include <string>

namespace ds {

// What is on screen, as far as which sound a press earns goes. Each screen
// describes itself (see their describe_for_sound()); the app adds where it is.
struct UiSoundView {
    int screen = 0;          // which screen is up -- the app's state
    uint64_t focus = 0;      // where the cursor is on it
    uint64_t rest = 0;       // everything else a press can change: a page, a
                             // dialog, a value
    bool in_game = false;    // a game on screen, no menu over it
    bool menu = false;       // the quick menu, or a page opened from it, over a game
    bool error = false;      // an error being shown
    bool on_cancel = false;  // the cursor on a popup's Cancel, which A backs
                             // out with as B does -- and sounds like it
    uint32_t saves = 0;      // save states made so far, each one chimed
    int toggle = -1;         // the switch the cursor is on: 1 on, 0 off, -1
                             // not on one -- a press flipping it sounds
                             // which way it went
    bool ring = false;       // the cursor drawn as a rounded ring -- not the
                             // quick menu's sections -- so a direction that
                             // moves nothing bumps it
};

// Hashes a screen's state into a view's focus or rest.
inline void mix_sound_state(uint64_t& into, uint64_t value) {
    into ^= value + 0x9E3779B97F4A7C15ull + (into << 6) + (into >> 2);
}
inline void mix_sound_state(uint64_t& into, const std::string& value) {
    uint64_t hash = 1469598103934665603ull;
    for (const char c : value) hash = (hash ^ static_cast<unsigned char>(c)) * 1099511628211ull;
    mix_sound_state(into, hash);
}

}  // namespace ds
