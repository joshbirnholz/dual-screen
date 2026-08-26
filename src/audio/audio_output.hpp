#pragma once

#include <SDL.h>

#include <cstdint>
#include <string>

namespace ds {

class UiSounds;

// Stereo 16-bit audio out, fed by the core's sample batches.
//
// The DS runs at ~59.83 Hz against a 60 Hz display, so the queue drifts even
// when everything is behaving. Rather than resample, the queue is bounded: it
// is topped up with silence if it runs dry and trimmed if it runs long, which
// is inaudible at these magnitudes and keeps latency from creeping up over a
// long session.
class AudioOutput {
public:
    ~AudioOutput();

    // The shell's own effects, mixed into this stream. Where a platform has
    // only one audio device -- the Switch does -- they cannot have one of
    // their own, so they borrow this one and hand it back when it closes.
    void set_effects(UiSounds* effects);

    bool open(double sample_rate, std::string* error);
    void close();
    bool is_open() const { return device_ != 0; }

    void push(const int16_t* interleaved_stereo, std::size_t frames);
    void set_paused(bool paused);
    // Drops everything queued — used when resuming from suspend, where the
    // queue is both stale and arbitrarily deep.
    void flush();

    // Keeps the effects audible when the game itself has gone quiet, which is
    // most of the time they matter: the game is paused behind the very menu
    // making them. Tops the queue up with the effects alone, and does nothing
    // while the game is filling it.
    void pump_effects();

    double sample_rate() const { return sample_rate_; }
    std::size_t queued_frames() const;

private:
    // Hands an already-mixed buffer to the device, through the resampler if
    // this device would not take the rate the game asked for.
    void queue(const int16_t* interleaved_stereo, std::size_t frames);

    SDL_AudioDeviceID device_ = 0;
    SDL_AudioSpec spec_ {};
    SDL_AudioStream* stream_ = nullptr;  // only when the device refused our rate
    double sample_rate_ = 0.0;
    std::size_t high_watermark_frames_ = 0;
    bool paused_ = true;
    UiSounds* effects_ = nullptr;
};

}  // namespace ds
