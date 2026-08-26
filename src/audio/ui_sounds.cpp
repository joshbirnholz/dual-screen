#include "audio/ui_sounds.hpp"

#include <algorithm>
#include <cstring>

#include "render/embedded_assets.hpp"
#include "util/log.hpp"

namespace ds {
namespace {

// The rate every effect was resampled to when it was packed, so nothing has to
// be resampled while it is playing.
constexpr int kSampleRate = 48000;
// How many can sound at once. A menu makes at most a couple -- a cursor move
// under a confirmation, say -- and a cap means a stuck caller cannot grow this
// without bound.
constexpr std::size_t kMaxVoices = 8;

uint32_t read_u32(const uint8_t* data) {
    return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
           (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
}

}  // namespace

bool decode_ui_sound_pack(const uint8_t* data,
                          std::size_t size,
                          std::vector<const int16_t*>& out_samples,
                          std::vector<std::size_t>& out_frames) {
    out_samples.clear();
    out_frames.clear();
    if (data == nullptr || size < 8 || std::memcmp(data, "DSFX", 4) != 0) {
        return false;
    }
    const uint32_t count = read_u32(data + 4);
    if (count != static_cast<uint32_t>(UiSound::Count)) {
        return false;
    }
    const std::size_t table = 8;
    const std::size_t body = table + static_cast<std::size_t>(count) * 8;
    if (size < body) {
        return false;
    }
    for (uint32_t index = 0; index < count; ++index) {
        const uint32_t offset = read_u32(data + table + index * 8);
        const uint32_t frames = read_u32(data + table + index * 8 + 4);
        const std::size_t end = body + (static_cast<std::size_t>(offset) + frames) * 2;
        if (end > size) {
            return false;
        }
        out_samples.push_back(
            reinterpret_cast<const int16_t*>(data + body + static_cast<std::size_t>(offset) * 2));
        out_frames.push_back(frames);
    }
    return true;
}

UiSounds::~UiSounds() { shutdown(); }

bool UiSounds::initialize(std::string* error) {
    std::vector<const int16_t*> samples;
    std::vector<std::size_t> frames;
    if (!decode_ui_sound_pack(assets::kUiSoundsBin, assets::kUiSoundsBin_size, samples, frames)) {
        if (error != nullptr) *error = "the packed interface sounds could not be read";
        return false;
    }
    clips_.clear();
    for (std::size_t index = 0; index < samples.size(); ++index) {
        clips_.push_back(Clip{samples[index], frames[index]});
    }

    SDL_AudioSpec want {};
    want.freq = kSampleRate;
    want.format = AUDIO_S16SYS;
    want.channels = 1;
    want.samples = 512;
    want.callback = &UiSounds::fill;
    want.userdata = this;
    SDL_AudioSpec have {};
    device_ = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (device_ == 0) {
        // Not fatal: a machine with no sound card still plays games.
        if (error != nullptr) *error = SDL_GetError();
        log_warn(std::string("no device for the interface sounds: ") + SDL_GetError());
        return false;
    }
    SDL_PauseAudioDevice(device_, 0);
    return true;
}

void UiSounds::shutdown() {
    if (device_ != 0) {
        SDL_CloseAudioDevice(device_);
        device_ = 0;
    }
    voices_.clear();
    clips_.clear();
}

void UiSounds::play(UiSound sound) {
    const int id = static_cast<int>(sound);
    if (device_ == 0 || id < 0 || static_cast<std::size_t>(id) >= clips_.size()) {
        return;
    }
    SDL_LockAudioDevice(device_);
    for (Voice& voice : voices_) {
        if (voice.sound == id && !voice.looping) {
            voice.position = 0;  // restarted rather than stacked
            SDL_UnlockAudioDevice(device_);
            return;
        }
    }
    if (voices_.size() < kMaxVoices) {
        voices_.push_back(Voice{id, 0, false});
    }
    SDL_UnlockAudioDevice(device_);
}

void UiSounds::start_loop(UiSound sound) {
    const int id = static_cast<int>(sound);
    if (device_ == 0 || id < 0 || static_cast<std::size_t>(id) >= clips_.size()) {
        return;
    }
    SDL_LockAudioDevice(device_);
    for (const Voice& voice : voices_) {
        if (voice.looping && voice.sound == id) {
            SDL_UnlockAudioDevice(device_);
            return;  // already going; starting it again would restart it
        }
    }
    voices_.erase(std::remove_if(voices_.begin(), voices_.end(),
                                 [](const Voice& voice) { return voice.looping; }),
                  voices_.end());
    voices_.push_back(Voice{id, 0, true});
    SDL_UnlockAudioDevice(device_);
}

void UiSounds::stop_loop() {
    if (device_ == 0) {
        return;
    }
    SDL_LockAudioDevice(device_);
    voices_.erase(std::remove_if(voices_.begin(), voices_.end(),
                                 [](const Voice& voice) { return voice.looping; }),
                  voices_.end());
    SDL_UnlockAudioDevice(device_);
}

void SDLCALL UiSounds::fill(void* opaque, uint8_t* stream, int bytes) {
    auto* sounds = static_cast<UiSounds*>(opaque);
    std::memset(stream, 0, static_cast<std::size_t>(bytes));
    sounds->mix(reinterpret_cast<int16_t*>(stream), static_cast<std::size_t>(bytes) / 2);
}

void UiSounds::mix(int16_t* out, std::size_t frames) {
    for (std::size_t index = 0; index < voices_.size();) {
        Voice& voice = voices_[index];
        const Clip& clip = clips_[static_cast<std::size_t>(voice.sound)];
        std::size_t written = 0;
        while (written < frames) {
            if (voice.position >= clip.frames) {
                if (!voice.looping) break;
                voice.position = 0;
            }
            const std::size_t run =
                std::min(frames - written, clip.frames - voice.position);
            for (std::size_t step = 0; step < run; ++step) {
                const int mixed = out[written + step] + clip.samples[voice.position + step];
                out[written + step] = static_cast<int16_t>(std::max(-32768, std::min(32767, mixed)));
            }
            written += run;
            voice.position += run;
        }
        if (!voice.looping && voice.position >= clip.frames) {
            voices_.erase(voices_.begin() + static_cast<std::ptrdiff_t>(index));
        } else {
            ++index;
        }
    }
}

}  // namespace ds
