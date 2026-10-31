#include "audio/ui_sounds.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

#include "render/embedded_assets.hpp"
#include "util/log.hpp"

#ifdef __SWITCH__
#include <malloc.h>
#include <switch.h>

#include <atomic>
#include <mutex>
#else
#include <SDL.h>
#endif

namespace ds {
namespace {

constexpr const char* kNames[kUiSoundCount] = {"move_cursor", "confirm", "cancel", "error", "open_quick_menu",
                                                 "save_state", "toggle_on", "toggle_off", "bump"};

uint32_t le32(const uint8_t* at) {
    return uint32_t(at[0]) | uint32_t(at[1]) << 8 | uint32_t(at[2]) << 16 | uint32_t(at[3]) << 24;
}

// How long a press may wait for what it changed to show -- see UiSoundJudge.
constexpr double kPressCountsFor = 0.5;
// A direction that moved nothing coming in again sooner than this after the
// last is one held down, repeating.
constexpr double kHeldRepeatWithin = 0.15;

}  // namespace

const char* ui_sound_name(UiSound sound) { return kNames[static_cast<int>(sound)]; }

bool decode_ui_sound_pack(const uint8_t* data, std::size_t size, UiSoundPack& out, std::string* error,
                          const std::string& style) {
    out = UiSoundPack();
    const auto fail = [&](const std::string& why) {
        if (error != nullptr) *error = why;
        return false;
    };
    if (data == nullptr || size < 16 || std::memcmp(data, "DSUS", 4) != 0) return fail("not a sound pack");
    const uint32_t version = le32(data + 4);
    if (version != 1 && version != 2) return fail("a sound pack of another version");
    out.rate = static_cast<int>(le32(data + 8));
    const uint32_t count = le32(data + 12);
    std::size_t at = 16;
    // Every entry's style, sound and length, before choosing which style.
    struct Entry {
        std::string style;
        int sound = -1;
        uint32_t frames = 0;
    };
    std::vector<Entry> entries;
    std::vector<std::string> styles;
    for (uint32_t i = 0; i < count; ++i) {
        if (at >= size) return fail("the sound pack is cut short");
        const std::size_t length = data[at++];
        if (at + length + 4 > size) return fail("the sound pack is cut short");
        std::string name(reinterpret_cast<const char*>(data + at), length);
        at += length;
        Entry entry;
        entry.frames = le32(data + at);
        at += 4;
        const std::size_t slash = name.find('/');
        if (slash != std::string::npos) {
            entry.style = name.substr(0, slash);
            name = name.substr(slash + 1);
        }
        for (int s = 0; s < kUiSoundCount; ++s) {
            if (name == kNames[s]) entry.sound = s;
        }
        if (std::find(styles.begin(), styles.end(), entry.style) == styles.end()) styles.push_back(entry.style);
        entries.push_back(entry);
    }
    const std::string chosen =
        std::find(styles.begin(), styles.end(), style) != styles.end() ? style
        : styles.empty()                                               ? std::string()
                                                                       : styles.front();
    std::vector<std::pair<int, uint32_t>> order;  // sound, frames
    for (const Entry& entry : entries) {
        order.push_back({entry.style == chosen ? entry.sound : -1, entry.frames});
    }
    for (const auto& [sound, frames] : order) {
        const std::size_t bytes = std::size_t(frames) * 4;
        if (at + bytes > size) return fail("the sound pack is cut short");
        if (sound >= 0) {
            std::vector<int16_t>& clip = out.clips[static_cast<std::size_t>(sound)];
            clip.resize(std::size_t(frames) * 2);
            for (std::size_t i = 0; i < clip.size(); ++i) {
                clip[i] = static_cast<int16_t>(uint16_t(data[at + i * 2]) | uint16_t(data[at + i * 2 + 1]) << 8);
            }
        }
        at += bytes;
    }
    for (int s = 0; s < kUiSoundCount; ++s) {
        if (out.clips[static_cast<std::size_t>(s)].empty()) return fail(std::string("no ") + kNames[s] + " in the pack");
    }
    return true;
}

int16_t UiSoundMixer::limit(int32_t sample) {
    // Untouched up to three quarters of full scale; above that, bent smoothly
    // towards it rather than cut off flat, which is what made two sounds at
    // once crackle.
    constexpr float knee = 24576.0f;
    constexpr float room = 32767.0f - knee;
    const float magnitude = std::fabs(static_cast<float>(sample));
    if (magnitude <= knee) return static_cast<int16_t>(sample);
    const float bent = knee + room * std::tanh((magnitude - knee) / room);
    return static_cast<int16_t>(sample < 0 ? -bent : bent);
}

void UiSoundMixer::start(UiSound sound) {
    if (pack_ == nullptr) return;
    const int wanted = static_cast<int>(sound);
    Voice* free = nullptr;
    for (Voice& voice : voices_) {
        if (sound == UiSound::MoveCursor && voice.sound == wanted) voice.sound = -1;
        if (voice.sound < 0 && free == nullptr) free = &voice;
    }
    if (free == nullptr) {
        free = &*std::max_element(voices_.begin(), voices_.end(),
                                  [](const Voice& a, const Voice& b) { return a.at < b.at; });
    }
    free->sound = wanted;
    free->at = 0;
}

void UiSoundMixer::stop_all() {
    for (Voice& voice : voices_) voice.sound = -1;
}

void UiSoundMixer::mix(int16_t* out, std::size_t frames) {
    std::vector<int32_t>& sum = sum_;
    sum.assign(frames * 2, 0);
    for (Voice& voice : voices_) {
        if (voice.sound < 0 || pack_ == nullptr) continue;
        const std::vector<int16_t>& clip = pack_->clips[static_cast<std::size_t>(voice.sound)];
        const std::size_t clip_frames = clip.size() / 2;
        const std::size_t n = std::min(frames, clip_frames - std::min(clip_frames, voice.at));
        const int16_t* from = clip.data() + voice.at * 2;
        for (std::size_t i = 0; i < n * 2; ++i) sum[i] += from[i];
        voice.at += n;
        if (voice.at >= clip_frames) voice.sound = -1;
    }
    for (std::size_t i = 0; i < frames * 2; ++i) out[i] = limit(sum[i]);
}

std::size_t UiSoundMixer::playing() const {
    return static_cast<std::size_t>(
        std::count_if(voices_.begin(), voices_.end(), [](const Voice& voice) { return voice.sound >= 0; }));
}

void UiSoundJudge::note_input(const std::vector<UiAction>& actions, bool tapped, double now_seconds,
                              bool repeating) {
    // A game's own buttons: A and B mean nothing to the shell while it is
    // playing, and opening the menu has a sound of its own.
    if (have_view_ && last_.in_game) return;
    Pending wanted = Pending::None;
    std::optional<UiAction> direction;
    const auto rank = [](Pending pending) {
        switch (pending) {
            case Pending::Confirm: return 4;
            case Pending::Cancel: return 3;
            case Pending::Tap: return 2;
            case Pending::Move: return 1;
            case Pending::None: break;
        }
        return 0;
    };
    const auto offer = [&](Pending pending) {
        if (rank(pending) > rank(wanted)) wanted = pending;
    };
    for (const UiAction action : actions) {
        switch (action) {
            case UiAction::Up:
            case UiAction::Down:
            case UiAction::Left:
            case UiAction::Right:
                direction = action;
                offer(Pending::Move);
                break;
            case UiAction::PageUp:
            case UiAction::PageDown: offer(Pending::Move); break;
            case UiAction::Confirm:
            case UiAction::MenuQuit:
            case UiAction::MenuReset:
            case UiAction::MenuDisconnect:
            case UiAction::MenuSettings:
            case UiAction::MenuMinus: offer(Pending::Confirm); break;
            case UiAction::Cancel:
            case UiAction::ToggleMenu: offer(Pending::Cancel); break;
            default: break;
        }
    }
    if (tapped) offer(Pending::Tap);
    if (wanted != Pending::None) {
        pending_ = wanted;
        pending_at_ = now_seconds;
        pressed_direction_ = wanted == Pending::Move ? direction : std::nullopt;
        pressed_repeating_ = repeating;
    }
}

std::optional<UiSound> UiSoundJudge::observe(const UiSoundView& view, double now_seconds) {
    if (!have_view_) {
        have_view_ = true;
        last_ = view;
        return std::nullopt;
    }
    const UiSoundView before = last_;
    last_ = view;
    if (pending_ != Pending::None && now_seconds - pending_at_ > kPressCountsFor) pending_ = Pending::None;

    // Looked at once, on the view just after the press.
    const std::optional<UiAction> pressed = pressed_direction_;
    pressed_direction_.reset();

    std::optional<UiSound> sound;
    if (view.error && !before.error) {
        sound = UiSound::Error;
    } else if (view.saves != before.saves) {
        sound = UiSound::SaveState;
    } else if (before.in_game && view.menu) {
        sound = UiSound::OpenQuickMenu;
    } else if (before.menu && view.in_game) {
        // Closing has no sound of its own: it is backing out, however it was
        // done and whoever did it.
        sound = UiSound::Cancel;
    } else if (pending_ != Pending::None) {
        const bool moved = view.focus != before.focus;
        const bool changed = view.screen != before.screen || view.rest != before.rest;
        // The switch under the cursor flipped, the cursor where it was, by
        // what flips one: A or a tap, never a direction.
        const bool flipped = (pending_ == Pending::Confirm || pending_ == Pending::Tap) && !moved &&
                             view.screen == before.screen &&
                             before.toggle >= 0 && view.toggle >= 0 && before.toggle != view.toggle;
        if (flipped) {
            sound = view.toggle != 0 ? UiSound::ToggleOn : UiSound::ToggleOff;
        } else if (pending_ == Pending::Move && pressed && !moved && !changed && !view.in_game &&
                   view.ring) {
            // Against the end: nothing moved, the ring still there.
            const bool held = pressed_repeating_ || (blocked_direction_ == pressed &&
                                                     now_seconds - blocked_at_ < kHeldRepeatWithin);
            blocked_direction_ = pressed;
            blocked_at_ = now_seconds;
            if (!held) {
                sound = UiSound::Bump;
                bump_direction_ = *pressed;
            }
            pending_ = Pending::None;
        } else if (moved || changed) {
            switch (pending_) {
                case Pending::Move: sound = UiSound::MoveCursor; break;
                // A popup's Cancel, chosen with A, is backing out all the same.
                case Pending::Confirm: sound = before.on_cancel ? UiSound::Cancel : UiSound::Confirm; break;
                case Pending::Cancel: sound = UiSound::Cancel; break;
                // A first tap on a row moves the cursor to it; a tap that does
                // anything more is a choice.
                case Pending::Tap:
                    sound = !changed ? UiSound::MoveCursor : before.on_cancel ? UiSound::Cancel : UiSound::Confirm;
                    break;
                case Pending::None: break;
            }
        }
    }
    if (sound) pending_ = Pending::None;
    if (sound && *sound != UiSound::Bump) blocked_direction_.reset();
    return sound;
}

#ifdef __SWITCH__

// The console's plain audio output, which is not what SDL plays a game's sound
// through: SDL's Switch audio drives the audio renderer, and there is only one
// renderer to a program -- a second driver on it would trample the first's
// voices on every update. This has a session of its own, which the system
// mixes with the renderer's.
//
// Fed from a thread of its own, four buffers of 240 frames kept queued -- the
// 5 ms the system mixes in at a time, so 20 ms in hand. The thread runs just
// above the app's own threads, on the program's default core: run beside
// them at the same priority, a frame that kept the main thread busy -- the
// quick menu drawing every layout's preview -- left the queue to run dry, and
// what came out was a sound with holes in it.
struct UiSoundPlayer::Backend {
    static constexpr std::size_t kFrames = 240;
    static constexpr std::size_t kBufferBytes = 0x1000;  // what the service wants them aligned to
    static constexpr int kBuffers = 4;
    // Lower is sooner; the main thread and those it starts are 0x2C.
    static constexpr int kFeederPriority = 0x2B;
    // How long the feeder may go without handing over a buffer before the
    // output is taken to have stopped: it hands one over every 5 ms.
    static constexpr u64 kStalledNs = 250000000ull;
    UiSoundMixer mixer;
    std::mutex mutex;
    std::atomic<bool> stopping{false};
    // When the feeder last handed over a buffer, in system ticks.
    std::atomic<u64> fed_at{0};
    const UiSoundPack* pack_ref = nullptr;
    u64 restart_tried_at = 0;
    Thread feeder{};
    bool feeding = false;
    bool service = false;
    bool started = false;
    std::array<AudioOutBuffer, kBuffers> buffers{};

    bool open(const UiSoundPack& pack, std::string* error) {
        const auto fail = [&](const std::string& why, Result rc) {
            if (error != nullptr) *error = why + " (" + std::to_string(rc) + ")";
            close();
            return false;
        };
        pack_ref = &pack;
        stopping.store(false);
        fed_at.store(armGetSystemTick());
        Result rc = audoutInitialize();
        if (R_FAILED(rc)) return fail("the console's audio output would not open", rc);
        service = true;
        if (audoutGetSampleRate() != 48000 || audoutGetChannelCount() != 2 ||
            audoutGetPcmFormat() != PcmFormat_Int16) {
            return fail("the console's audio output is not 48 kHz 16-bit stereo", 0);
        }
        mixer.set_pack(&pack);
        for (AudioOutBuffer& buffer : buffers) {
            buffer = AudioOutBuffer{};
            buffer.buffer = memalign(kBufferBytes, kBufferBytes);
            if (buffer.buffer == nullptr) return fail("no memory for the menu sounds", 0);
            std::memset(buffer.buffer, 0, kBufferBytes);
            armDCacheFlush(buffer.buffer, kBufferBytes);
            buffer.buffer_size = kBufferBytes;
            buffer.data_size = kFrames * 4;
        }
        rc = audoutStartAudioOut();
        if (R_FAILED(rc)) return fail("the console's audio output would not start", rc);
        started = true;
        for (AudioOutBuffer& buffer : buffers) {
            rc = audoutAppendAudioOutBuffer(&buffer);
            if (R_FAILED(rc)) return fail("the console's audio output would not take a buffer", rc);
        }
        rc = threadCreate(&feeder, &Backend::feed_entry, this, nullptr, 0x8000, kFeederPriority, -2);
        if (R_FAILED(rc)) return fail("the menu sounds' thread would not start", rc);
        rc = threadStart(&feeder);
        if (R_FAILED(rc)) {
            threadClose(&feeder);
            return fail("the menu sounds' thread would not start", rc);
        }
        feeding = true;
        return true;
    }

    static void feed_entry(void* self) { static_cast<Backend*>(self)->feed(); }

    void feed() {
        while (!stopping.load()) {
            AudioOutBuffer* released = nullptr;
            u32 count = 0;
            // A tenth of a second at most, so close() is never kept waiting.
            const Result rc = audoutWaitPlayFinish(&released, &count, 100000000ull);
            if (R_FAILED(rc) || count == 0 || released == nullptr) continue;
            {
                std::lock_guard<std::mutex> lock(mutex);
                mixer.mix(static_cast<int16_t*>(released->buffer), kFrames);
            }
            armDCacheFlush(released->buffer, kBufferBytes);
            if (R_SUCCEEDED(audoutAppendAudioOutBuffer(released))) fed_at.store(armGetSystemTick());
        }
    }

    // The output can stop under a game -- buffers it never gives back, a
    // session the system has ended -- and nothing says so: the game's own
    // sound goes another way and carries on, and the menus simply went quiet
    // for the rest of the session. A sound wanted from an output that has not
    // taken a buffer for a quarter of a second starts it again first, at most
    // once a second.
    bool restart_if_stalled() {
        const u64 now = armGetSystemTick();
        if (started && armTicksToNs(now - fed_at.load()) < kStalledNs) return true;
        if (pack_ref == nullptr || (restart_tried_at != 0 && armTicksToNs(now - restart_tried_at) < 1000000000ull)) {
            return started;
        }
        restart_tried_at = now;
        const UiSoundPack* again = pack_ref;
        close();
        mixer.stop_all();
        std::string error;
        if (!open(*again, &error)) {
            log_warn("the menu sounds stopped, and would not start again: " + error);
            return false;
        }
        log_info("the menu sounds had stopped, so they were started again");
        return true;
    }

    void play(UiSound sound) {
        if (!restart_if_stalled()) return;
        std::lock_guard<std::mutex> lock(mutex);
        mixer.start(sound);
    }

    void close() {
        if (feeding) {
            stopping.store(true);
            threadWaitForExit(&feeder);
            threadClose(&feeder);
            feeding = false;
        }
        if (started) {
            audoutStopAudioOut();
            started = false;
        }
        if (service) {
            audoutExit();
            service = false;
        }
        for (AudioOutBuffer& buffer : buffers) {
            std::free(buffer.buffer);
            buffer.buffer = nullptr;
        }
    }
};

#else

// A device of its own, pulled by a callback that mixes whatever is playing:
// 256 frames at 48 kHz is about 5 ms between a press and its sound starting.
struct UiSoundPlayer::Backend {
    SDL_AudioDeviceID device = 0;
    UiSoundMixer mixer;
    bool subsystem = false;
    bool said_lost = false;  // that it went away and no other would open

    static void fill(void* self, Uint8* stream, int length) {
        auto* backend = static_cast<Backend*>(self);
        backend->mixer.mix(reinterpret_cast<int16_t*>(stream), static_cast<std::size_t>(length) / 4);
    }

    bool open(const UiSoundPack& pack, std::string* error) {
#ifdef __ANDROID__
        // SDL plays one device at a time there, and the game's sound needs it.
        (void)pack;
        if (error != nullptr) *error = "SDL on Android has only the one audio device, which a game's sound needs";
        return false;
#endif
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
            if (error != nullptr) *error = std::string("no SDL audio: ") + SDL_GetError();
            return false;
        }
        subsystem = true;
        mixer.set_pack(&pack);
        const SDL_AudioSpec desired = spec();
        device = open_device();
        if (device == 0) {
            if (error != nullptr) *error = std::string("no audio device for the menu sounds: ") + SDL_GetError();
            close();
            return false;
        }
        // Some of SDL's drivers -- a phone's, the dummy one -- open one device
        // at a time, and this one was opened first: kept, it would be the
        // game's sound that found the device taken. Asking for a second one
        // is how to tell.
        SDL_AudioSpec probe_spec = desired;
        probe_spec.callback = nullptr;
        const SDL_AudioDeviceID probe = SDL_OpenAudioDevice(nullptr, 0, &probe_spec, nullptr, 0);
        if (probe == 0) {
            if (error != nullptr) {
                *error = std::string("the ") + SDL_GetCurrentAudioDriver() +
                         " audio driver plays one device at a time, and the game's sound needs it";
            }
            close();
            return false;
        }
        SDL_CloseAudioDevice(probe);
        SDL_PauseAudioDevice(device, 0);
        return true;
    }

    SDL_AudioSpec spec() {
        SDL_AudioSpec desired{};
        desired.freq = 48000;
        desired.format = AUDIO_S16SYS;
        desired.channels = 2;
        desired.samples = 256;
        desired.callback = &Backend::fill;
        desired.userdata = this;
        return desired;
    }

    // No changes allowed: SDL converts to whatever the device really is.
    SDL_AudioDeviceID open_device() {
        const SDL_AudioSpec desired = spec();
        SDL_AudioSpec obtained{};
        return SDL_OpenAudioDevice(nullptr, 0, &desired, &obtained, 0);
    }

    // A device can go away under a program that has it open -- the output
    // changed, a dock or headphones unplugged, the sound server restarted
    // across a sleep -- and SDL then never calls back again. The game's own
    // device is opened afresh with each game and carried on; the menus went
    // quiet for the rest of the session. A sound wanted from a device that has
    // stopped opens the default one again first.
    bool reopen_if_lost() {
        if (device != 0 && SDL_GetAudioDeviceStatus(device) != SDL_AUDIO_STOPPED) return true;
        if (device != 0) {
            SDL_CloseAudioDevice(device);
            device = 0;
            mixer.stop_all();
        }
        device = open_device();
        if (device == 0) {
            if (!said_lost) {
                log_warn(std::string("the menu sounds' audio device went away, and no other would open: ") +
                         SDL_GetError());
                said_lost = true;
            }
            return false;
        }
        said_lost = false;
        SDL_PauseAudioDevice(device, 0);
        log_info("the menu sounds' audio device went away, so they play on the one there is now");
        return true;
    }

    void play(UiSound sound) {
        if (!reopen_if_lost()) return;
        SDL_LockAudioDevice(device);
        mixer.start(sound);
        SDL_UnlockAudioDevice(device);
    }

    void close() {
        if (device != 0) {
            SDL_CloseAudioDevice(device);
            device = 0;
        }
        if (subsystem) {
            SDL_QuitSubSystem(SDL_INIT_AUDIO);
            subsystem = false;
        }
    }
};

#endif

UiSoundPlayer::UiSoundPlayer() = default;

UiSoundPlayer::~UiSoundPlayer() { close(); }

bool UiSoundPlayer::open(std::string* error) {
    close();
    if (!decode_ui_sound_pack(assets::kUiSounds, assets::kUiSounds_size, pack_, error)) return false;
    auto backend = std::make_unique<Backend>();
    if (!backend->open(pack_, error)) return false;
    backend_ = std::move(backend);
    return true;
}

void UiSoundPlayer::close() {
    if (backend_) {
        backend_->close();
        backend_.reset();
    }
}

bool UiSoundPlayer::is_open() const { return backend_ != nullptr; }

void UiSoundPlayer::play(UiSound sound) {
    if (!enabled_) return;
    log_debug(std::string("menu sound: ") + ui_sound_name(sound) + (backend_ ? "" : " (no device to play it on)"));
    if (backend_) backend_->play(sound);
}

}  // namespace ds
