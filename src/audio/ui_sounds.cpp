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

// Holds SDL's audio lock while a voice list is touched, and does nothing when
// there is no device of our own to lock -- the game's output calls in from its
// own thread, where its own lock is already held.
class Lock {
public:
    explicit Lock(SDL_AudioDeviceID device) : device_(device) {
        if (device_ != 0) SDL_LockAudioDevice(device_);
    }
    ~Lock() {
        if (device_ != 0) SDL_UnlockAudioDevice(device_);
    }
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;

private:
    SDL_AudioDeviceID device_;
};

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
    // A device of their own is a convenience, not a requirement: where one is
    // busy -- or where there is only one and the game wants it -- these are
    // mixed into that instead.
    open_device();
    return true;
}

bool UiSounds::open_device() {
    if (device_ != 0) {
        return true;
    }
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        log_warn(std::string("no audio for the interface sounds: ") + SDL_GetError());
        return false;
    }
    SDL_AudioSpec want {};
    want.freq = kSampleRate;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 512;
    want.callback = &UiSounds::fill;
    want.userdata = this;
    SDL_AudioSpec have {};
    // Whatever rate and channel count the device would rather have. These are
    // resampled and spread on the way out regardless, so taking the device's
    // own shape costs nothing and keeps SDL's conversion out of the path --
    // which is where a console with one fixed output stops making any sound at
    // all, while the game's stream through the same hardware stays audible.
    device_ = SDL_OpenAudioDevice(nullptr, 0, &want, &have,
                                  SDL_AUDIO_ALLOW_FREQUENCY_CHANGE |
                                      SDL_AUDIO_ALLOW_CHANNELS_CHANGE);
    if (device_ == 0) {
        log_info(std::string("the interface sounds will share the game's audio device: ") +
                 SDL_GetError());
        return false;
    }
    device_rate_ = have.freq;
    device_channels_ = have.channels;
    log_info("the interface sounds have a device of their own: " + std::to_string(device_rate_) +
             " Hz, " + std::to_string(device_channels_) + " channels");
    SDL_PauseAudioDevice(device_, 0);
    return true;
}

void UiSounds::release_device() {
    if (device_ == 0) {
        return;
    }
    SDL_CloseAudioDevice(device_);
    device_ = 0;
    device_rate_ = 0;
    device_channels_ = 0;
}

void UiSounds::take_device() { open_device(); }

void UiSounds::shutdown() {
    release_device();
    voices_.clear();
    clips_.clear();
}

void UiSounds::play(UiSound sound) {
    const int id = static_cast<int>(sound);
    if (muted_ || id < 0 || static_cast<std::size_t>(id) >= clips_.size()) {
        return;
    }
    const Lock held(device_);
    for (Voice& voice : voices_) {
        if (voice.sound == id && !voice.looping) {
            voice.position = 0.0;  // restarted rather than stacked
            return;
        }
    }
    if (voices_.size() < kMaxVoices) {
        voices_.push_back(Voice{id, 0.0, false});
    }
}

void UiSounds::start_loop(UiSound sound) {
    const int id = static_cast<int>(sound);
    if (muted_ || id < 0 || static_cast<std::size_t>(id) >= clips_.size()) {
        return;
    }
    const Lock held(device_);
    for (const Voice& voice : voices_) {
        if (voice.looping && voice.sound == id) {
            return;  // already going; starting it again would restart it
        }
    }
    voices_.erase(std::remove_if(voices_.begin(), voices_.end(),
                                 [](const Voice& voice) { return voice.looping; }),
                  voices_.end());
    voices_.push_back(Voice{id, 0.0, true});
}

void UiSounds::stop_loop() {
    const Lock held(device_);
    voices_.erase(std::remove_if(voices_.begin(), voices_.end(),
                                 [](const Voice& voice) { return voice.looping; }),
                  voices_.end());
}

bool UiSounds::sounding() const {
    const Lock held(device_);
    return !voices_.empty();
}

void UiSounds::set_muted(bool muted) {
    if (muted == muted_) {
        return;
    }
    muted_ = muted;
    if (!muted_) {
        return;
    }
    // Whatever is already sounding stops with it, rather than finishing over
    // the top of a game whose sound is coming out of the other device.
    const Lock held(device_);
    voices_.clear();
}

void SDLCALL UiSounds::fill(void* opaque, uint8_t* stream, int bytes) {
    auto* sounds = static_cast<UiSounds*>(opaque);
    std::memset(stream, 0, static_cast<std::size_t>(bytes));
    if (sounds->device_channels_ < 1 || sounds->device_rate_ <= 0) {
        return;
    }
    const std::size_t frame_bytes = sizeof(int16_t) * static_cast<std::size_t>(sounds->device_channels_);
    sounds->render_into(reinterpret_cast<int16_t*>(stream),
                        static_cast<std::size_t>(bytes) / frame_bytes, sounds->device_rate_,
                        sounds->device_channels_);
}

void UiSounds::render_into(int16_t* interleaved,
                           std::size_t frames,
                           int sample_rate,
                           int channels) {
    if (interleaved == nullptr || frames == 0 || channels < 1 || sample_rate <= 0 ||
        voices_.empty()) {
        return;
    }
    // How far through an effect one frame of the destination takes us. The
    // effects are packed at one rate; the buffer they are going into is at
    // whatever the game asked for.
    const double step = static_cast<double>(kSampleRate) / static_cast<double>(sample_rate);

    for (std::size_t index = 0; index < voices_.size();) {
        Voice& voice = voices_[index];
        const Clip& clip = clips_[static_cast<std::size_t>(voice.sound)];
        bool finished = false;
        for (std::size_t frame = 0; frame < frames; ++frame) {
            if (voice.position >= static_cast<double>(clip.frames)) {
                if (!voice.looping) {
                    finished = true;
                    break;
                }
                voice.position -= static_cast<double>(clip.frames);
            }
            const int16_t sample = clip.samples[static_cast<std::size_t>(voice.position)];
            for (int channel = 0; channel < channels; ++channel) {
                int16_t& target = interleaved[frame * static_cast<std::size_t>(channels) + channel];
                const int mixed = target + sample;
                target = static_cast<int16_t>(std::max(-32768, std::min(32767, mixed)));
            }
            voice.position += step;
        }
        if (finished) {
            voices_.erase(voices_.begin() + static_cast<std::ptrdiff_t>(index));
        } else {
            ++index;
        }
    }
}

}  // namespace ds
