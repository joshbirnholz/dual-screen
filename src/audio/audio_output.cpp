#include "audio/audio_output.hpp"

#include <algorithm>
#include <vector>

#include "audio/ui_sounds.hpp"
#include "util/log.hpp"

namespace ds {
namespace {

constexpr int kBytesPerFrame = 4;  // 2 channels x int16

}  // namespace

AudioOutput::~AudioOutput() { close(); }

void AudioOutput::set_effects(UiSounds* effects) { effects_ = effects; }

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

    // Asked for before opening: on a platform with one device the effects are
    // holding it, and the game's sound has the better claim.
    if (effects_ != nullptr) {
        effects_->release_device();
    }

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

    // Cap latency at roughly a quarter second of audio.
    high_watermark_frames_ = static_cast<std::size_t>(spec_.freq / 4);

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
        // Handed back, so a launcher with no game running still makes sounds.
        if (effects_ != nullptr) {
            effects_->take_device();
        }
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
}

std::size_t AudioOutput::queued_frames() const {
    if (device_ == 0) return 0;
    return SDL_GetQueuedAudioSize(device_) / kBytesPerFrame;
}

void AudioOutput::push(const int16_t* interleaved_stereo, std::size_t frames) {
    if (device_ == 0 || interleaved_stereo == nullptr || frames == 0) {
        return;
    }

    // Drop audio rather than let the queue grow without bound; a deep queue is
    // audible as lag between what you press and what you hear.
    if (queued_frames() > high_watermark_frames_) {
        return;
    }

    // The effects ride the game's stream rather than a second device, and are
    // mixed in at this rate: whatever resampling this stream needs, they need
    // the same.
    const int16_t* source = interleaved_stereo;
    static thread_local std::vector<int16_t> mixed;
    if (effects_ != nullptr && effects_->sounding()) {
        mixed.assign(interleaved_stereo, interleaved_stereo + frames * 2);
        effects_->render_into(mixed.data(), frames, static_cast<int>(sample_rate_ + 0.5), 2);
        source = mixed.data();
    }

    queue(source, frames);
}

void AudioOutput::queue(const int16_t* interleaved_stereo, std::size_t frames) {
    const int byte_count = static_cast<int>(frames * kBytesPerFrame);
    if (stream_ != nullptr) {
        if (SDL_AudioStreamPut(stream_, interleaved_stereo, byte_count) != 0) {
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
    SDL_QueueAudio(device_, interleaved_stereo, static_cast<Uint32>(byte_count));
}

void AudioOutput::pump_effects() {
    if (device_ == 0 || effects_ == nullptr || !effects_->sounding()) {
        return;
    }
    // Only while the game is not filling the queue itself. A game that is
    // running keeps it well above this, so this does nothing until one pauses.
    const std::size_t rate = static_cast<std::size_t>(sample_rate_ + 0.5);
    const std::size_t chunk = std::max<std::size_t>(1, rate / 30);
    if (queued_frames() > chunk) {
        return;
    }
    static thread_local std::vector<int16_t> silence;
    silence.assign(chunk * 2, 0);
    effects_->render_into(silence.data(), chunk, static_cast<int>(rate), 2);
    // Queued rather than pushed: it has been mixed already, and push() would
    // mix the same effects into it a second time.
    queue(silence.data(), chunk);
}

}  // namespace ds
