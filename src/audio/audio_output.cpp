#include "audio/audio_output.hpp"

#include <vector>

#include "util/log.hpp"

namespace ds {
namespace {

constexpr int kBytesPerFrame = 4;  // 2 channels x int16

}  // namespace

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

}  // namespace ds
