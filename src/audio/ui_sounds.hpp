#pragma once

#include <SDL.h>

#include <cstdint>
#include <string>
#include <vector>

namespace ds {

// The shell's own sounds. Named for the moment each belongs to rather than for
// what it sounds like, so a call site reads as what happened.
enum class UiSound {
    Confirm,
    Cancel,
    MoveCursor,
    Error,
    ConnectionTimeout,
    OpenQuickMenu,
    CloseQuickMenu,
    ConnectingLoop,
    Count,
};

// Plays them, on a device of its own rather than mixed into the game's.
//
// The game's output is opened at whatever rate its core asks for and is not
// open at all outside a game, and these have to be audible in a launcher as
// much as in a menu. A second device costs one more SDL handle and saves
// resampling every effect to a rate that changes with the console being
// played.
class UiSounds {
public:
    ~UiSounds();

    bool initialize(std::string* error);
    void shutdown();
    bool ready() const { return device_ != 0; }

    // Starts one, over the top of whatever else is still sounding. Playing the
    // same effect twice in quick succession restarts it rather than stacking,
    // which is what a cursor held against the end of a list would otherwise do.
    void play(UiSound sound);
    // The one effect that repeats: it plays under a wait, so it ends when the
    // waiting does rather than after a fixed time.
    void start_loop(UiSound sound);
    void stop_loop();

private:
    struct Clip {
        const int16_t* samples = nullptr;
        std::size_t frames = 0;
    };
    struct Voice {
        int sound = -1;
        std::size_t position = 0;
        bool looping = false;
    };

    static void SDLCALL fill(void* opaque, uint8_t* stream, int bytes);
    void mix(int16_t* out, std::size_t frames);

    SDL_AudioDeviceID device_ = 0;
    std::vector<Clip> clips_;
    std::vector<Voice> voices_;
};

// Decodes the packed effects. Exposed for its own sake so the pack can be
// checked without opening an audio device.
bool decode_ui_sound_pack(const uint8_t* data,
                          std::size_t size,
                          std::vector<const int16_t*>& out_samples,
                          std::vector<std::size_t>& out_frames);

}  // namespace ds
