#pragma once

#include <deque>
#include <functional>
#include <string>
#include <vector>

#include "core/screen_source.hpp"
#include "input/ds_input.hpp"
#include "net/discovery.hpp"
#include "net/protocol.hpp"
#include "net/transport.hpp"
#include "video/video_decoder.hpp"

namespace ds::net {

// Bounds on how much picture is ever held back to smooth out an uneven link.
//
// The floor is one frame rather than none: a frame still waits for its slot on
// the clock, so motion keeps the host's cadence instead of this device's, but
// nothing beyond that is held speculatively. The ceiling is what a genuinely
// bad link is allowed to cost before the answer becomes "show it late" rather
// than "hold more".
inline constexpr std::size_t kMinVideoBufferFrames = 1;
inline constexpr std::size_t kMaxVideoBufferFrames = 4;

// How many frames to keep in hand given how unevenly they have been arriving.
//
// A fixed depth has to be sized for the worst link it will ever see, and then
// every good link pays that cost for ever: frames sit finished and undisplayed
// purely because a number said so. Worse, a depth that is only ever a floor
// ratchets -- one hiccup fills it and nothing ever drains it again, so latency
// climbs over a session and never comes back down. Sized from the jitter
// actually measured instead, a calm link settles to the floor and a rough one
// buys exactly the cover it needs, for exactly as long as it needs it.
std::size_t video_buffer_target_frames(double jitter_ms, double period_ms);

// How long a finished frame may sit waiting before the pacing clock has to
// give ground and let it through, in milliseconds.
//
// This is what stops the clock hoarding. Showing one frame per period keeps
// the cadence even but conserves the queue's depth exactly, so any backlog
// becomes permanent latency. Letting a frame through once it has waited longer
// than the link's own measured unevenness drains that backlog instead, so the
// queue settles at the depth the link needs rather than at whatever a past
// hiccup left behind. Never below one period, or there would be nothing left
// for the clock to pace with and frames would simply be shown on arrival,
// jitter and all; never above two, so no jitter estimate can talk this back
// into hoarding.
uint64_t video_hold_allowance_ms(double jitter_ms, uint64_t period_ms);

// The companion half: finds hosts, connects to one, and presents the screens it
// receives as a ScreenSource -- the same seam the libretro core plugs into, so
// the compositor and layout system never learn where the pixels came from.
class CompanionSession final : public ScreenSource {
public:
    enum class Status {
        Idle,
        Searching,
        Connecting,
        Connected,
        Failed,
    };

    CompanionSession();
    ~CompanionSession() override;

    // Starts browsing for hosts. Safe to call repeatedly.
    bool begin_search(const std::string& device_name, ScreenId wanted_screen, std::string* error);
    // Connects to one of the hosts found by the search.
    // By value: callers pass an entry out of hosts(), and a session that ever
    // clears that list while connecting would leave the argument dangling.
    void connect_to(DiscoveredHost host);
    void disconnect();

    void poll(uint64_t now_ms);

    Status status() const { return status_; }
    const std::string& status_detail() const { return status_detail_; }
    const std::vector<DiscoveredHost>& hosts() const { return browser_.hosts(); }
    const std::string& game_title() const { return game_title_; }
    // Which console the host is playing, so this device draws it the same way.
    SystemId system() const { return system_; }
    ScreenId granted_screen() const { return granted_screen_; }
    // The session as the host sees it. This device follows all of it rather
    // than deciding anything locally, which is what keeps the two in step.
    const std::string& host_layout_id() const { return host_layout_id_; }
    bool host_paused() const { return host_paused_; }
    bool host_filtering() const { return host_filtering_; }
    AudioTarget host_audio_target() const { return host_audio_target_; }
    Handedness host_handedness() const { return host_handedness_; }
    RightStickMode host_right_stick_mode() const { return host_right_stick_mode_; }
    int host_render_scale() const { return host_render_scale_; }
    bool host_screen_gap() const { return host_screen_gap_; }

    // The one menu, as the host has it. This device draws from these rather
    // than running a menu of its own, so both screens show the same thing
    // whichever pad is working it.
    bool host_menu_open() const { return host_menu_open_; }
    // The host's own settings screen. Not mirrored -- this device is told only
    // that it is up, so it can say where to look and keep sending presses.
    bool host_settings_open() const { return host_settings_open_; }
    int host_menu_focus() const { return host_menu_focus_; }
    uint8_t host_menu_confirm() const { return host_menu_confirm_; }
    int host_max_render_scale() const { return host_max_render_scale_; }
    bool host_cursor_choosable() const { return host_cursor_choosable_; }
    bool host_right_stick_choosable() const { return host_right_stick_choosable_; }
    bool host_cursor_shown() const { return host_cursor_shown_; }

    // Sends this device's input to the host. Only what changed goes out.
    void send_input(const DsInputState& state);

    // Asks the host to change something. The host owns every one of these and
    // broadcasts the result back, so nothing is assumed to have taken effect
    // here -- which is exactly why the two devices cannot end up disagreeing.
    void request_layout(const std::string& layout_id);
    void request_filtering(bool bilinear);
    void request_audio_target(AudioTarget target);
    void request_handedness(Handedness handedness);
    void request_right_stick_mode(RightStickMode mode);
    void request_render_scale(int scale);
    void request_screen_gap(bool include_gap);
    // A press made on this device, for the host's menu to answer.
    void request_menu_action(MenuAction action);
    // Where a finger landed here, already resolved to an entry: the two
    // devices' menus are laid out for different screens, so the host cannot
    // work out what a tap over there was on.
    void request_menu_focus(int focus);
    // A tap: one press, at the entry the finger landed on. Sent together so
    // the two cannot be separated on the way over.
    void request_menu_press(int focus, MenuAction action);
    // One-shot: asks the host to restart the game. There is nothing to adopt
    // back, since there is no value beyond the moment it happens.
    void request_reset();
    // One-shot: asks the host to shut down entirely, the same as choosing Quit
    // there. Does not itself end this device's session -- the caller still
    // calls disconnect() to leave, the same as it would for Disconnect.
    void request_quit();
    // Not one of the choices above -- the host does not adopt or broadcast
    // this, it just uses it privately to decide how much detail a screen sent
    // here is worth being sent at. Call it once after connecting and again
    // whenever this device's own window resizes.
    void request_display_size(int width, int height);

    // Tells the host whether a menu is up on this device, so it pauses too.
    // Sent whenever it changes, and repeated with every other request.
    void set_menu_open(bool open);

    // Called with audio received from the host, ready to play. Set before
    // connecting.
    using AudioCallback =
        std::function<void(const int16_t* interleaved_stereo, std::size_t frames, uint32_t rate)>;
    void set_audio_callback(AudioCallback callback) { audio_callback_ = std::move(callback); }
    // The rate the host said it produces audio at, known from the handshake.
    uint32_t audio_sample_rate() const { return audio_sample_rate_; }

    // Called whenever the host says the Wii U GamePad's motor turned on or
    // off, so this device can rumble whatever controller it has in its
    // place. Set before connecting, same as the audio callback above.
    using RumbleCallback = std::function<void(bool active)>;
    void set_rumble_callback(RumbleCallback callback) { rumble_callback_ = std::move(callback); }

    // The last frame received for a screen, as XRGB8888 pixels on the CPU,
    // with the serial that identifies it so a caller can tell a new frame from
    // one it has already drawn. Valid until the next poll(). Empty until
    // something has arrived.
    //
    // screens() hands the same pixels over as GL textures, which is what the
    // desktop compositor draws. A device that does not have that renderer --
    // the Switch client, which draws through SDL's own -- uploads these to a
    // texture of its own instead, rather than there being a second copy of
    // the receiving and decoding to keep in step with this one.
    struct ScreenPixels {
        const uint32_t* pixels = nullptr;
        int width = 0;
        int height = 0;
        uint64_t serial = 0;

        bool valid() const { return pixels != nullptr && width > 0 && height > 0; }
    };
    ScreenPixels screen_pixels(ScreenId screen) const;

    // The same frame in the form it arrived in: RGB565, two bytes a pixel.
    // A renderer that can upload that directly should use this instead --
    // expanding costs a whole pass over the frame and hands the GPU twice the
    // bytes, both of which are worth avoiding on a small device.
    struct ScreenBytes {
        const uint8_t* pixels = nullptr;
        int width = 0;
        int height = 0;
        uint64_t serial = 0;

        bool valid() const { return pixels != nullptr && width > 0 && height > 0; }
    };
    ScreenBytes screen_rgb565(ScreenId screen) const;

    // Whether a screen has ever arrived, and the serial of the last one. The
    // split layout only sends one screen, so "did the other one arrive?" is a
    // real question worth being able to answer.
    bool has_screen(ScreenId screen) const;
    uint64_t received_serial(ScreenId screen) const;
    // Top-left pixel of the last frame for a screen, for tests that have no GL
    // context and so cannot look at the texture.
    uint32_t sample_pixel(ScreenId screen) const;

    // --- ScreenSource ---
    bool advance_frame(std::string* error) override;
    const ScreenTextures& screens() override;
    // Drops the GL textures the streamed screens were uploaded into, so the
    // next screens() uploads them again. For a device whose GL context can be
    // taken away and given back -- Android, when the app is backgrounded --
    // where the texture names survive as numbers that no longer mean anything.
    void forget_screen_textures();

    // How many whole frames have arrived and not yet been shown.
    std::size_t buffered_frames() const;
    // The newest frame actually decoded here, which is what the host is told.
    uint64_t newest_frame_serial_for_test() const {
        return std::max(top_.serial, bottom_.serial);
    }
    // Which way the newest frame for this screen arrived. The two are meant to
    // be interchangeable, which is exactly why a test has to be able to tell
    // them apart: a run that quietly fell back to tiles looks identical in the
    // pixels.
    bool screen_is_video_for_test(ScreenId screen) const {
        return (screen == ScreenId::Top ? top_ : bottom_).video;
    }
    SystemScreens screens_geometry() const override;
    bool wants_shutdown() const override { return false; }

private:
    void handle_message(MessageType type, const std::vector<uint8_t>& payload);
    // Frame and Audio, the host->companion half of the media channel -- see
    // kMediaPort. A malformed or stray datagram is simply ignored rather than
    // ending the session: UDP has no stream to desynchronise, so one bad
    // packet is just one bad packet.
    void handle_media_datagram(MessageType type, const uint8_t* payload, std::size_t payload_size);
    // Accumulates one screen's fragments (see FrameFragmentHeader) until
    // they are all in, then hands the whole thing to pending_frames_ the same
    // way a TCP-carried Frame message used to arrive complete already. A
    // fragment for a newer serial than whatever is in progress discards the
    // old, incomplete one -- the sender has moved on, so waiting for stray
    // fragments of a frame that will never complete would only add latency.
    struct FrameReassembly {
        bool active = false;
        uint64_t serial = 0;
        ScreenId screen = ScreenId::Top;
        FrameEncoding encoding = FrameEncoding::Rgb565Deflate;
        uint16_t width = 0;
        uint16_t height = 0;
        uint16_t fragment_count = 0;
        std::size_t received_count = 0;
        std::vector<std::vector<uint8_t>> fragments;
        // Separate from fragments[i].empty(): a fragment can legitimately
        // carry zero payload bytes, which must not read as "not received yet"
        // and stall a reassembly that is otherwise already complete.
        std::vector<bool> received;
    };
    void handle_frame_fragment(const uint8_t* payload, std::size_t payload_size);
    FrameReassembly reassembly_top_;
    FrameReassembly reassembly_bottom_;
    void send_request(RequestMessage request);
    void fail(const std::string& reason);

    // Frames that have arrived but not yet been applied.
    //
    // The host produces on its own clock and this device shows on its own, and
    // the two are never quite the same: a frame or two arrives inside one
    // display interval and none inside the next. Applying everything that has
    // arrived, as this used to, turns that into a shown frame skipped and then
    // a shown frame repeated -- which is what a stream at a healthy sixty
    // frames a second still looks choppy at.
    //
    // Kept whole rather than collapsed because a tiled frame is a difference
    // from the one before it: they have to be applied in order, and none can
    // be thrown away without corrupting the picture.
    std::deque<FrameMessage> pending_frames_;
    // Defined further down, with the rest of what a screen holds.
    struct ScreenBuffer;
    void apply_frame(const FrameMessage& frame);
    // False when nothing came out: a failure, already reported, or a stream
    // still waiting for its first keyframe.
    bool apply_video_frame(const FrameMessage& frame, ScreenBuffer& target);
    bool decode_next_frame();

    HostBrowser browser_;
    TcpStream stream_;
    std::vector<uint8_t> inbox_;
    // Video, audio, input and frame-acks ride here instead of the TCP
    // connection above -- see kMediaPort. An ephemeral local port is enough:
    // this device only ever sends to and receives from the one host, whose
    // fixed, well-known port it already sends to directly.
    UdpSocket media_socket_;
    // From WelcomeMessage, echoed on every datagram this device sends there
    // -- see HostSession::session_token_'s own comment for why.
    uint64_t session_token_ = 0;
    // The host's own address, learned from the TCP connection (same machine,
    // just a different port for the media channel).
    std::string host_address_;

    Status status_ = Status::Idle;
    std::string status_detail_;
    std::string device_name_;
    ScreenId wanted_screen_ = ScreenId::Bottom;
    ScreenId granted_screen_ = ScreenId::Bottom;
    std::string game_title_;
    SystemId system_ = SystemId::Ds;
    std::string host_layout_id_;
    bool host_paused_ = false;
    bool host_filtering_ = false;
    AudioTarget host_audio_target_ = AudioTarget::Both;
    Handedness host_handedness_ = Handedness::Right;
    RightStickMode host_right_stick_mode_ = RightStickMode::CStick;
    int host_render_scale_ = 1;
    bool host_screen_gap_ = true;
    bool host_menu_open_ = false;
    bool host_settings_open_ = false;
    int host_menu_focus_ = 0;
    uint8_t host_menu_confirm_ = 0;
    int host_max_render_scale_ = 1;
    bool host_cursor_choosable_ = false;
    bool host_right_stick_choosable_ = false;
    bool host_cursor_shown_ = true;
    bool menu_open_ = false;
    bool said_hello_ = false;
    // When to give up on a connection that has not finished arriving. Zero
    // until the first poll() after connect_to(), so the whole timer runs on
    // the caller's clock rather than half on one and half on another.
    uint64_t connect_deadline_ms_ = 0;
    // The newest frame serial already reported to the host, and the newest
    // that has arrived. Reported on arrival rather than on being shown: the
    // ack answers "did this get here", which is the host's flow control, and
    // frames waiting their turn below have got here. Answering it with what
    // has been shown instead made the host read this device's own smoothing
    // as a device falling behind, and throttle against it.
    uint64_t acked_serial_ = 0;
    uint64_t received_serial_ = 0;

    // When the next frame is due on screen, and how far apart frames are
    // arriving. Paced by a clock rather than by one per pass, because a pass
    // is not a frame: a 90Hz panel showing 60fps content passes half again as
    // often as there is anything new to show, and pinning the two together
    // leaves nothing buffered and each frame held for whichever number of
    // refreshes its arrival happened to land on.
    uint64_t next_due_ms_ = 0;
    uint64_t last_arrival_ms_ = 0;
    double frame_period_ms_ = 16.7;
    uint64_t frame_period() const;
    // How unevenly frames have been arriving lately, in milliseconds, and the
    // buffer depth that calls for. Every frame held back is a frame of latency
    // paid for smoothness, so this buys only as much as the link is actually
    // being uneven by, and gives it back as soon as the link settles -- see
    // video_buffer_target_frames().
    double jitter_ms_ = 0.0;
    std::size_t target_buffer_frames() const;

    // When each frame finished reassembling, so that the wait between having
    // every byte of a picture and actually showing it can be measured rather
    // than reasoned about. That wait is the one part of the end-to-end delay
    // this end chooses rather than inherits, which makes it the first thing
    // worth knowing the real size of -- see report_stream().
    std::deque<std::pair<uint64_t, uint64_t>> complete_at_;
    uint64_t stats_wait_total_ms_ = 0;
    uint64_t stats_wait_worst_ms_ = 0;
    unsigned stats_waits_ = 0;
    uint64_t stats_decode_total_ms_ = 0;
    unsigned stats_decodes_ = 0;

    // The other half of the host's once-a-second summary: what actually
    // arrived here. The two together say whether frames are being lost on the
    // wire or never sent in the first place.
    void report_stream(uint64_t now_ms);
    uint64_t stats_at_ms_ = 0;
    unsigned stats_decoded_ = 0;

public:
    // Counted by whoever is drawing, once per presented frame. Decoded and
    // drawn are different questions: a device can be handed every frame and
    // still show a fraction of them, and only the second is what the player
    // sees. Told apart here because the difference is the whole diagnosis.
    void note_frame_drawn() { ++stats_drawn_; }

private:
    unsigned stats_drawn_ = 0;

    // Latest pixels per screen, uploaded lazily so a frame that arrives twice
    // between draws is only sent to the GPU once.
    struct ScreenBuffer {
        // Which of the two below is the frame and which is derived from it. A
        // tiled payload patches rgb565 and is expanded on demand; a video frame
        // decodes straight to pixels and never has a 5-6-5 form at all.
        bool video = false;
        // The frame as it travels and as tiles are patched onto it. Canonical:
        // a tiled payload is a difference from this, so it has to survive from
        // one frame to the next.
        std::vector<uint8_t> rgb565;
        int width = 0;
        int height = 0;
        uint64_t serial = 0;
        uint64_t uploaded_serial = 0;
        // Expanded only when somebody asks for XRGB8888, and only once per
        // frame that does. A client that can upload 5-6-5 straight to a
        // texture never asks, and so never pays for the pass or the doubled
        // bytes.
        mutable std::vector<uint32_t> pixels;
        mutable uint64_t expanded_serial = 0;
        mutable bool ever_expanded = false;
        Texture texture;

        const std::vector<uint32_t>& expanded() const;
        // Whether anything has arrived for this screen at all, whichever form
        // it arrived in.
        bool has_frame() const { return video ? !pixels.empty() : !rgb565.empty(); }
    };
    ScreenBuffer top_;
    ScreenBuffer bottom_;
    // One per screen, matching the encoders at the other end: two streams, two
    // sizes, and each frame only means anything after the ones before it.
    video::VideoDecoder video_top_;
    video::VideoDecoder video_bottom_;
    ScreenTextures textures_;

    DsInputState last_sent_input_;
    bool ever_sent_input_ = false;
    AudioCallback audio_callback_;
    uint32_t audio_sample_rate_ = 0;
    RumbleCallback rumble_callback_;
    // Per AudioStreamId (3 entries -- Shared/TopDevice/BottomDevice), for
    // detecting a genuinely missing audio packet rather than the ordinary
    // jitter UDP never promised against -- see AudioMessage::sequence and
    // handle_media_datagram()'s Audio case.
    bool audio_sequence_known_[3] = {false, false, false};
    uint32_t audio_next_sequence_[3] = {0, 0, 0};
};

const char* to_string(CompanionSession::Status status);

}  // namespace ds::net
