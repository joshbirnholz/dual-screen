#pragma once

#include <SDL.h>

#include <cstdint>
#include <string>

namespace ds {

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

    bool open(double sample_rate, std::string* error);
    void close();
    bool is_open() const { return device_ != 0; }

    void push(const int16_t* interleaved_stereo, std::size_t frames);
    void set_paused(bool paused);
    // Drops everything queued — used when resuming from suspend, where the
    // queue is both stale and arbitrarily deep.
    void flush();

    double sample_rate() const { return sample_rate_; }
    std::size_t queued_frames() const;

private:
    SDL_AudioDeviceID device_ = 0;
    SDL_AudioSpec spec_ {};
    SDL_AudioStream* stream_ = nullptr;  // only when the device refused our rate
    double sample_rate_ = 0.0;
    std::size_t high_watermark_frames_ = 0;
    bool paused_ = true;
};

}  // namespace ds
