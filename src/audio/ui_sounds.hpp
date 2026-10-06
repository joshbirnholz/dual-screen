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
// out of, something refused, and the quick menu opening and closing over a
// game. The owner's own recordings, in assets/ui_sounds/, made ready by
// assets/make_ui_sounds.py and carried inside the binary.
enum class UiSound { MoveCursor, Confirm, Cancel, Error, OpenQuickMenu, CloseQuickMenu };
inline constexpr int kUiSoundCount = 6;
const char* ui_sound_name(UiSound sound);

// Every sound, 48 kHz int16 stereo, as assets/ui_sounds.bin carries them.
struct UiSoundPack {
    int rate = 0;
    std::array<std::vector<int16_t>, kUiSoundCount> clips;
};
// False, with why, for anything that is not a whole pack with every sound.
bool decode_ui_sound_pack(const uint8_t* data, std::size_t size, UiSoundPack& out, std::string* error);

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
// than by which button it was. A press that changes nothing (the cursor at
// the end of a list, a choice that is not there) makes no sound; one that
// does makes the sound of what was pressed. A press only counts for half a
// second: on a device showing another's menu, what it changed arrives with
// the next update from there, a frame or two later.
//
// The quick menu's own sounds go with it opening and closing over a game,
// whoever opened it, and the error sound with an error appearing.
class UiSoundJudge {
public:
    void note_input(const std::vector<UiAction>& actions, bool tapped, double now_seconds);
    std::optional<UiSound> observe(const UiSoundView& view, double now_seconds);

private:
    enum class Pending { None, Move, Confirm, Cancel, Tap };
    Pending pending_ = Pending::None;
    double pending_at_ = 0.0;
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
