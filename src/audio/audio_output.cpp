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
    stats_at_ms_ = now_ms;
    const double rate = sample_rate_ > 0.0 ? sample_rate_ : 48000.0;
    const auto to_ms = [rate](uint64_t frames) {
        return static_cast<uint64_t>(frames * 1000.0 / rate);
    };
    log_debug("audio: queue " + std::to_string(to_ms(stats_queue_total_ / stats_pushes_)) +
              "ms avg " + std::to_string(to_ms(stats_queue_worst_)) + "ms worst against a " +
              std::to_string(to_ms(target_frames_)) + "ms target, " + std::to_string(stats_empty_) +
              " empty, " + std::to_string(stats_panics_) + " cut short, playing at " +
              std::to_string(static_cast<int>((rate_ratio_ - 1.0) * 10000.0)) + " parts per "
              "ten thousand off");
    stats_queue_total_ = 0;
    stats_queue_worst_ = 0;
    stats_pushes_ = 0;
    stats_empty_ = 0;
    stats_panics_ = 0;
}

void AudioOutput::prime() {
    primed_ = true;
    if (target_frames_ == 0) {
        return;
    }
    const std::vector<int16_t> silence(target_frames_ * 2, 0);
    SDL_QueueAudio(device_, silence.data(),
                   static_cast<Uint32>(silence.size() * sizeof(int16_t)));
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
    if (queued == 0) {
        ++stats_empty_;
    }

    scratch_.clear();
    resampler_.resample(interleaved_stereo, frames, rate_ratio_, scratch_);
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
