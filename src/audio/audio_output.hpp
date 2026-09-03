#pragma once

#include <SDL.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ds {

// How deep the outgoing queue is aimed at, and how far off it has to get
// before the correction below is running as hard as it ever will. Both are
// milliseconds of audio.
//
// The target is the latency this deliberately carries: enough to ride out the
// jitter between a packet arriving and the device asking for it (a poll cadence
// of ~11ms, plus whatever the network adds on top), and no more, because every
// millisecond of it is a millisecond between pressing a button and hearing the
// result.
inline constexpr int kAudioTargetQueueMs = 60;
// Past this the queue is not drifting, it is stalled -- a device that stopped
// consuming, a session that was suspended -- and no gentle correction is going
// to walk back a backlog that size in any reasonable time. Started clean
// instead. Well clear of the target so ordinary drift never reaches it.
inline constexpr int kAudioPanicQueueMs = 400;

// The most the playback rate is ever bent to correct drift, as a fraction. 1%
// is about 17 cents of pitch -- inaudible on game audio, and enough to close a
// full target's worth of error in a few seconds.
inline constexpr double kAudioMaxRateAdjust = 0.01;

// Playback rate to run at given how deep the queue currently is. Above the
// target it returns slightly more than 1 (consume input faster, emit fewer
// frames, so the queue drains); below it, slightly less. Clamped to
// kAudioMaxRateAdjust either way and eased towards from `current` rather than
// jumped to, so the rate itself never steps audibly.
double audio_drift_ratio(std::size_t queued_frames, std::size_t target_frames, double current);

// Linear-interpolating resampler for stereo int16 at a ratio very near 1.
//
// Split out from AudioOutput because it is the part worth testing on its own:
// it has no device, no SDL and no timing, just samples in and samples out. The
// state it does carry -- a fractional read position and the previous block's
// last frame -- is exactly what keeps one block continuous with the next, so
// correcting drift never itself becomes the discontinuity it exists to avoid.
class AudioResampler {
public:
    void reset();
    // Appends to `out` rather than clearing it.
    void resample(const int16_t* interleaved_stereo, std::size_t frames, double ratio,
                  std::vector<int16_t>& out);

private:
    double phase_ = 0.0;
    int16_t last_frame_[2] = {0, 0};
    bool have_last_ = false;
};

// Stereo 16-bit audio out, fed by the core's sample batches or by a host's
// streamed ones.
//
// Two clocks are always involved and they are never quite the same: whatever
// produces the samples (an emulated console's audio rate, or a host machine's,
// arriving over a network) and the sound device actually playing them. Left
// alone the queue between them drifts -- one way until it is deep enough to be
// audible as lag, the other until it runs dry.
//
// Neither end of that is handled by throwing samples away. Discarding a whole
// packet because the queue got long puts a step discontinuity in the middle of
// a waveform, which is precisely what a click or a pop is, and it does it
// silently and repeatedly for as long as the drift continues -- so the symptom
// is a stream that pops every few seconds while every packet arrives intact and
// nothing anywhere reports a loss. Instead the queue is held near
// kAudioTargetQueueMs by bending the playback rate a fraction of a percent
// (see audio_drift_ratio), which is inaudible and self-correcting, and the
// samples themselves are always played in full.
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
    // The rate correction currently being applied, for tests and diagnostics.
    double rate_ratio() const { return rate_ratio_; }

    // Says once a second what the queue has been doing, if anything has been
    // pushed since the last time. `now_ms` is the caller's own clock, the same
    // one the session's stream report runs on -- the two lines are meant to be
    // read side by side.
    //
    // Cheap to call every frame: it does nothing at all until a second has
    // passed.
    void report(uint64_t now_ms);

private:
    // Queues target_frames_ of silence so playback starts from the depth the
    // correction is aiming to hold, rather than from empty -- which would
    // otherwise underrun immediately and have to climb out of it.
    void prime();

    SDL_AudioDeviceID device_ = 0;
    SDL_AudioSpec spec_ {};
    SDL_AudioStream* stream_ = nullptr;  // only when the device refused our rate
    double sample_rate_ = 0.0;
    std::size_t target_frames_ = 0;
    std::size_t panic_frames_ = 0;
    double rate_ratio_ = 1.0;
    // What report() says. Cleared every time it does.
    uint64_t stats_at_ms_ = 0;
    uint64_t stats_queue_total_ = 0;
    std::size_t stats_queue_worst_ = 0;
    unsigned stats_pushes_ = 0;
    unsigned stats_empty_ = 0;
    unsigned stats_panics_ = 0;
    AudioResampler resampler_;
    std::vector<int16_t> scratch_;
    bool primed_ = false;
    bool paused_ = true;
};

}  // namespace ds
