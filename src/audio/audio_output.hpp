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
//
// A ceiling rather than a multiple of the target, which is the mistake the
// first version of the device-buffer floor made: four times a 170ms target put
// this at 682ms, so a queue that was running away had most of a second to run
// away into before anything stopped it. Whatever the target ends up being,
// what matters here is how far behind is too far to be worth keeping, and that
// is a duration.
//
// With kAudioTrimMarginMs below holding the queue near its target this should
// now only ever fire on a genuine stall, which is what it was always for. It
// spent a long time doing the trim's job badly instead: a source sending more
// than real time walked the queue all the way up here, over and over, and
// every cycle of that was most of half a second of lag followed by an audible
// gap.
inline constexpr int kAudioPanicQueueMs = 400;

// The most the playback rate is ever bent to correct drift, as a fraction.
//
// 1% is about 17 cents of pitch, which nobody hears on game audio. This was
// briefly 5%, to let the correction converge against a queue that kept growing
// on a Switch, and that was a mistake worth writing down: 5% is most of a
// semitone, and a queue that stays above target holds the correction there
// permanently, so what it bought was not a converged queue but a whole session
// transposed. Both companions came back higher-pitched than the same game
// playing out of the host's own speakers, which is exactly what a saturated
// correction sounds like.
//
// The argument for the wider limit -- that a sink genuinely running slow needs
// a correction its own size, and that resampling to the rate the device really
// runs at is what makes a second of game audio take a second -- is sound as
// far as it goes, and it does not go far enough. It only holds when the rates
// differ because the *sink* is off. A source handing over more than a second
// of audio per second is the other case entirely, and there the queue is not
// drift to be resampled away, it is surplus: pitching everything up to burn it
// off plays the game sharp for as long as the surplus lasts. Both companions
// showing it at once points at the source, not at two unrelated clocks.
//
// So this stays inaudible, and a queue the correction cannot hold is dealt
// with by throwing the surplus away (see kAudioTrimMarginMs) rather than by
// bending pitch further. report() says "(at the limit)" whenever the
// correction is saturated, which is what makes the difference visible.
inline constexpr double kAudioMaxRateAdjust = 0.01;

// How far past the target the queue may wander before the surplus is thrown
// away rather than resampled off, in milliseconds of audio.
//
// This is the other half of what kAudioMaxRateAdjust deliberately will not do.
// A source handing over more audio per second than a second holds -- measured
// at 2 to 5 parts per hundred from a Cemu running a little fast -- is surplus,
// and a correction limited to one part per hundred cannot take it off however
// long it is given. The queue therefore grows, and with only the panic ceiling
// above it the whole surplus is carried for fourteen seconds, at up to 400ms
// of lag, before being dropped in one audible gap. Measured on a Mac companion:
// the queue walked 169, 202, 225, ... 361ms and started again, over and over,
// with "(at the limit)" on every line in between.
//
// So the surplus is taken off as it appears instead. The total thrown away is
// the same either way -- it is however much more than real time the source is
// sending -- and this is the choice of paying it in small change or in one
// note. Small change wins twice: the queue stays near the target, which is the
// whole of the latency this carries, and a few milliseconds spliced out (see
// audio_splice) is a tick where a third of a second is a skip.
inline constexpr int kAudioTrimMarginMs = 40;

// Frames crossfaded across a splice, at the rate the device is running.
// About three milliseconds at 48kHz: long enough that the join is a ramp
// rather than a step, short enough to be over before it is heard as anything
// of its own.
inline constexpr std::size_t kAudioSpliceFrames = 128;

// How many frames of a newly arrived block to throw away, given how deep the
// queue already is. Zero until the queue is more than `margin_frames` past the
// target.
//
// Capped at half the block so there is always audio on both sides of the join
// for audio_splice() to cross between -- a whole block dropped is a hard cut,
// and paying a large surplus off over several blocks costs nothing, since the
// queue is read afresh on each one.
std::size_t audio_trim_frames(std::size_t queued_frames, std::size_t target_frames,
                              std::size_t margin_frames, std::size_t block_frames);

// Appends `frames` of stereo int16 to `out` with the first `skip` of them
// dropped, crossfading from what was dropped into what follows so the join is
// not a step. With `skip` of zero this is a plain copy.
void audio_splice(const int16_t* interleaved_stereo, std::size_t frames, std::size_t skip,
                  std::vector<int16_t>& out);

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
// Drift -- two clocks that are near enough the same and slowly parting -- is
// not handled by throwing samples away. Discarding a whole packet because the
// queue got long puts a step discontinuity in the middle of a waveform, which
// is precisely what a click or a pop is, and it does it silently and
// repeatedly for as long as the drift continues -- so the symptom is a stream
// that pops every few seconds while every packet arrives intact and nothing
// anywhere reports a loss. Instead the queue is held near
// kAudioTargetQueueMs by bending the playback rate a fraction of a percent
// (see audio_drift_ratio), which is inaudible and self-correcting.
//
// Surplus -- a source genuinely sending more than a second of audio per second
// -- is the other case, and it cannot be resampled away at any rate worth
// listening to. That much is thrown away, a few milliseconds at a time and
// crossfaded across the join, which is the least damage available: see
// kAudioTrimMarginMs and audio_splice.
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
    std::size_t trim_margin_frames_ = 0;
    double rate_ratio_ = 1.0;
    // What report() says. Cleared every time it does.
    uint64_t stats_at_ms_ = 0;
    uint64_t stats_queue_total_ = 0;
    std::size_t stats_queue_worst_ = 0;
    unsigned stats_pushes_ = 0;
    unsigned stats_empty_ = 0;
    unsigned stats_panics_ = 0;
    unsigned stats_trims_ = 0;
    uint64_t stats_trimmed_frames_ = 0;
    // Frames handed in since the last report, against the wall clock it took
    // to hand them in. The queue depth says the two ends disagree; only this
    // says which way and by how much, and whether the disagreement is the
    // sink's or the source's -- a source sending 49000 frames a second of
    // "48000 Hz" audio is a surplus no playback rate should be asked to
    // absorb. See kAudioMaxRateAdjust.
    uint64_t stats_frames_in_ = 0;
    AudioResampler resampler_;
    std::vector<int16_t> spliced_;
    std::vector<int16_t> scratch_;
    bool primed_ = false;
    bool paused_ = true;
};

}  // namespace ds
