// Stands in for Cemu.
//
// The Wii U is played by a program this app does not contain, and that program
// does not exist yet -- so this one plays its part: it connects over the frame
// link, publishes a television picture and a GamePad picture under one serial
// each frame, sends a tone, and answers a reset by starting its scene again.
//
// The same reason tests/mock_core exists. Everything on this side of the link
// can be finished and held to before the emulator on the other side of it is
// ready, and when that emulator is ready the only untested thing left is the
// emulator.
//
// It uses ExternalFrameProducer rather than a copy of the protocol, so what is
// exercised here is the code a fork of Cemu would link, not a description of it.
//
//   fake_wii_u [--socket PATH] [--seconds N] [--fps N] [--pad-only] [--refuse-pause]
//              [--ignore-pause]

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "core/external_frame_link.hpp"
#include "core/system.hpp"
#include "util/log.hpp"

namespace {

// A Wii U's own two screens. Cemu can render above these; this does not, since
// what is being exercised is the handover rather than an upscaler.
constexpr int kTvWidth = 1280;
constexpr int kTvHeight = 720;
constexpr int kPadWidth = 854;
constexpr int kPadHeight = 480;

uint32_t pack(int r, int g, int b) {
    const auto clamp = [](int v) { return static_cast<uint32_t>(v < 0 ? 0 : (v > 255 ? 255 : v)); };
    // Byte order R,G,B,0xff -- what a real Wii U's GPU-rendered screens
    // arrive as (see ExternalScreenSource::screens()'s upload_rgba8888()),
    // not the byte-swapped layout a CPU-rendered libretro core uses.
    return clamp(r) | (clamp(g) << 8) | (clamp(b) << 16) | (0xffu << 24);
}

// Two pictures that are obviously different from each other and obviously
// moving, so a screenshot says which screen it is and whether frames are
// arriving. The television is blue and the GamePad green -- the same convention
// the mock core uses, so the checks that already exist read the same way.
void draw(std::vector<uint32_t>& out, int width, int height, int frame, bool television) {
    out.resize(static_cast<std::size_t>(width) * height);
    const float phase = static_cast<float>(frame) * 0.05f;

    // Per column and per row rather than per pixel. The first version of this
    // called sin() for every pixel of both screens and managed 25 frames a
    // second, which made a rig meant to show a link running at 60 into a
    // measurement of its own test pattern.
    static std::vector<int> wave;
    wave.resize(static_cast<std::size_t>(width));
    for (int x = 0; x < width; ++x) {
        const float fx = static_cast<float>(x) / static_cast<float>(width);
        wave[static_cast<std::size_t>(x)] = static_cast<int>(40.0f * std::sin((fx * 6.0f) + phase));
    }

    for (int y = 0; y < height; ++y) {
        const int down = static_cast<int>(40.0f * static_cast<float>(y) /
                                          static_cast<float>(height));
        uint32_t* row = out.data() + static_cast<std::size_t>(y) * width;
        for (int x = 0; x < width; ++x) {
            const int w = wave[static_cast<std::size_t>(x)];
            const int across = static_cast<int>(40.0f * static_cast<float>(x) /
                                                static_cast<float>(width));
            row[x] = television ? pack(20 + w, 60 + down, 200 + w)
                                : pack(20 + w, 200 + w, 60 + across);
        }
    }
}

const char* button_name(ds::ExternalButton button) {
    switch (button) {
        case ds::ExternalButton::A: return "A";
        case ds::ExternalButton::B: return "B";
        case ds::ExternalButton::X: return "X";
        case ds::ExternalButton::Y: return "Y";
        case ds::ExternalButton::L: return "L";
        case ds::ExternalButton::R: return "R";
        case ds::ExternalButton::Zl: return "ZL";
        case ds::ExternalButton::Zr: return "ZR";
        case ds::ExternalButton::Plus: return "+";
        case ds::ExternalButton::Minus: return "-";
        case ds::ExternalButton::Up: return "Up";
        case ds::ExternalButton::Down: return "Down";
        case ds::ExternalButton::Left: return "Left";
        case ds::ExternalButton::Right: return "Right";
        case ds::ExternalButton::LeftStick: return "L3";
        case ds::ExternalButton::RightStick: return "R3";
    }
    return "?";
}

// What arrived, in words. A real emulator would apply this to the game; there
// is no game here, so saying it is the whole of what this can do with it -- and
// it is what lets a test assert that a press on one machine reached another.
std::string describe(const ds::ExternalInput& input) {
    std::string text;
    for (int index = 0; index <= static_cast<int>(ds::ExternalButton::RightStick); ++index) {
        const auto button = static_cast<ds::ExternalButton>(index);
        if (input.pressed(button)) {
            text += std::string(text.empty() ? "" : " ") + button_name(button);
        }
    }
    if (text.empty()) {
        text = "nothing";
    }
    if (input.left_x != 0 || input.left_y != 0) {
        text += " left(" + std::to_string(input.left_x) + "," + std::to_string(input.left_y) + ")";
    }
    if (input.right_x != 0 || input.right_y != 0) {
        text += " right(" + std::to_string(input.right_x) + "," +
                std::to_string(input.right_y) + ")";
    }
    if (input.touching) {
        text += " touch(" + std::to_string(input.touch_x) + "," +
                std::to_string(input.touch_y) + ")";
    }
    return text;
}

void usage() {
    std::fprintf(stderr,
                 "fake_wii_u [--socket PATH] [--seconds N] [--fps N] [--pad-only] "
                 "[--refuse-pause] [--ignore-pause]\n");
}

}  // namespace

namespace {

// What Cemu would list for a game with the community's packs installed: one
// that needs the game started again, one with two choices, one with none.
std::vector<ds::ExternalGraphicPack> fake_graphic_packs() {
    std::vector<ds::ExternalGraphicPack> packs(3);
    packs[0].id = "graphicPacks/Fake_Resolution/rules.txt";
    packs[0].path = "Fake Game/Graphics";
    packs[0].name = "Graphics";
    packs[0].enabled = true;
    packs[0].restart_needed = true;
    packs[0].choices.push_back({"TV Resolution", {"1280x720", "1920x1080", "2560x1440", "3840x2160"}, 1});
    packs[0].choices.push_back({"Shadows", {"Low", "Medium", "High"}, 1});
    packs[1].id = "graphicPacks/Fake_FPS/rules.txt";
    packs[1].path = "Fake Game/Mods/FPS++";
    packs[1].name = "FPS++";
    packs[1].choices.push_back({"", {"30FPS", "60FPS"}, 0});
    packs[2].id = "graphicPacks/Fake_Bloom/rules.txt";
    packs[2].path = "Fake Game/Enhancements/No Bloom";
    packs[2].name = "No Bloom";
    return packs;
}

void change_graphic_pack(std::vector<ds::ExternalGraphicPack>& packs,
                         const ds::ExternalGraphicPackChange& change) {
    for (ds::ExternalGraphicPack& pack : packs) {
        if (pack.id != change.id) continue;
        if (change.kind == ds::ExternalGraphicPackChange::Kind::Enabled) {
            pack.enabled = change.enabled;
            ds::log_info("graphics pack " + pack.name + (change.enabled ? " turned on" : " turned off"));
            return;
        }
        for (ds::ExternalGraphicPack::Choice& choice : pack.choices) {
            if (choice.category != change.category) continue;
            for (std::size_t at = 0; at < choice.presets.size(); ++at) {
                if (choice.presets[at] == change.preset) choice.active = static_cast<int>(at);
            }
        }
        ds::log_info("graphics pack " + pack.name + ": " + change.preset);
        return;
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::string socket_path = ds::ExternalFrameLink::default_socket_path();
    double seconds = 10.0;
    double fps = 60.0;
    // Publishes only the GamePad screen, to prove a consumer copes with one
    // screen arriving and the other never doing so.
    bool pad_only = false;
    // Answers a pause the way a game with its HOME Menu turned off does, to
    // show what the quick menu does then.
    bool refuse_pause = false;
    // Never answers one, as a game that does not listen for the HOME Menu
    // never does.
    bool ignore_pause = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto value = [&]() -> std::string {
            if (i + 1 >= argc) {
                usage();
                std::exit(2);
            }
            return argv[++i];
        };
        if (arg == "--socket") socket_path = value();
        else if (arg == "--seconds") seconds = std::atof(value().c_str());
        else if (arg == "--fps") fps = std::atof(value().c_str());
        else if (arg == "--pad-only") pad_only = true;
        else if (arg == "--refuse-pause") refuse_pause = true;
        else if (arg == "--ignore-pause") ignore_pause = true;
        else {
            usage();
            return 2;
        }
    }
    if (fps <= 0.0) {
        usage();
        return 2;
    }

    ds::ExternalStreamInfo info;
    info.top_width = kTvWidth;
    info.top_height = kTvHeight;
    info.bottom_width = kPadWidth;
    info.bottom_height = kPadHeight;
    info.audio_sample_rate = 48000;
    info.frames_per_second = fps;
    // The overlay too, as Cemu offers it, so the menu keys go the way they do
    // with Cemu -- through a pause. Its pictures are taken and drawn nowhere:
    // there is no window here to draw them in. And a few graphics packs, kept
    // only in memory, for the quick menu's Graphics Packs page to change.
    info.features = ds::kExternalFeatureOverlay | ds::kExternalFeaturePause |
                    ds::kExternalFeatureGraphicPacks | ds::kExternalFeatureOptions |
                    ds::kExternalFeatureAmiibo;
    std::vector<ds::ExternalGraphicPack> packs = fake_graphic_packs();
    // Cemu's options, as its fork offers them, kept only in memory.
    std::vector<ds::ExternalOption> options(3);
    options[0] = {"upscale_filter", "Upscaling", {"Bilinear", "Bicubic", "Hermite", "Nearest neighbor"}, 1, false};
    options[1] = {"fullscreen_scaling", "Aspect ratio", {"Original", "Stretched"}, 0, false};
    options[2] = {"performance_overlay", "Performance overlay",
                  {"Off", "Frame rate", "Frame rate and CPU", "Everything"}, 0, false};

    ds::ExternalFrameProducer producer;
    std::string error;
    if (!producer.connect(socket_path, info, &error)) {
        std::fprintf(stderr, "fake_wii_u: %s\n", error.c_str());
        return 1;
    }
    ds::log_info("connected to " + socket_path);

    std::vector<uint32_t> tv;
    std::vector<uint32_t> pad;
    std::vector<int16_t> audio;
    const auto started = std::chrono::steady_clock::now();
    const auto frame_time = std::chrono::duration<double>(1.0 / fps);

    int frame = 0;
    // Where the scene is drawn from. A reset moves it back to the beginning,
    // which is the whole of what this stands in for.
    int scene_origin = 0;
    // Only reported when it changes: a pad is sent every frame it differs and
    // a line a frame would be a log rather than a record.
    ds::ExternalInput last_input;
    bool paused = false;
    while (true) {
        const auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration<double>(now - started).count() >= seconds) {
            break;
        }

        bool reset = false;
        if (!producer.poll(reset, &error)) {
            ds::log_info("the far end went away: " + error);
            break;
        }
        if (reset) {
            ds::log_info("restarting, because the player asked");
            scene_origin = frame;
        }
        // Pausing here is only saying so: the scene stands still meanwhile.
        bool pause = false;
        while (producer.take_pause_request(pause)) {
            if (ignore_pause) {
                ds::log_info(pause ? "not answering a request to pause"
                                   : "not answering a request to go on");
            } else if (pause && refuse_pause) {
                ds::log_info("refusing to pause, as a game saving would");
                producer.send_pause_state(ds::ExternalPauseState::Refused);
            } else if (pause) {
                ds::log_info("paused");
                paused = true;
                producer.send_pause_state(ds::ExternalPauseState::Paused);
            } else if (paused) {
                ds::log_info("going on");
                paused = false;
                producer.send_pause_state(ds::ExternalPauseState::Running);
            }
        }
        bool packs_wanted = producer.take_graphic_packs_wanted();
        for (ds::ExternalGraphicPackChange change; producer.take_graphic_pack_change(change);) {
            change_graphic_pack(packs, change);
            packs_wanted = true;
        }
        if (packs_wanted) producer.send_graphic_packs(packs);
        bool options_wanted = producer.take_options_wanted();
        std::string option_id;
        for (int choice = 0; producer.take_option_change(option_id, choice);) {
            for (ds::ExternalOption& option : options) {
                if (option.id == option_id && choice < static_cast<int>(option.choices.size())) {
                    option.active = choice;
                    ds::log_info(option.label + ": " + option.choices[static_cast<std::size_t>(choice)]);
                }
            }
            options_wanted = true;
        }
        if (options_wanted) producer.send_options(options);
        // An amiibo is only measured, not read: a file the size of an
        // amiibo's dump is one, as far as this stand-in goes.
        for (std::string amiibo; producer.take_amiibo_scan(amiibo);) {
            std::FILE* file = std::fopen(amiibo.c_str(), "rb");
            ds::ExternalAmiiboResult result = ds::ExternalAmiiboResult::CannotOpen;
            if (file != nullptr) {
                std::fseek(file, 0, SEEK_END);
                const long size = std::ftell(file);
                std::fclose(file);
                result = size == 540 || size == 572 ? ds::ExternalAmiiboResult::Scanned
                                                    : ds::ExternalAmiiboResult::NotAnAmiibo;
            }
            ds::log_info("amiibo " + amiibo +
                         (result == ds::ExternalAmiiboResult::Scanned ? ": scanned" : ": not scanned"));
            producer.send_amiibo_result(result);
        }
        if (producer.have_input() && producer.input() != last_input) {
            last_input = producer.input();
            ds::log_info("pad: " + describe(last_input));
        }

        if (paused) scene_origin += 1;
        const int scene_frame = frame - scene_origin;
        // Only whichever screen the shell has actually asked for -- see
        // ExternalControl::RequestScreen. Mirrors the gate a real fork of
        // Cemu applies (DualscreenSession::ShouldCapture), so a test run
        // against this stand-in exercises the same "nothing arrives for a
        // screen nobody wanted" behavior a real one does.
        const std::optional<ds::ScreenId> requested = producer.requested_screen();
        if (!pad_only && requested == ds::ScreenId::Top) {
            draw(tv, kTvWidth, kTvHeight, scene_frame, /*television=*/true);
            if (!producer.publish(ds::ScreenId::Top, static_cast<uint64_t>(frame), tv.data(),
                                  kTvWidth, kTvHeight, kTvWidth, &error)) {
                ds::log_info("could not publish the television screen: " + error);
                break;
            }
        }
        if (requested == ds::ScreenId::Bottom) {
            draw(pad, kPadWidth, kPadHeight, scene_frame, /*television=*/false);
            if (!producer.publish(ds::ScreenId::Bottom, static_cast<uint64_t>(frame), pad.data(),
                                  kPadWidth, kPadHeight, kPadWidth, &error)) {
                ds::log_info("could not publish the GamePad screen: " + error);
                break;
            }
        }

        // A tone, so the audio path carries something a listener could tell
        // apart from silence.
        const std::size_t frames_of_audio =
            static_cast<std::size_t>(info.audio_sample_rate / fps);
        audio.assign(frames_of_audio * 2, 0);
        for (std::size_t i = 0; i < frames_of_audio; ++i) {
            const double t = static_cast<double>(frame * frames_of_audio + i) /
                             static_cast<double>(info.audio_sample_rate);
            const auto sample = static_cast<int16_t>(6000.0 * std::sin(t * 2.0 * 3.14159265 * 440.0));
            audio[i * 2] = sample;
            audio[i * 2 + 1] = sample;
        }
        if (!producer.send_audio(ds::ScreenId::Top, audio.data(), frames_of_audio, &error)) {
            ds::log_info("could not send audio: " + error);
            break;
        }

        ++frame;
        std::this_thread::sleep_until(now + std::chrono::duration_cast<
                                                std::chrono::steady_clock::duration>(frame_time));
    }

    ds::log_info("published " + std::to_string(frame) + " frames");
    producer.close();
    return 0;
}
