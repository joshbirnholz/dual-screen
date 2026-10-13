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
                 "[--refuse-pause]\n");
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
    // there is no window here to draw them in.
    info.features = ds::kExternalFeatureOverlay | ds::kExternalFeaturePause;

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
            if (pause && refuse_pause) {
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
