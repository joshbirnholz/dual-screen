#include "audio/audio_output.hpp"

#include <algorithm>
#include <chrono>
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

uint64_t steady_ms() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}

int16_t lerp_sample(int16_t a, int16_t b, double t) {
    const double value = static_cast<double>(a) + (static_cast<double>(b) - a) * t;
    return static_cast<int16_t>(std::lround(std::clamp(value, -32768.0, 32767.0)));
}

}  // namespace

std::size_t audio_target_frames(int device_rate, std::size_t device_buffer_frames) {
    const std::size_t cover = static_cast<std::size_t>(device_rate) * kAudioTargetQueueMs / 1000;
    if (device_buffer_frames <= cover) {
        return cover;
    }
    return device_buffer_frames / 2 + cover;
}

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

std::size_t audio_cover_after_running_dry(std::size_t current_frames, std::size_t dry_frames,
                                          std::size_t step_frames, std::size_t ceiling_frames) {
    return std::min(current_frames + std::max(dry_frames, step_frames), ceiling_frames);
}

std::size_t audio_dry_spell_frames(std::size_t since_last_push_frames,
                                   std::size_t queued_after_last_push_frames) {
    return since_last_push_frames > queued_after_last_push_frames
               ? since_last_push_frames - queued_after_last_push_frames
               : 0;
}

std::size_t audio_splice(const int16_t* interleaved_stereo, std::size_t frames, std::size_t skip,
                         std::vector<int16_t>& out) {
    if (interleaved_stereo == nullptr || frames == 0) {
        return 0;
    }
    if (skip >= frames) {
        return 0;
    }
    if (skip == 0) {
        out.insert(out.end(), interleaved_stereo, interleaved_stereo + frames * 2);
        return 0;
    }
    const auto mono = [interleaved_stereo](std::size_t frame) {
        return static_cast<double>(interleaved_stereo[frame * 2]) +
               static_cast<double>(interleaved_stereo[frame * 2 + 1]);
    };

    // Where to cut: near `skip`, wherever the stretch after the cut looks most
    // like the stretch it takes over from -- see kAudioSpliceSearchFrames.
    const std::size_t window = std::min(kAudioSpliceFrames, frames - skip);
    const std::size_t reach = std::min(kAudioSpliceSearchFrames, skip / 2);
    const std::size_t lowest = std::max<std::size_t>(1, skip - reach);
    const std::size_t highest = std::min(skip + reach, frames - window);
    std::size_t cut = skip;
    if (window >= 16 && highest >= lowest) {
        double best = -2.0;
        for (std::size_t candidate = lowest; candidate <= highest; ++candidate) {
            double together = 0.0;
            double energy = 0.0;
            double reference = 0.0;
            for (std::size_t i = 0; i < window; ++i) {
                const double a = mono(i);
                const double b = mono(candidate + i);
                together += a * b;
                energy += b * b;
                reference += a * a;
            }
            // Normalised, so a loud stretch is not preferred for being loud;
            // silence on either side matches anything equally well.
            const double score =
                energy > 0.0 && reference > 0.0 ? together / std::sqrt(energy * reference) : 0.0;
            if (score > best) {
                best = score;
                cut = candidate;
            }
        }
    }

    const std::size_t kept = frames - cut;
    const std::size_t base = out.size();
    out.resize(base + kept * 2);
    int16_t* dest = out.data() + base;
    std::copy_n(interleaved_stereo + cut * 2, kept * 2, dest);
    // Ramp from what is being dropped into what replaces it. Both sides are
    // the same signal a few milliseconds apart and now in step, so this is a
    // short crossfade rather than a fade to nothing -- the level holds, and
    // nothing changes that can be heard as an edge.
    const std::size_t blend = std::min({kAudioSpliceFrames, cut, kept});
    for (std::size_t i = 0; i < blend; ++i) {
        const double t = static_cast<double>(i + 1) / static_cast<double>(blend + 1);
        for (int channel = 0; channel < 2; ++channel) {
            const double from = interleaved_stereo[i * 2 + channel];
            const double to = interleaved_stereo[(cut + i) * 2 + channel];
            dest[i * 2 + channel] =
                static_cast<int16_t>(std::lround(std::clamp(from + (to - from) * t,
                                                            -32768.0, 32767.0)));
        }
    }
    return cut;
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

    const int source_rate = static_cast<int>(sample_rate + 0.5);
    SDL_AudioSpec desired {};
    desired.freq = source_rate;
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
    // And at the Switch's own rate, 48kHz, converted to on the way in below --
    // which is what makes those 4096 frames 85ms. At a DS's 32.7kHz they are
    // 125ms, a buffer, and the Switch plays more than one buffer behind.
    desired.freq = 48000;
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

    if (spec_.freq != source_rate) {
        // The device is not at the core's rate -- it would not take it, or it
        // was not asked to -- so convert on the way in.
        stream_ = SDL_NewAudioStream(AUDIO_S16SYS, 2, source_rate, spec_.format, spec_.channels,
                                     spec_.freq);
        if (stream_ == nullptr) {
            log_warn(std::string("could not create an audio resampler: ") + SDL_GetError() +
                     "; audio will play at the wrong pitch");
        } else {
            log_info("resampling core audio from " + std::to_string(source_rate) + " Hz to " +
                     std::to_string(spec_.freq) + " Hz");
        }
    }

    // Both measured against the device's own rate, since that is what the queue
    // these bound is actually drained at.
    panic_frames_ = static_cast<std::size_t>(spec_.freq) * kAudioPanicQueueMs / 1000;
    // A target the device's own refills would take the queue past is not a
    // target, it is a guaranteed underrun, and the drift correction then
    // spends the whole session winding the rate up to chase a level it can
    // never hold. See audio_target_frames() for what is enough and why.
    const std::size_t device_buffer_frames = static_cast<std::size_t>(spec_.samples);
    target_frames_ = audio_target_frames(spec_.freq, device_buffer_frames);
    const std::size_t plain_target =
        static_cast<std::size_t>(spec_.freq) * kAudioTargetQueueMs / 1000;
    if (target_frames_ != plain_target) {
        log_info("audio: the device takes " + std::to_string(device_buffer_frames) +
                 " frames at a time at " + std::to_string(spec_.freq) + " Hz, so the queue is "
                 "held at " + std::to_string(target_frames_ * 1000 / spec_.freq) + "ms rather than " +
                 std::to_string(kAudioTargetQueueMs) + "ms");
    }
    // Still clear of the target, since ordinary drift must never reach it --
    // but by a margin rather than a multiple, so a large target cannot push
    // this out to most of a second of backlog. See kAudioPanicQueueMs.
    constexpr int kPanicMarginMs = 200;
    const std::size_t margin_frames =
        static_cast<std::size_t>(spec_.freq) * kPanicMarginMs / 1000;
    panic_frames_ = std::max(panic_frames_, target_frames_ + margin_frames);
    // Kept for push(), which holds the panic clear of the target plus whatever
    // cover has been bought on top of it.
    panic_margin_frames_ = margin_frames;
    cover_frames_ = 0;
    cover_ceiling_frames_ = static_cast<std::size_t>(spec_.freq) * kAudioCoverCeilingMs / 1000;
    cover_step_frames_ = static_cast<std::size_t>(spec_.freq) * kAudioCoverStepMs / 1000;
    last_push_ms_ = 0;
    queued_after_last_push_ = 0;
    last_dry_ms_ = 0;
    last_given_back_ms_ = 0;
    trim_margin_frames_ = static_cast<std::size_t>(spec_.freq) * kAudioTrimMarginMs / 1000;
    // Kept as a floor, though the low-water mark is what actually sees past
    // the sawtooth of the device draining in whole buffers now. This costs
    // nothing where the buffer is ordinary -- the margin above is already
    // wider -- and it is the one case the quarter-second window could not
    // cover on its own: a device whose single refill is itself most of that
    // window. See AudioLowWater and kAudioTrimMarginMs.
    trim_margin_frames_ = std::max(trim_margin_frames_, device_buffer_frames);
    low_water_window_frames_ =
        static_cast<std::size_t>(spec_.freq) * kAudioLowWaterWindowMs / 1000;
    rate_ratio_ = 1.0;
    resampler_.reset();
    reset_stretching();
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
    reset_stretching();
    primed_ = false;
    // A suspend is not a dry spell, and the time it took must not be read as
    // one by the first push after it.
    last_push_ms_ = 0;
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
              std::to_string(to_ms(aimed_frames())) + "ms target" +
              (cover_frames_ > 0 ? " (" + std::to_string(to_ms(cover_frames_)) +
                                       "ms of it bought after running dry)"
                                 : std::string()) +
              " it is trimmed towards once it sits above " +
              std::to_string(to_ms(aimed_frames() + trim_margin_frames_)) +
              "ms, " + std::to_string(stats_empty_) +
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
    if (stats_stretch_worst_ > 1.0) {
        log_debug("audio: the game is running slow, and its sound was stretched to up to " +
                  std::to_string(static_cast<int>(stats_stretch_worst_ * 100.0 + 0.5)) +
                  "% of its length to keep up with it");
    }
    stats_stretch_worst_ = 1.0;
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
    if (aimed_frames() != 0) {
        const std::vector<int16_t> silence(aimed_frames() * 2, 0);
        SDL_QueueAudio(device_, silence.data(),
                       static_cast<Uint32>(silence.size() * sizeof(int16_t)));
    }
    // Wherever the queue now stands is where the low-water mark starts, on
    // every path through here -- there is no case where a stream begins and
    // the mark should still be carrying a reading from an older one.
    low_water_.reset(low_water_window_frames_, queued_frames());
}

void AudioOutput::update_cover(uint64_t now_ms, std::size_t queued, std::size_t frames) {
    const double rate = spec_.freq > 0 ? static_cast<double>(spec_.freq) : 48000.0;
    if (queued == 0 && last_push_ms_ != 0 &&
        now_ms - last_push_ms_ < static_cast<uint64_t>(kAudioStreamStoppedMs)) {
        // How long it ran empty is how much deeper the queue is about to be
        // once whatever was held back lands -- keep that rather than trim it.
        const auto since = static_cast<std::size_t>(
            static_cast<double>(now_ms - last_push_ms_) * rate / 1000.0);
        const std::size_t dry = audio_dry_spell_frames(since, queued_after_last_push_);
        cover_frames_ = audio_cover_after_running_dry(cover_frames_, dry, cover_step_frames_,
                                                      cover_ceiling_frames_);
        last_dry_ms_ = now_ms;
        last_given_back_ms_ = now_ms;
    } else if (cover_frames_ > 0 && last_dry_ms_ != 0 &&
               now_ms - last_dry_ms_ >= static_cast<uint64_t>(kAudioCalmMsBeforeGivingBack) &&
               now_ms - last_given_back_ms_ >= 1000) {
        const auto step = static_cast<std::size_t>(rate * kAudioCoverGivenBackPerSecondMs / 1000.0);
        cover_frames_ -= std::min(cover_frames_, step);
        last_given_back_ms_ = now_ms;
    }
    last_push_ms_ = now_ms;
    // What this push leaves waiting, near enough: a trim or the rate takes a
    // few frames off it, which only makes the next dry spell read a little
    // long -- the safe direction for cover to be wrong in.
    queued_after_last_push_ = queued + frames;
}

void AudioOutput::reset_stretching() {
    source_speed_.reset();
    stretcher_.reset(sample_rate_);
    stretching_ = false;
    stretch_ = 1.0;
}

const int16_t* AudioOutput::stretch_if_slow(const int16_t* interleaved_stereo, std::size_t frames,
                                            uint64_t now_ms, std::size_t queued) {
    source_speed_.note(now_ms, frames);
    const double speed = source_speed_.speed(sample_rate_);
    if (!stretching_ && speed < kAudioStretchBelowSpeed) {
        stretching_ = true;
        log_debug("audio: arriving at " + std::to_string(static_cast<int>(speed * 100.0 + 0.5)) +
                  "% of real time, so it is being slowed down with the game");
    } else if (stretching_ && speed > kAudioStretchUntilSpeed) {
        stretching_ = false;
        log_debug("audio: back up to real time");
    }

    if (stretching_) {
        stretch_ = audio_stretch_for(speed, queued, aimed_frames());
        stats_stretch_worst_ = std::max(stats_stretch_worst_, stretch_);
        stretched_.clear();
        stretcher_.process(interleaved_stereo, frames, stretch_, stretched_);
        return stretched_.data();
    }
    stretch_ = 1.0;
    if (stretcher_.holding()) {
        // What the stretcher held back goes out first, carrying straight on
        // from where it left off, and then this block as it is.
        stretched_.clear();
        stretcher_.finish(stretched_);
        stretched_.insert(stretched_.end(), interleaved_stereo, interleaved_stereo + frames * 2);
        return stretched_.data();
    }
    return interleaved_stereo;
}

void AudioOutput::push(const int16_t* interleaved_stereo, std::size_t frames) {
    if (device_ == 0 || interleaved_stereo == nullptr || frames == 0) {
        return;
    }

    if (!primed_) {
        prime();
    }

    if (queued_frames() > std::max(panic_frames_, aimed_frames() + panic_margin_frames_)) {
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
        reset_stretching();
        prime();
    }

    const std::size_t queued = queued_frames();
    const uint64_t now_ms = steady_ms();
    // Read before update_cover() moves last_push_ms_ on: a queue found empty
    // after an earlier push is one the device ran out of, and what follows it
    // is fading back in from silence. The very first push finds it empty too,
    // and that is a start rather than a stop.
    const bool resuming_from_dry = queued == 0 && last_push_ms_ != 0;
    update_cover(now_ms, queued, frames);
    rate_ratio_ = audio_drift_ratio(queued, aimed_frames(), rate_ratio_);

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
    stats_frames_in_ += frames;
    const int16_t* block = stretch_if_slow(interleaved_stereo, frames, now_ms, queued);
    const std::size_t block_frames = block == interleaved_stereo ? frames : stretched_.size() / 2;
    const std::size_t sustained = low_water_.note(queued, block_frames);
    // Not while stretching, which holds the queue at its target by itself --
    // and a queue above it there is one the stretch is already easing off.
    const std::size_t skip =
        stretching_ ? 0
                    : audio_trim_frames(sustained, aimed_frames(), trim_margin_frames_,
                                        block_frames);

    // What the queue is actually doing, once a second. Sound going wrong is
    // otherwise the one thing in this app with no numbers at all behind it --
    // "it does not sound right" and a log that says nothing. These four say
    // which of the three ways it can go wrong is happening: running dry
    // (empty, and the rate bent up to catch back up), running late (queued far
    // above the target, and bent down), or being cut off outright (the panic
    // above, which is audible as a jump).
    stats_queue_total_ += queued;
    ++stats_pushes_;
    stats_queue_worst_ = std::max(stats_queue_worst_, queued);
    stats_queue_lowest_ = std::min(stats_queue_lowest_, queued);
    if (queued == 0) {
        ++stats_empty_;
    }

    spliced_.clear();
    const std::size_t skipped = audio_splice(block, block_frames, skip, spliced_);
    if (skipped > 0) {
        ++stats_trims_;
        stats_trimmed_frames_ += skipped;
    }
    if (spliced_.empty()) {
        return;
    }
    if (resuming_from_dry) {
        const std::size_t fade = std::min(kAudioResumeFadeFrames, spliced_.size() / 2);
        for (std::size_t frame = 0; frame < fade; ++frame) {
            const double gain = static_cast<double>(frame + 1) / static_cast<double>(fade + 1);
            for (int channel = 0; channel < 2; ++channel) {
                int16_t& sample = spliced_[frame * 2 + channel];
                sample = static_cast<int16_t>(std::lround(sample * gain));
            }
        }
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
