#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "input/input_manager.hpp"
#include "ui/sound_view.hpp"

namespace ds {

// The menus' own sounds: a cursor moving, something chosen, something backed
// out of, something refused, the quick menu opening over a game -- its
// closing is backing out, and sounds like it -- a save state made, and a
// switch turned on or off, and the cursor bumping against the end of where it
// can go. Made by assets/synth_ui_sounds.py, made ready by
// assets/make_ui_sounds.py and carried inside the binary.
enum class UiSound { MoveCursor, Confirm, Cancel, Error, OpenQuickMenu, SaveState, ToggleOn, ToggleOff, Bump };
inline constexpr int kUiSoundCount = 9;
const char* ui_sound_name(UiSound sound);
// As net::MenuSound numbers them on the wire: one more than here.
inline uint8_t ui_sound_to_wire(UiSound sound) { return static_cast<uint8_t>(static_cast<int>(sound) + 1); }
inline std::optional<UiSound> ui_sound_from_wire(uint8_t value) {
    if (value < 1 || value > kUiSoundCount) return std::nullopt;
    return static_cast<UiSound>(value - 1);
}
// A Bump's way, as net::MenuSoundMessage carries it: 1 up, 2 down, 3 left,
// 4 right.
inline uint8_t bump_direction_to_wire(UiAction direction) {
    switch (direction) {
        case UiAction::Up: return 1;
        case UiAction::Down: return 2;
        case UiAction::Left: return 3;
        case UiAction::Right: return 4;
        default: return 0;
    }
}
inline std::optional<UiAction> bump_direction_from_wire(uint8_t value) {
    switch (value) {
        case 1: return UiAction::Up;
        case 2: return UiAction::Down;
        case 3: return UiAction::Left;
        case 4: return UiAction::Right;
        default: return std::nullopt;
    }
}

// Every sound, 48 kHz int16 stereo, as assets/ui_sounds.bin carries them.
struct UiSoundPack {
    int rate = 0;
    std::array<std::vector<int16_t>, kUiSoundCount> clips;
};
// False, with why, for anything that is not a whole pack with every sound.
// A pack carries several styles, each sound named "<style>/<sound>"; this
// reads `style`'s, or the first style's when it has no such style.
bool decode_ui_sound_pack(const uint8_t* data, std::size_t size, UiSoundPack& out, std::string* error,
                          const std::string& style = std::string());

// Up to eight sounds at once, mixed into whatever the device asks for. No
// device and no clock here: what a buffer of sound comes out as, so it can be
// tested. A cursor's sound starts again rather than piling up -- held down,
// the cursor moves every 75 ms, faster than the sound dies away -- and a
// voice wanted when all eight are busy is the one furthest along.
class UiSoundMixer {
public:
    void set_pack(const UiSoundPack* pack) { pack_ = pack; }
    void start(UiSound sound);
    void stop_all();
    // Writes `frames` of int16 stereo to `out`: the voices summed, or silence.
    void mix(int16_t* out, std::size_t frames);
    std::size_t playing() const;
    // A sum of voices, back into 16 bits without clipping hard.
    static int16_t limit(int32_t sample);

private:
    struct Voice {
        int sound = -1;
        std::size_t at = 0;  // frames in
    };
    const UiSoundPack* pack_ = nullptr;
    std::array<Voice, 8> voices_{};
    std::vector<int32_t> sum_;  // kept, so mixing allocates only the first time
};

// Which sound, if any, a press has earned -- decided by what it did rather
// than by which button it was. A press that changes nothing (a choice that is
// not there) makes no sound -- but for a direction against the end of where
// the cursor can go, below; one that does makes the sound of what was
// pressed. A press only counts for half a
// second: on a device showing another's menu, what it changed arrives with
// the next update from there, a frame or two later.
//
// The quick menu's own sounds go with it opening and closing over a game,
// whoever opened it, the error sound with an error appearing, and the save
// chime with a save state made -- in place of the press's own sound. A press
// that flips the switch the cursor is on -- A or a tap; no direction flips
// one -- sounds which way it went, on or off, in place of a choice's.
//
// A direction that moves nothing while the cursor is a rounded ring bumps,
// as a Switch's cursor does: the ring jiggles that way and makes a small
// sound -- see bump_direction(). Only the press itself, and only on the view
// just after it, the menus moving their cursors as they hear a press; a
// direction held against the end does not go on bumping. A cursor that is
// not a ring -- the quick menu's sections -- stays silent, as before.
class UiSoundJudge {
public:
    // `repeating`: the direction among `actions` is a held one repeating,
    // not a press.
    void note_input(const std::vector<UiAction>& actions, bool tapped, double now_seconds,
                    bool repeating = false);
    std::optional<UiSound> observe(const UiSoundView& view, double now_seconds);
    // Which way the last Bump went: Up, Down, Left or Right.
    UiAction bump_direction() const { return bump_direction_; }

private:
    enum class Pending { None, Move, Confirm, Cancel, Tap };
    Pending pending_ = Pending::None;
    double pending_at_ = 0.0;
    // The direction pressed, while the press is still to be looked at.
    std::optional<UiAction> pressed_direction_;
    bool pressed_repeating_ = false;
    UiAction bump_direction_ = UiAction::Up;
    // The last direction that moved nothing, and when: one coming in again
    // within a few hundredths is held, whatever said it -- the other device's
    // presses carry no word of repeating.
    std::optional<UiAction> blocked_direction_;
    double blocked_at_ = -1.0;
    bool have_view_ = false;
    UiSoundView last_{};
};

// Plays them, on a device of its own: a game's sound goes through a queue held
// a tenth of a second deep (see AudioOutput), which is right for a game and
// far too late for a click, and it is paused whenever a menu is up -- exactly
// when these are wanted. Off a Switch, a second SDL device with a buffer of
// 5 ms; on one, the console's plain audio output, since SDL's Switch audio
// has the console's one audio renderer to itself and wants 85 ms buffers.
// Silent, not failing, wherever neither opens -- which includes Android, where
// SDL has only the one device and the game's sound needs it.
class UiSoundPlayer {
public:
    UiSoundPlayer();
    ~UiSoundPlayer();
    UiSoundPlayer(const UiSoundPlayer&) = delete;
    UiSoundPlayer& operator=(const UiSoundPlayer&) = delete;

    bool open(std::string* error);
    void close();
    bool is_open() const;
    void set_enabled(bool enabled) { enabled_ = enabled; }
    bool enabled() const { return enabled_; }
    void play(UiSound sound);

private:
    struct Backend;
    std::unique_ptr<Backend> backend_;
    UiSoundPack pack_;
    bool enabled_ = true;
};

}  // namespace ds
