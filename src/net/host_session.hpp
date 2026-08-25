#pragma once

#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "core/screen_source.hpp"
#include "input/ds_input.hpp"
#include "net/discovery.hpp"
#include "net/protocol.hpp"
#include "net/transport.hpp"

namespace ds::net {

// The host half of a two-device session: announces the running game, accepts one
// companion, streams it the screens it needs, and hands its input back.
//
// Polled from the frame loop like everything else. It never blocks and never
// owns a thread, so a companion that stalls slows nothing down locally.
class HostSession {
public:
    struct Config {
        std::string device_name;
        std::string game_title;
        std::string rom_sha1;
        // Which console is running, so the companion lays its screens out the
        // same way and clamps touches to the same coordinate space.
        SystemId system = SystemId::Ds;
        // Where the host starts. A companion asks for whichever screen it
        // wants and the host takes the other, so this is only the arrangement
        // before anyone connects.
        ScreenId host_screen = ScreenId::Top;
        // Rate the game's audio is produced at, so the companion can open its
        // own output to match.
        uint32_t audio_sample_rate = 0;
    };

    bool start(const Config& config, std::string* error);
    void stop();
    bool running() const { return listener_.listening(); }

    bool has_companion() const { return companion_.connected() && handshake_done_; }
    const std::string& companion_name() const { return companion_name_; }
    ScreenId companion_screen() const { return companion_screen_; }
    // Whatever the companion did not take.
    ScreenId host_screen() const { return config_.host_screen; }
    // Empty unless the last companion left or was refused, for the host to show.
    const std::string& last_notice() const { return last_notice_; }

    // The host's own screen assignment, which the split layout follows.
    void set_host_screen(ScreenId screen);
    // Everything the two devices have to agree on. The host is the only writer,
    // which is what stops the two ends drifting apart.
    struct SharedState {
        std::string layout_id;
        // Whether that layout puts one screen on each device. Passed in rather
        // than matched against an id here: which layouts are per-device is the
        // registry's business, and a hardcoded id here would quietly stop being
        // true the next time the layout set changed.
        bool per_device_layout = false;
        bool paused = false;
        bool bilinear_filtering = false;
        AudioTarget audio_target = AudioTarget::Both;
        Handedness handedness = Handedness::Right;
        RightStickMode right_stick_mode = RightStickMode::CStick;
        // The host is the only device running a core, but the setting is shown
        // and changed on both, so it travels with everything else they share.
        int render_scale = 1;
        // Whether a layout shows the gap between its screens, for whichever
        // layouts offer the choice -- see LayoutComposition::gap_toggleable.
        bool screen_gap = true;
    };

    // Cheap to call every frame; nothing is sent unless something changed.
    void set_session_state(const SharedState& state);

    // Sends the game's audio to the companion. DS and 3DS mirror it, so both
    // devices play the same thing; the stream id leaves room for Wii U, where
    // each device has its own.
    void publish_audio(const int16_t* interleaved_stereo, std::size_t frames, uint32_t sample_rate);
    void set_audio_enabled(bool enabled) { audio_enabled_ = enabled; }
    bool audio_enabled() const { return audio_enabled_; }

    // Ends the session from this end. The companion is told, so it returns to
    // its launcher rather than waiting for a timeout.
    void disconnect_companion(const std::string& reason);

    // What the companion has asked for since the last call, if anything. The
    // host owns every one of these choices, so the app validates and applies
    // them and they come back to the companion as session state.
    bool take_request(RequestMessage& out_request);

    // A companion asking to quit is latched here the instant its request
    // arrives, separately from take_request() above. A quit is always
    // followed straight away by the companion's own Bye, which lands in the
    // same poll() and would otherwise wipe an unconsumed request_ (via
    // drop_companion()) before the app next got to read it.
    bool take_companion_quit_request();

    // The companion has its own menu open, so the game is paused there. The
    // host pauses too: a session where one device is playing on while the other
    // reads a menu is two people looking at different games.
    bool companion_menu_open() const { return companion_menu_open_; }

    // The companion's own window, in device pixels, if it has said. Not part
    // of the shared session state -- it is never broadcast anywhere, just kept
    // so the caller can decide how much detail a screen it sends is worth
    // being sent at. False before the companion has reported anything, which
    // is the same as not knowing: the caller falls back to the console's own
    // resolution.
    bool companion_display_size(int& out_width, int& out_height) const {
        out_width = companion_display_width_;
        out_height = companion_display_height_;
        return companion_display_width_ > 0 && companion_display_height_ > 0;
    }

    // One screen's pixels inside the core's framebuffer. The two are described
    // separately because they are not always the same size.
    struct ScreenPixels {
        const uint32_t* pixels = nullptr;
        int width = 0;
        int height = 0;
        int stride_pixels = 0;
        PixelChannelOrder channel_order = PixelChannelOrder::XrgbBytes;
    };

    // Which screens the companion needs right now. Public because reading a
    // screen out of a GPU-rendered frame costs a copy back off the card, so the
    // caller has to be able to skip one nobody is going to see.
    bool companion_wants(ScreenId screen) const;

    // Sends whichever screens the companion needs for the current layout.
    // `serial` should advance only when the pixels actually changed.
    void publish_screens(const ScreenPixels& top, const ScreenPixels& bottom, uint64_t serial);

    // Accepts connections, moves bytes, and drains input. Call once a frame.
    void poll(uint64_t now_ms);

    // Input received from the companion since the last call, already in DS
    // coordinate space. Merged with local input by the caller.
    const DsInputState& companion_input() const { return companion_input_; }
    bool companion_input_fresh() const { return companion_input_fresh_; }

private:
    void refresh_beacon();
    void drop_companion(const std::string& reason);
    void handle_message(MessageType type, const std::vector<uint8_t>& payload);
    // How much may sit unsent before a frame is dropped rather than queued --
    // see kMaxQueuedFrames.
    std::size_t queue_limit() const;
public:
    // Total bytes ever handed to the companion's socket. Lets a test see
    // whether a frame was queued at all, which is the thing the backlog limit
    // decides.
    uint64_t queued_bytes_for_test() const { return companion_.queued_total(); }

private:
    // How many whole frames have been queued but have not yet left the
    // machine. Prunes the ones that have.
    std::size_t frames_in_flight();

    Config config_;
    TcpListener listener_;
    HostAnnouncer announcer_;

    TcpStream companion_;
    std::vector<uint8_t> inbox_;
    bool handshake_done_ = false;
    std::string companion_name_;
    ScreenId companion_screen_ = ScreenId::Bottom;
    std::string last_notice_;

    SharedState shared_;
    bool session_state_dirty_ = true;

    DsInputState companion_input_;
    bool companion_input_fresh_ = false;
    uint64_t sent_serial_ = 0;
    bool audio_enabled_ = true;
    bool companion_menu_open_ = false;
    int companion_display_width_ = 0;
    int companion_display_height_ = 0;
    // The size of the last screen payload actually sent, both screens
    // combined. Bounds how much backlog is allowed to build up before a frame
    // is dropped -- in frame-times rather than a fixed byte count, so the
    // limit rises and falls with whatever resolution is currently being
    // streamed instead of being tuned for one console and wrong for another.
    // Where each queued frame ends, counted in bytes ever handed to the
    // socket. A frame has gone once that many bytes have actually departed.
    std::deque<uint64_t> frame_marks_;
    // The newest frame the companion says it has decoded. Zero until it has
    // said anything.
    uint64_t acked_serial_ = 0;
    // The last frame of each screen that was actually put on the wire, in
    // RGB565, which the next one is sent as the difference from. The frame
    // *sent* rather than the one produced: a frame skipped because the link
    // was behind must still be carried into the next one that goes out, or
    // the tiles it held would never arrive. Cleared when a companion comes or
    // goes, so the next one starts from a whole frame.
    std::vector<uint8_t> previous_top_;
    std::vector<uint8_t> previous_bottom_;
    // Send the current screens once more even if nothing has changed, because
    // what the companion needs has.
    bool resend_screens_ = true;
    bool have_request_ = false;
    RequestMessage request_;
    bool companion_wants_quit_ = false;
};

}  // namespace ds::net
