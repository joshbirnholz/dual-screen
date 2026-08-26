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

// What to call one in a log, so a report of silence says whether the effect was
// ever asked for or only failed to come out of the device.
const char* to_string(UiSound sound);

// Plays them, mixed into whatever is already making sound.
//
// A device of its own would be simpler, but the Switch has exactly one and the
// game's audio needs it: taking a second one there silences the game entirely.
// So these hold a device only while nothing else does -- in a launcher, before
// a game has started -- and hand it over when the game opens one, mixing into
// that instead. See AudioOutput::set_effects().
class UiSounds {
public:
    ~UiSounds();

    // Reads the packed effects. A device is opened too if one is free, but
    // failing to get one is not a failure: the game's output will lend its own.
    bool initialize(std::string* error);
    void shutdown();
    bool ready() const { return !clips_.empty(); }

    // Handed over when the game's output wants the only device there is, and
    // handed back when it is done with it.
    void release_device();
    void take_device();

    // Mixes whatever is sounding into a buffer somebody else is about to play,
    // at that buffer's rate and channel count. The effects are packed at one
    // rate, so this resamples them on the way in.
    //
    // Safe without a lock of its own: this is only reached from the game's
    // audio path while these have no device -- and so no callback thread -- of
    // their own, which is the only arrangement that exists.
    void render_into(int16_t* interleaved, std::size_t frames, int sample_rate, int channels);

    // Silences everything while another device is the one making the game's
    // sound. The shell's own effects belong wherever the game's do: two
    // devices side by side, both clicking, is one click too many.
    void set_muted(bool muted);
    bool muted() const { return muted_; }
    // Whether anything is sounding right now.
    bool sounding() const;

    // The shape of the device these hold, when they hold one at all: zero for
    // both when the game's output has it. Reported so a platform that grants
    // something other than what was asked for says so in the log.
    int device_rate() const { return device_rate_; }
    int device_channels() const { return device_channels_; }

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
        // Fractional, because the buffer being mixed into is at whatever rate
        // the game asked for and the effects are packed at one of their own.
        double position = 0.0;
        bool looping = false;
    };

    static void SDLCALL fill(void* opaque, uint8_t* stream, int bytes);
    bool open_device();

    SDL_AudioDeviceID device_ = 0;
    int device_rate_ = 0;
    int device_channels_ = 0;
    bool muted_ = false;
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
