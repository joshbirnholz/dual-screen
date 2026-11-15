#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "layout/screen_id.hpp"
#include "net/web_play_server.hpp"
#include "video/video_codec.hpp"

namespace ds::net {

// Web Play, running: the server and the encoders, on a thread of their own.
//
// The game's frame loop hands screens and sound in and reads the browsers'
// pads out, and never waits on a browser -- TLS, compression and a slow
// friend's connection all happen over here. See WebPlayServer for what goes
// over the wire.
class WebPlayHost {
public:
    struct Config {
        uint16_t port = 0;
        TlsIdentity identity;  // empty: plain HTTP, for tests
        std::string device_name;
        std::string game_title;
        // The page, when not the built-in one (tests).
        std::string page;
        // Per screen. Every browser watching it is sent the same stream, so
        // this computer's upload is this times the number of browsers.
        int bitrate_kbps = 6000;
        // The largest picture sent of each screen; a bigger one is scaled
        // down to fit, keeping its shape.
        int max_tv_width = 1280;
        int max_tv_height = 720;
        int max_gamepad_width = 854;
        int max_gamepad_height = 480;
    };

    WebPlayHost() = default;
    ~WebPlayHost();
    WebPlayHost(const WebPlayHost&) = delete;
    WebPlayHost& operator=(const WebPlayHost&) = delete;

    bool start(const Config& config, std::string* error);
    void stop();
    bool running() const { return running_; }
    uint16_t port() const { return port_; }

    // Whether a browser is watching this screen now, so it is worth reading.
    bool screen_watched(ScreenId screen) const;
    // A frame of a screen. Copied; the latest one waiting replaces any before
    // it that has not been encoded yet.
    void publish_screen(ScreenId screen, const uint32_t* pixels, int width, int height, int stride_pixels,
                        video::PixelOrder order);
    // Some of a screen's sound, s16 stereo.
    void publish_audio(ScreenId screen, const int16_t* samples, std::size_t frames, uint32_t rate);

    std::vector<WebViewer> viewers() const;
    std::vector<WebPad> pads() const;
    uint64_t roster_serial() const;

    void set_viewer_screen(uint32_t viewer, ScreenId screen);
    void send_rumble(uint32_t pad_id, uint16_t low, uint16_t high, uint32_t duration_ms);
    // JSON to one browser, or 0 for all.
    void send_text(uint32_t viewer, const std::string& json);

    // What the encoders actually opened as, for the log: "libx264".
    std::string backend(ScreenId screen) const;

private:
    struct PendingScreen {
        std::vector<uint32_t> pixels;
        int width = 0;
        int height = 0;
        video::PixelOrder order = video::PixelOrder::Bgra;
        bool fresh = false;
    };
    struct PendingAudio {
        std::vector<int16_t> samples;
        uint32_t rate = 0;
    };
    struct Command {
        enum class Kind { Screen, Rumble, Text } kind = Kind::Text;
        uint32_t target = 0;
        ScreenId screen = ScreenId::Top;
        uint16_t low = 0, high = 0;
        uint32_t duration_ms = 0;
        std::string text;
    };
    void run();
    void encode(ScreenId screen, PendingScreen& frame);

    Config config_;
    WebPlayServer server_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> quit_{false};
    uint16_t port_ = 0;

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    PendingScreen pending_[2];
    PendingAudio audio_[2];
    std::vector<Command> commands_;
    // What the thread last saw, for the frame loop to read without waiting.
    std::vector<WebViewer> viewers_;
    std::vector<WebPad> pads_;
    uint64_t roster_serial_ = 0;
    bool watched_[2] = {false, false};
    std::string backend_[2];

    struct Encoder;
    Encoder* encoders_[2] = {nullptr, nullptr};
};

}  // namespace ds::net
