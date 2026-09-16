#include "audio/audio_output.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

#include "util/log.hpp"

namespace ds {
namespace {

constexpr int kBytesPerFrame = 4;  // 2 channels x int16

// How much of the way to the newly computed rate each block moves. The
// correction is chasing clock drift, which changes over seconds, not over
// single packets, so easing towards it keeps a momentarily long or short queue
// -- one late packet, one busy frame -- from swinging the rate around.
constexpr double kRatioSmoothing = 0.05;

// Turns "how far off target" into a rate. Deliberately steep next to
// kAudioMaxRateAdjust: anything more than about a quarter of the target out
// already runs at the full correction, so real drift is worked off as fast as
// it can be while the rate still stays inaudible.
constexpr double kRateGain = 0.04;

int16_t lerp_sample(int16_t a, int16_t b, double t) {
    const double value = static_cast<double>(a) + (static_cast<double>(b) - a) * t;
    return static_cast<int16_t>(std::lround(std::clamp(value, -32768.0, 32767.0)));
}

}  // namespace

double audio_drift_ratio(std::size_t queued_frames, std::size_t target_frames, double current) {
    if (target_frames == 0) {
        return 1.0;
    }
    const double target = static_cast<double>(target_frames);
    const double error = (static_cast<double>(queued_frames) - target) / target;
    const double desired = std::clamp(1.0 + kRateGain * error, 1.0 - kAudioMaxRateAdjust,
                                      1.0 + kAudioMaxRateAdjust);
    return current + (desired - current) * kRatioSmoothing;
}

void AudioLowWater::reset(std::size_t window_frames, std::size_t queued) {
    window_frames_ = window_frames;
    filled_frames_ = 0;
    // Both buckets start where the queue is, so a stream that has just been
    // primed does not read as empty before a single push has happened.
    current_ = queued;
    previous_ = queued;
}

std::size_t AudioLowWater::note(std::size_t queued, std::size_t frames) {
    if (window_frames_ == 0) {
        // Never told how far to look back, so there is nothing to look back
        // over. Answer with the depth in front of it rather than with a mark
        // that can only ever fall: reading the instant is worse than reading
        // the trough, and never reading anything above the target at all --
        // which is what a zero window silently did -- is worse than both.
        current_ = queued;
        previous_ = queued;
        return queued;
    }
    current_ = std::min(current_, queued);
    filled_frames_ += frames;
    if (window_frames_ > 0 && filled_frames_ >= window_frames_) {
        // The bucket just closed becomes the older of the two, and the new one
        // starts from where the queue is now rather than from nothing -- an
        // empty bucket would read as a queue at zero for as long as it took to
        // fill, which is the opposite of what this is for.
        previous_ = current_;
        current_ = queued;
        filled_frames_ = 0;
    }
    return depth();
}

std::size_t AudioLowWater::depth() const { return std::min(current_, previous_); }

std::size_t audio_trim_frames(std::size_t sustained_frames, std::size_t target_frames,
                              std::size_t margin_frames, std::size_t block_frames) {
    if (sustained_frames <= target_frames + margin_frames) {
        return 0;
    }
    const std::size_t surplus = sustained_frames - target_frames;
    // Half the block, so audio_splice() has something on both sides of the
    // join. A block too short to cut in half is left whole rather than cut
    // badly; the next one is read against a queue that has not moved, so
    // nothing is lost by waiting for it.
    return std::min(surplus, block_frames / 2);
}

void audio_splice(const int16_t* interleaved_stereo, std::size_t frames, std::size_t skip,
                  std::vector<int16_t>& out) {
    if (interleaved_stereo == nullptr || frames == 0) {
        return;
    }
    if (skip >= frames) {
        return;
    }
    const std::size_t kept = frames - skip;
    const std::size_t base = out.size();
    out.resize(base + kept * 2);
    int16_t* dest = out.data() + base;
    std::copy_n(interleaved_stereo + skip * 2, kept * 2, dest);
    if (skip == 0) {
        return;
    }
    // Ramp from what is being dropped into what replaces it. Both sides are
    // the same signal a few milliseconds apart, so this is a short crossfade
    // rather than a fade to nothing -- the level holds, and what changes is
    // only the phase, over long enough not to be heard as an edge.
    const std::size_t blend = std::min({kAudioSpliceFrames, skip, kept});
    for (std::size_t i = 0; i < blend; ++i) {
        const double t = static_cast<double>(i + 1) / static_cast<double>(blend + 1);
        for (int channel = 0; channel < 2; ++channel) {
            const double from = interleaved_stereo[i * 2 + channel];
            const double to = interleaved_stereo[(skip + i) * 2 + channel];
            dest[i * 2 + channel] =
                static_cast<int16_t>(std::lround(std::clamp(from + (to - from) * t,
                                                            -32768.0, 32767.0)));
        }
    }
}

void AudioResampler::reset() {
    phase_ = 0.0;
    last_frame_[0] = 0;
    last_frame_[1] = 0;
    have_last_ = false;
}

void AudioResampler::resample(const int16_t* interleaved_stereo, std::size_t frames, double ratio,
                              std::vector<int16_t>& out) {
    if (interleaved_stereo == nullptr || frames == 0 || ratio <= 0.0) {
        return;
    }

    // Index -1 is the previous block's last frame, so a read position that
    // lands between two blocks interpolates across the join instead of
    // restarting from this block's first sample.
    const auto frame_at = [&](std::ptrdiff_t index, int channel) -> int16_t {
        if (index < 0) {
            return have_last_ ? last_frame_[channel] : interleaved_stereo[channel];
        }
        const std::size_t clamped =
            std::min(static_cast<std::size_t>(index), frames - 1);
        return interleaved_stereo[clamped * 2 + static_cast<std::size_t>(channel)];
    };

    // phase_ is the read position the next output frame falls at, measured
    // from this block's first frame. It starts at 0 for the very first block
    // and afterwards carries over somewhere in [-1, 0): the tail of the
    // previous block that the next output frame interpolates out of, which is
    // what index -1 above supplies.
    double position = phase_;
    while (position + 1.0 < static_cast<double>(frames)) {
        const double floor_position = std::floor(position);
        const auto index = static_cast<std::ptrdiff_t>(floor_position);
        const double fraction = position - floor_position;
        out.push_back(lerp_sample(frame_at(index, 0), frame_at(index + 1, 0), fraction));
        out.push_back(lerp_sample(frame_at(index, 1), frame_at(index + 1, 1), fraction));
        position += ratio;
    }

    phase_ = std::clamp(position - static_cast<double>(frames), -1.0, 0.0);
    last_frame_[0] = interleaved_stereo[(frames - 1) * 2];
    last_frame_[1] = interleaved_stereo[(frames - 1) * 2 + 1];
    have_last_ = true;
}

AudioOutput::~AudioOutput() { close(); }

bool AudioOutput::open(double sample_rate, std::string* error) {
    close();

    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        if (error != nullptr) *error = std::string("could not start SDL audio: ") + SDL_GetError();
        return false;
    }

    SDL_AudioSpec desired {};
    desired.freq = static_cast<int>(sample_rate + 0.5);
    desired.format = AUDIO_S16SYS;
    desired.channels = 2;
    // ~11ms at 44.1kHz; small enough to stay responsive, large enough not to
    // underrun when a frame takes longer than usual.
    desired.samples = 512;
#if defined(__SWITCH__)
    // Except on a Switch, where SDL2's audio backend does not deliver a buffer
    // this small cleanly -- the sound comes out broken up rather than merely
    // late. Moonlight-Switch hit the same thing and pins 4096 for the same
    // reason (SDLAudioRenderer.cpp: "Latest SDL2 Switch port broke audio for
    // lower samples"). That is ~85ms a buffer, which is latency this would
    // rather not carry, but a clean 85ms beats a broken 11ms.
    desired.samples = 4096;
#endif
    desired.callback = nullptr;  // queue-driven

    device_ = SDL_OpenAudioDevice(nullptr, 0, &desired, &spec_, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
    if (device_ == 0) {
        if (error != nullptr) {
            *error = std::string("could not open an audio device: ") + SDL_GetError();
        }
        return false;
    }

    sample_rate_ = sample_rate;

    if (spec_.freq != desired.freq) {
        // The device would not take the core's rate, so convert on the way in.
        stream_ = SDL_NewAudioStream(AUDIO_S16SYS, 2, desired.freq, spec_.format, spec_.channels,
                                     spec_.freq);
        if (stream_ == nullptr) {
            log_warn(std::string("could not create an audio resampler: ") + SDL_GetError() +
                     "; audio will play at the wrong pitch");
        } else {
            log_info("resampling core audio from " + std::to_string(desired.freq) + " Hz to " +
                     std::to_string(spec_.freq) + " Hz");
        }
    }

    // Both measured against the device's own rate, since that is what the queue
    // these bound is actually drained at.
    target_frames_ = static_cast<std::size_t>(spec_.freq) * kAudioTargetQueueMs / 1000;
    panic_frames_ = static_cast<std::size_t>(spec_.freq) * kAudioPanicQueueMs / 1000;
    // A target below what the device takes in one go is not a target, it is a
    // guaranteed underrun: the device empties the queue past it on every
    // single refill, and the drift correction spends the whole session winding
    // the rate up to chase a level it can never hold. So the target is never
    // less than a couple of the device's own buffers, whatever it granted --
    // which matters most exactly where the buffer had to be made large.
    const std::size_t device_buffer_frames = static_cast<std::size_t>(spec_.samples);
    const std::size_t floor_frames = device_buffer_frames * 2;
    if (target_frames_ < floor_frames) {
        log_info("audio: device buffer is " + std::to_string(device_buffer_frames) +
                 " frames, so aiming the queue at " + std::to_string(floor_frames) +
                 " rather than " + std::to_string(target_frames_));
        target_frames_ = floor_frames;
    }
    // Still clear of the target, since ordinary drift must never reach it --
    // but by a margin rather than a multiple, so a large target cannot push
    // this out to most of a second of backlog. See kAudioPanicQueueMs.
    constexpr int kPanicMarginMs = 200;
    const std::size_t margin_frames =
        static_cast<std::size_t>(spec_.freq) * kPanicMarginMs / 1000;
    panic_frames_ = std::max(panic_frames_, target_frames_ + margin_frames);
    trim_margin_frames_ = static_cast<std::size_t>(spec_.freq) * kAudioTrimMarginMs / 1000;
    // A margin smaller than one of the device's own refills would have the
    // queue crossing it purely on the sawtooth of being drained in whole
    // buffers, and the surplus trimmed away would be the device's cadence
    // rather than anything the source did.
    trim_margin_frames_ = std::max(trim_margin_frames_, device_buffer_frames);
    low_water_window_frames_ =
        static_cast<std::size_t>(spec_.freq) * kAudioLowWaterWindowMs / 1000;
    rate_ratio_ = 1.0;
    resampler_.reset();
    primed_ = false;

    SDL_PauseAudioDevice(device_, 0);
    paused_ = false;
    return true;
}

void AudioOutput::close() {
    if (stream_ != nullptr) {
        SDL_FreeAudioStream(stream_);
        stream_ = nullptr;
    }
    if (device_ != 0) {
        SDL_CloseAudioDevice(device_);
        device_ = 0;
    }
    paused_ = true;
}

void AudioOutput::set_paused(bool paused) {
    if (device_ == 0 || paused_ == paused) {
        return;
    }
    SDL_PauseAudioDevice(device_, paused ? 1 : 0);
    paused_ = paused;
}

void AudioOutput::flush() {
    if (device_ != 0) {
        SDL_ClearQueuedAudio(device_);
    }
    if (stream_ != nullptr) {
        SDL_AudioStreamClear(stream_);
    }
    rate_ratio_ = 1.0;
    resampler_.reset();
    primed_ = false;
}

std::size_t AudioOutput::queued_frames() const {
    if (device_ == 0) return 0;
    return SDL_GetQueuedAudioSize(device_) / kBytesPerFrame;
}

void AudioOutput::report(uint64_t now_ms) {
    if (stats_at_ms_ == 0) {
        stats_at_ms_ = now_ms;
        return;
    }
    if (now_ms - stats_at_ms_ < 1000 || stats_pushes_ == 0) {
        return;
    }
    const uint64_t elapsed_ms = now_ms - stats_at_ms_;
    stats_at_ms_ = now_ms;
    const double rate = sample_rate_ > 0.0 ? sample_rate_ : 48000.0;
    const auto to_ms = [rate](uint64_t frames) {
        return static_cast<uint64_t>(frames * 1000.0 / rate);
    };
    log_debug("audio: queue " + std::to_string(to_ms(stats_queue_total_ / stats_pushes_)) +
              "ms avg " + std::to_string(to_ms(stats_queue_worst_)) + "ms worst " +
              // The one that says whether a deep average is a queue running
              // late or one merely arriving in lumps, which is the difference
              // between a surplus worth throwing away and jitter worth keeping.
              // See AudioLowWater.
              std::to_string(to_ms(stats_queue_lowest_ == static_cast<std::size_t>(-1)
                                       ? 0
                                       : stats_queue_lowest_)) +
              "ms at its lowest, against a " +
              std::to_string(to_ms(target_frames_)) + "ms target, " + std::to_string(stats_empty_) +
              " empty, " + std::to_string(stats_panics_) + " cut short, playing at " +
              std::to_string(static_cast<int>((rate_ratio_ - 1.0) * 10000.0)) + " parts per "
              "ten thousand off" +
              // Saturated means the queue is not being corrected so much as
              // merely resisted, and it will drift until something flushes
              // it. Worth saying outright: it is the difference between a
              // correction working and one that cannot.
              (std::abs(rate_ratio_ - 1.0) >= kAudioMaxRateAdjust * 0.98 ? " (at the limit)"
                                                                         : ""));
    // And where the disagreement comes from, which the queue depth alone
    // cannot say: how much audio actually arrived in the second just gone,
    // against how much a second of audio is. Well over means the far end is
    // sending faster than real time and no playback rate will fix it; near
    // enough means the queue is the sink's business. See kAudioMaxRateAdjust.
    if (elapsed_ms > 0 && rate > 0.0) {
        const double arriving = stats_frames_in_ * 1000.0 / static_cast<double>(elapsed_ms);
        log_debug("audio: " + std::to_string(static_cast<int>(arriving + 0.5)) +
                  " frames arrived in the last second against a " +
                  std::to_string(static_cast<int>(rate + 0.5)) + " Hz stream (" +
                  std::to_string(static_cast<int>((arriving / rate - 1.0) * 10000.0)) +
                  " parts per ten thousand off real time)");
    }
    if (stats_trims_ > 0) {
        log_debug("audio: threw away " + std::to_string(to_ms(stats_trimmed_frames_)) +
                  "ms over " + std::to_string(stats_trims_) +
                  " splices to keep the queue near its target; that much more than real "
                  "time is arriving");
    }
    stats_trims_ = 0;
    stats_trimmed_frames_ = 0;
    stats_frames_in_ = 0;
    stats_queue_lowest_ = static_cast<std::size_t>(-1);
    stats_queue_total_ = 0;
    stats_queue_worst_ = 0;
    stats_pushes_ = 0;
    stats_empty_ = 0;
    stats_panics_ = 0;
}

void AudioOutput::prime() {
    primed_ = true;
    if (target_frames_ != 0) {
        const std::vector<int16_t> silence(target_frames_ * 2, 0);
        SDL_QueueAudio(device_, silence.data(),
                       static_cast<Uint32>(silence.size() * sizeof(int16_t)));
    }
    // Wherever the queue now stands is where the low-water mark starts, on
    // every path through here -- there is no case where a stream begins and
    // the mark should still be carrying a reading from an older one.
    low_water_.reset(low_water_window_frames_, queued_frames());
}

void AudioOutput::push(const int16_t* interleaved_stereo, std::size_t frames) {
    if (device_ == 0 || interleaved_stereo == nullptr || frames == 0) {
        return;
    }

    if (!primed_) {
        prime();
    }

    if (queued_frames() > panic_frames_) {
        ++stats_panics_;
        // Not drift -- see kAudioPanicQueueMs. Everything queued is already
        // far enough behind to be worthless, so it goes, and playback picks up
        // from here rather than working through it.
        SDL_ClearQueuedAudio(device_);
        if (stream_ != nullptr) {
            SDL_AudioStreamClear(stream_);
        }
        rate_ratio_ = 1.0;
        resampler_.reset();
        prime();
    }

    const std::size_t queued = queued_frames();
    rate_ratio_ = audio_drift_ratio(queued, target_frames_, rate_ratio_);

    // Surplus, not drift -- see kAudioTrimMarginMs. Taken off the block on the
    // way in rather than off the queue behind it, which comes to the same
    // audio removed from the same stream and is the end SDL will let anything
    // be removed from: SDL_DequeueAudio() returns 0 on a playback device and
    // leaves the queue alone.
    //
    // Read against the low-water mark rather than the depth at this instant,
    // because audio arrives in lumps and the depth at a push is whatever the
    // lump just made it -- see AudioLowWater. The mark still moves as soon as
    // frames are dropped here (the depth above is read before this block is
    // added, so the next push sees the smaller queue), which is what keeps the
    // same surplus from being paid for twice.
    const std::size_t sustained = low_water_.note(queued, frames);
    const std::size_t skipped =
        audio_trim_frames(sustained, target_frames_, trim_margin_frames_, frames);
    if (skipped > 0) {
        ++stats_trims_;
        stats_trimmed_frames_ += skipped;
    }

    // What the queue is actually doing, once a second. Sound going wrong is
    // otherwise the one thing in this app with no numbers at all behind it --
    // "it does not sound right" and a log that says nothing. These four say
    // which of the three ways it can go wrong is happening: running dry
    // (empty, and the rate bent up to catch back up), running late (queued far
    // above the target, and bent down), or being cut off outright (the panic
    // above, which is audible as a jump).
    stats_queue_total_ += queued;
    stats_frames_in_ += frames;
    ++stats_pushes_;
    stats_queue_worst_ = std::max(stats_queue_worst_, queued);
    stats_queue_lowest_ = std::min(stats_queue_lowest_, queued);
    if (queued == 0) {
        ++stats_empty_;
    }

    spliced_.clear();
    audio_splice(interleaved_stereo, frames, skipped, spliced_);
    if (spliced_.empty()) {
        return;
    }

    scratch_.clear();
    resampler_.resample(spliced_.data(), spliced_.size() / 2, rate_ratio_, scratch_);
    if (scratch_.empty()) {
        return;
    }

    const int byte_count = static_cast<int>(scratch_.size() * sizeof(int16_t));
    if (stream_ != nullptr) {
        if (SDL_AudioStreamPut(stream_, scratch_.data(), byte_count) != 0) {
            return;
        }
        static thread_local std::vector<uint8_t> converted;
        const int available = SDL_AudioStreamAvailable(stream_);
        if (available <= 0) {
            return;
        }
        converted.resize(static_cast<std::size_t>(available));
        const int got = SDL_AudioStreamGet(stream_, converted.data(), available);
        if (got > 0) {
            SDL_QueueAudio(device_, converted.data(), static_cast<Uint32>(got));
        }
        return;
    }
    SDL_QueueAudio(device_, scratch_.data(), static_cast<Uint32>(byte_count));
}

}  // namespace ds
