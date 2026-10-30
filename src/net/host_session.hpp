#pragma once

#include <functional>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <optional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/screen_source.hpp"
#include "input/ds_input.hpp"
#include "net/discovery.hpp"
#include "net/protocol.hpp"
#include "net/transport.hpp"
#include "video/video_encoder.hpp"

namespace ds::net {

// See the definition in host_session.cpp. Exposed for tests: it is the one
// piece of the pipelined-encoder path whose behaviour cannot be reached
// through a software encoder, which hands every packet back on the same call
// that submitted it.
bool resolve_encoded_serial(std::deque<std::pair<int64_t, uint64_t>>& pending, int64_t pts,
                            uint64_t& out_serial);

// The host half of a two-device session: announces the running game, accepts one
// companion, streams it the screens it needs, and hands its input back.
//
// Polled from the frame loop like everything else. It never blocks and never
// owns a thread, so a companion that stalls slows nothing down locally.
// How many frames may be outstanding at once, from the delay this link is
// actually showing rather than the one it was assumed to have.
//
// A frame cannot be sent until an earlier one is acked, so the window divided
// by the round trip is a hard ceiling on the frame rate -- and a window that
// only covers the average round trip shuts on every excursion above it and
// drops frames in a burst until the link settles. Which is judder at the far
// end, not a saving: during an excursion the picture is not any newer for
// having fewer frames in it. See kMinFramesInFlight and its neighbours.
//
// `return_delay_ms` is how much of the round trip was spent on the way back --
// acks held up while frames arrived on time -- which the window covers without
// the ceiling it puts on everything else. Zero when the companion does not say
// how late its frames arrived.
std::size_t frames_in_flight_allowed(uint64_t round_trip_ms, double produce_period_ms,
                                     uint64_t return_delay_ms = 0);

// How long the oldest unacked frame may wait before it is taken for lost --
// see kAckStallMs. `round_trip_ms` is the link's high-water round trip, and
// `measured` whether it has one yet: until the companion has acked a frame
// at all there is nothing to scale by, and the full, cautious wait applies.
uint64_t ack_stall_ms(double round_trip_ms, bool measured);

// What decides whether one screen goes as H.264 or as changed tiles.
struct CodecEvidence {
    // The size it would be sent at.
    int width = 0;
    int height = 0;
    // Whether video can be sent at all: an encoder here, a decoder there, and
    // no recent failure of either.
    bool video_possible = false;
    // What tiles have been costing for this screen, in kilobytes a second,
    // while it has been sent as tiles.
    double tile_kbytes_per_second = 0.0;
    // Whether it went to video for being expensive as tiles rather than for
    // its size, and how long ago.
    bool on_video_for_weight = false;
    uint64_t ms_on_video = 0;
};
// A screen goes as video when it is big -- the Wii U's, or a 3DS read back
// above its native size -- where the tile encoder is both slow and heavy:
// measured, a GamePad is 43 Mbit/s as tiles and 2.5 as H.264, and encoding its
// tiles does not fit in a 60fps frame. It stays as tiles at a DS's or a 3DS's
// own size, where tiles are small, lossless and add no encoder delay -- unless
// the picture is so busy that tiles cost more than a home wireless link can be
// trusted with, in which case it goes as video for a while and is then tried
// as tiles again. And always as tiles when video cannot be had at all.
bool screen_goes_as_video(const CodecEvidence& evidence);
// The thresholds above, for tests.
constexpr int kVideoFromPixels = 200000;
constexpr double kVideoFromTileKBytesPerSecond = 2500.0;
constexpr uint64_t kVideoForWeightMs = 20000;

// How fast the media pacer may hand fragments to the socket, in bytes per
// millisecond, given what the encoders were asked for and what they are really
// producing. See the definition: those are not the same number.
double media_pacing_bytes_per_ms(uint32_t declared_kbps, double produced_bytes_per_ms);

// What bitrate a screen of this size, sent at this rate, should be encoded at.
// See the definition for why the rate has to be the measured one.
int bitrate_kbps_for_encode_size(int width, int height, double frames_per_second);

class HostSession {
public:
    struct Config {
        std::string device_name;
        std::string game_title;
        // Its icon, for the other device's menu: see WelcomeMessage::icon_rgba.
        int game_icon_width = 0;
        int game_icon_height = 0;
        std::vector<uint8_t> game_icon_rgba;
        // Its icon for Discord: see WelcomeMessage::icon_key.
        std::string game_icon_key;
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
        // Asked as each companion joins, by the name it gave: the token for
        // it to start games on this computer later, when Remote Start is on
        // -- see RemoteStartGrantMessage. False, or none, hands it nothing.
        std::function<bool(const std::string& device, RemoteStartGrantMessage& out)> remote_start_grant;
    };

    HostSession() = default;
    ~HostSession();
    HostSession(const HostSession&) = delete;
    HostSession& operator=(const HostSession&) = delete;

    bool start(const Config& config, std::string* error);
    // `companion_quits_too`: the player quit from the companion's side, so the
    // Bye tells it to close its own app as well -- see encode_bye().
    void stop(bool companion_quits_too = false);
    bool running() const { return listener_.listening(); }

    bool has_companion() const { return companion_.connected() && handshake_done_; }
    const std::string& companion_name() const { return companion_name_; }
    ScreenId companion_screen() const { return companion_screen_; }
    // What the companion said it could decode. Tiles are always possible; video
    // needs a decoder at the far end and an encoder at this one.
    bool companion_accepts_video() const { return companion_accepts_video_; }
    // Whether the device connected now has a mouse pointer; false with none.
    bool companion_has_pointer() const { return has_companion() && companion_has_pointer_; }

    // How screens are sent. Automatic, the default, is decided here screen by
    // screen -- see screen_goes_as_video(). The other two pin one format for
    // a whole run, which is what --video-codec is for: comparing the two on
    // real hardware, or a test that needs to know which it is getting.
    enum class CodecChoice { Automatic, Tiles, H264 };
    void set_codec_choice(CodecChoice choice);
    // Whether a screen of this size goes as video right now. Asked by the
    // caller before reading a screen back, since video is worth reading at
    // everything the core rendered and tiles are sent at native size.
    bool sends_video(ScreenId screen, int width, int height) const;
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

        // The one menu, as this device has it. The companion draws from these
        // rather than keeping a menu of its own, which is what lets the player
        // work it from either pad while watching either screen.
        bool menu_open = false;
        int menu_focus = 0;
        uint8_t menu_confirm = 0;
        uint8_t menu_confirm_focus = 0;
        // See SessionStateMessage::menu_unavailable_notice.
        bool menu_unavailable_notice = false;
        // See SessionStateMessage::graphic_packs_available.
        bool graphic_packs_available = false;
        bool amiibo_available = false;
        std::vector<std::pair<std::string, std::string>> producer_options;
        std::vector<std::vector<std::string>> producer_choices;
        int producer_controls_from = -1;
        // See SessionStateMessage::wii_u_controllers.
        std::vector<ControllerRowView> wii_u_controllers;
        // See SessionStateMessage::button_mapping.
        ButtonMapping button_mapping;
        // See SessionStateMessage::favorite_color.
        int favorite_color = -1;
        // See SessionStateMessage::corner_notice.
        std::string corner_notice;
        // See SessionStateMessage::controller_notices.
        std::vector<ControllerNotice> controller_notices;
        std::vector<std::string> amiibo;
        std::string amiibo_folder;
        // The settings screen, which is this device's own rather than shared:
        // the companion is told only that it is up. See
        // SessionStateMessage::settings_open.
        bool settings_open = false;
        // What decides which entries that menu has, so the companion builds
        // the identical row and the two cursors mean the same thing.
        int max_render_scale = 1;
        bool cursor_choosable = false;
        bool right_stick_choosable = false;
        // See SessionStateMessage::left_stick_choosable.
        bool left_stick_choosable = false;
        LeftStickPress left_stick_press = LeftStickPress::Microphone;
        // See SessionStateMessage::rumble_choosable.
        bool rumble_choosable = false;
        bool rumble_pak = false;
        // See SessionStateMessage::lcd_filter.
        bool lcd_filter_choosable = false;
        bool lcd_filter = false;
        // See SessionStateMessage::custom_textures.
        bool custom_textures_choosable = false;
        bool custom_textures = false;
        bool cursor_shown = true;
        bool save_points_available = false;
        bool save_sync_available = false;
        // See SessionStateMessage::resume_choosable and resume_notice.
        bool resume_choosable = false;
        bool resume_from_save_state = false;
        bool resume_notice = false;
        // Which layouts are flipped for a left hand; `handedness` is the
        // layout in use's. See SessionStateMessage::left_handed_layouts.
        std::vector<std::string> left_handed_layouts;
    };

    // Cheap to call every frame; nothing is sent unless something changed.
    void set_session_state(const SharedState& state);

    // What is in each save point slot, for the companion's copy of the quick
    // menu. Sent once, and again to each companion that joins; call it again
    // only when something has changed.
    void set_save_slots(const SaveSlotsMessage& slots);

    // The Controller Order screen as it stands, for the companion to draw.
    // Cheap to call every frame while it is up; nothing goes out unless it
    // changed, and it goes again to a companion that joins.
    void set_controllers_view(const ControllersViewMessage& view);
    // The same for the Graphics Packs page.
    void set_graphic_packs_view(const GraphicPacksViewMessage& view);
    // And the amiibo browser, while it is up.
    void set_amiibo_browser_view(const AmiiboBrowserViewMessage& view);
    // Where the page both devices show is scrolled here: sent on change.
    void set_menu_scroll(const MenuScrollMessage& scroll);
    // Where the companion has scrolled it, newest only, once.
    bool take_menu_scroll(MenuScrollMessage& out);
    // The cursor on the page both devices show, as it is here: sent on
    // change. And the companion's, newest only, once.
    void set_menu_cursor(const MenuCursorMessage& cursor);
    bool take_menu_cursor(MenuCursorMessage& out);
    // Where the stylus on the right stick is: sent on change.
    void set_stylus_cursor(const StylusCursorMessage& cursor);
    // The Button Mapping page as it stands here (closed once it is not):
    // sent on change.
    void set_button_mapping_view(const ButtonMappingViewMessage& view);
    // A Wii U player's page, the same way.
    void set_wii_u_player_view(const WiiUPlayerViewMessage& view);
    // And the game's own dialog, up over it (kind 0 once it is not).
    void set_game_dialog(const GameDialogMessage& dialog);
    // The companion's answers to it, oldest first.
    bool take_game_dialog_answer(GameDialogAnswerMessage& out);
    // Asks the companion's own updater to check, or to install what it
    // found -- see UpdateRequestMessage. Nothing without one.
    void request_companion_update(UpdateRequest request);
    // What it said, newest only, once.
    bool take_companion_update_answer(UpdateAnswerMessage& out);

    // Sends one of the game's audio streams to the companion. DS and 3DS have
    // one soundtrack and always pass Shared; a Wii U's TV and GamePad are
    // independent and pass TopDevice/BottomDevice, each checked here against
    // that stream's own target rather than the coarse enabled flag below,
    // since the two can differ (TV muted, GamePad not, or the reverse).
    void publish_audio(AudioStreamId stream, const int16_t* interleaved_stereo, std::size_t frames,
                       uint32_t sample_rate);
    // Governs only the Shared stream -- see publish_audio.
    void set_audio_enabled(bool enabled) { audio_enabled_ = enabled; }

    // Changed-tile pictures encoded, and every picture and sound sent, on the
    // encoder thread rather than on the thread calling publish_screens() and
    // publish_audio() -- which then only copies the pixels. For a device whose
    // game thread cannot spare the time: on a Switch each datagram is a call
    // into the system, a busy DS picture is thirty of them plus 3ms of
    // compression, and a game there dropped from one picture a pass to one
    // every three frames once a companion joined. Off by default: where
    // sending is cheap, doing it in place is a thread handoff sooner.
    // Set before start().
    void set_media_off_thread(bool off_thread) { media_off_thread_ = off_thread; }
    bool audio_enabled() const { return audio_enabled_; }

    // Tells the companion whether the Wii U GamePad's motor is on right now,
    // so its own device can rumble in its place -- the companion holds the
    // GamePad's controls regardless of which screen it is showing (see
    // companion_wants()' own comment on menu vs. per-device layout for the
    // same distinction elsewhere), so this is sent whenever a companion is
    // connected at all, not only while it is displaying the GamePad screen.
    // Cheap to call every time the state changes, like publish_audio -- only
    // sent when it actually changed.
    void publish_rumble(bool active);
    // The game was reset: the companion takes down the picture it has until
    // the restarted game sends one -- see MessageType::ScreensCleared.
    void clear_companion_screens();
    // A menu sound this device just played, sent for the companion to play
    // as well -- see MessageType::MenuSound. Nothing without one.
    void publish_menu_sound(MenuSound sound);

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

    // A press the companion made in the menu, and where its finger was if that
    // is what made it. Queued rather than merged into the pending request like
    // the settings above: a setting is a value, where whatever was said last
    // wins, but a press is something that happened and two of them are two
    // moves. False when there are none left.
    struct MenuPress {
        bool has_focus = false;
        uint16_t focus = 0;
        bool has_action = false;
        MenuAction action = MenuAction::Confirm;
    };
    bool take_menu_press(MenuPress& out);

    // The companion has asked for the menu. Latched on the edge rather than
    // read as a level, because the host holds the one menu now: a request still
    // in flight when the menu closed would otherwise reopen it, and a level
    // that nobody ever cleared would keep the game paused forever.
    bool take_menu_open_request();

    // Set once when video was forced -- set_codec_choice(H264), which is
    // --video-codec h264 -- and then turns out not to work: no codec built
    // into this binary, the companion said it cannot decode, a screen size
    // 4:2:0 cannot carry, or the encoder itself failing to open or to encode a
    // frame. A forced choice has no fallback, since a run that asked for video
    // on purpose is asking whether it works. The automatic choice falls back
    // to tiles instead and never sets this. Latched the same way
    // take_menu_open_request() is above -- once per failure that starts, not
    // once per frame it keeps failing on -- and cleared the moment video works
    // again, so the caller can show it once rather than fighting to keep only
    // one copy on screen.
    bool take_video_error(std::string& out_reason);

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

    // Call after the game has run -- one frame or a catch-up batch of them --
    // with the serial of the picture that came out. Tells the companion which
    // of its inputs that picture is the first to include (see InputEcho), and
    // lets go of a touch the game had not yet seen when it was released: a tap
    // whose press and release both land between two frames would otherwise
    // reach the game as no tap at all.
    void note_frame_ran(uint64_t serial);

    // The companion's pads, each on its own, in its own order -- and whether
    // it reports them at all, which one built before this existed does not.
    // See InputMessage::pads.
    bool companion_reports_pads() const { return companion_reports_pads_; }
    const std::vector<ReportedPad>& companion_pads() const { return companion_pads_; }

    // Turns one of the companion's pads' motors on or off -- a player's
    // rumble, sent to whichever pad is that player. Only what changed goes
    // out, like publish_rumble(). Ignored for a companion that does not report
    // pads, which would read it as the whole device.
    void publish_pad_rumble(uint32_t pad, bool active, uint8_t strength = 100);
    // One short buzz of `ms` on one of the companion's pads, which the
    // companion ends by itself -- see RumbleMessage::ping_ms. Always sent,
    // and leaves what publish_pad_rumble() last said about the pad alone.
    void publish_pad_ping(uint32_t pad, uint16_t ms);

private:
    void refresh_beacon();
    void drop_companion(const std::string& reason);
    void handle_message(MessageType type, const std::vector<uint8_t>& payload);
public:
    // Total bytes ever handed to the media socket for a frame. Lets a test see
    // whether a frame was actually sent, which is what the ack-based backlog
    // limit decides -- UDP has no send queue of its own to inspect the way
    // the TCP connection this used to ride did.
    uint64_t queued_bytes_for_test() const {
        std::lock_guard<std::mutex> lock(media_mutex_);
        return media_bytes_sent_;
    }
    // How many frames have been sent that the companion has not yet said it
    // has, which is what the frame limit throttles on. Frames, not the gap
    // between two serials -- see the gate in publish_screens() for why those
    // are not the same number. A companion's own smoothing must not show up
    // here.
    uint64_t companion_frames_behind_for_test() const {
        return static_cast<uint64_t>(frame_sent_at_.size());
    }
    // How many times the companion has said it lost a frame and needed one
    // that stands on its own, since the last stream report.
    unsigned keyframes_asked_for_test() const { return stats_keyframe_requests_; }
    // Whether the media channel's own address handshake has completed -- see
    // poll(). True some time after has_companion() (an extra UDP round trip,
    // negligible on a real LAN but real on a test's synthetic clock), so a
    // test publishing a screen or audio immediately after "connected" should
    // wait for this too, the same way the real app's every-frame retry
    // naturally would without ever having to check for it explicitly.
    bool companion_media_ready_for_test() const { return companion_media_known_; }

private:
    // A datagram off the media channel, already past the session-token check
    // -- see poll(). Frame and Audio are host->companion only, so one
    // arriving here is ignored; Input and FrameAck are this direction's real
    // traffic.
    void handle_media_datagram(MessageType type, const uint8_t* payload, std::size_t payload_size);
    // Splits one screen's frame payload across as many UDP datagrams as it
    // takes -- see kMaxMediaDatagramBytes -- and queues each one onto
    // pending_media_sends_ rather than sending it immediately (see poll()'s
    // own comment on why). Returns the number of bytes queued, for
    // queued_bytes_for_test() and stats_sent_ -- an approximation once
    // pacing is involved, since those bytes have not necessarily left the
    // machine yet, but the closest thing either of those has ever had to
    // track and not worth a separate, more precise accounting just for them.
    std::size_t send_frame_fragments(const FrameMessage& frame);
    // Whether the oldest frame still waiting on an ack has waited so long
    // that it, and everything sent after it, is taken for lost -- in which
    // case the baselines are dropped and the next publish sends everything
    // afresh. See ack_stall_ms().
    bool give_up_on_stalled_frames();
    // Set by an ack that arrived and still did not cover a frame sent well
    // before it -- see kLostAfterRoundTrips. Read, and cleared, by the above.
    bool in_flight_frames_lost_ = false;
    // How many frames have actually gone out on each screen, wrapping. Only
    // ever incremented by send_frame_fragments(); a companion reads a gap in
    // it as loss, so anything that made it count something that did not leave
    // would have the far end asking for keyframes it does not need.
    uint16_t sent_index_top_ = 0;
    uint16_t sent_index_bottom_ = 0;
    // Hands pending_media_sends_ to the socket, at a rate tied to the
    // currently configured encode bitrate rather than a fixed number of
    // bytes per call -- see its own definition for why a fixed number could
    // not stay right once resolution capping and per-resolution bitrate made
    // the encoders' own output rate no longer a constant this file picks.
    void drain_pending_media_sends(uint64_t now_ms);

    Config config_;
    TcpListener listener_;
    HostAnnouncer announcer_;
    // Video, input and frame-acks -- everything on the latency-critical path
    // except audio -- ride here instead of the TCP connection below. See
    // kMediaPort's own comment for why the two are kept apart, and
    // audio_media_socket_'s own comment for why audio is a third, separate
    // channel rather than sharing this one.
    UdpSocket media_socket_;
    // Audio is sent from its own socket rather than media_socket_ above so a
    // video keyframe burst can never make it silently disappear. Video
    // fragments are paced through pending_media_sends_/drain_pending_media_sends()
    // over time, but within a single drain call they are still handed to the
    // kernel back to back with no gap, and a keyframe -- several times the
    // size of an ordinary frame, recurring every couple of seconds by design
    // (see video_encoder.cpp's gop_size) -- can be large enough to fill
    // media_socket_'s own send buffer for a moment. publish_audio() is called
    // from Cemu's real-time audio thread, independently of and concurrently
    // with that drain, and its sendto() is fire-and-forget: a full send
    // buffer fails it outright with nothing retried, which reads on the
    // companion's end as exactly the kind of audio pop this was built to
    // avoid. A dedicated socket gives audio its own send buffer that a video
    // burst can never fill, so an audio packet is never lost to video traffic
    // congestion. It needs no port of its own -- see start()'s own comment.
    UdpSocket audio_media_socket_;
    // Generated fresh whenever a companion's handshake completes and sent to
    // it in WelcomeMessage; every media datagram either side sends carries it,
    // so a stray or late one from a different session is silently ignored
    // rather than acted on -- UDP has no connection of its own to say that for
    // it the way the TCP stream below does.
    uint64_t session_token_ = 0;
    // The companion's own media socket is not on a fixed port the way this
    // one is, so its address is learned from the first datagram it sends
    // rather than agreed on in advance -- see poll().
    std::string companion_media_address_;
    uint16_t companion_media_port_ = 0;
    bool companion_media_known_ = false;
    // See queued_bytes_for_test().
    uint64_t media_bytes_sent_ = 0;
    // Fully framed datagrams (media header and fragment header both already
    // written) waiting to actually go out -- see poll()'s own comment on why
    // this exists rather than send_frame_fragments() just calling send_to()
    // itself. No cap of its own: kMaxQueuedFrames already bounds how many
    // frames' worth of these can ever be waiting at once, by refusing to
    // queue a new one until earlier ones are acked.
    std::deque<std::vector<uint8_t>> pending_media_sends_;
    // The last few video frames' own pieces, as they went out, so a piece the
    // companion says never arrived can be sent again -- see resend_pieces().
    struct SentVideoFrame {
        ScreenId screen = ScreenId::Top;
        uint64_t serial = 0;
        uint16_t sent_index = 0;
        std::vector<std::vector<uint8_t>> pieces;
    };
    std::deque<SentVideoFrame> sent_video_frames_;
    // Puts the pieces asked for back on the wire, ahead of everything queued.
    void resend_pieces(const RepairRequestMessage& request);
    unsigned stats_pieces_resent_ = 0;
    unsigned stats_repairs_asked_ = 0;
    // When drain_pending_media_sends() last ran, and the encoders' own
    // configured rate at that point -- see its own comment for why the drain
    // rate tracks these instead of being a flat constant. Zero means it has
    // never run (or a companion just arrived, see drop_companion()): the
    // first call after that starts the clock rather than assuming whatever
    // time has passed since the process started was all fair game to send in.
    uint64_t media_pacing_last_ms_ = 0;
    // Where the host's own share of the delay goes: how long encoding a frame
    // takes, and how much time is sitting in the pacing queue waiting to go
    // out. Reported once a second alongside the rest -- see report_stream().
    uint64_t stats_encode_total_ms_ = 0;
    unsigned stats_encodes_ = 0;
    uint64_t stats_pacing_worst_ms_ = 0;
    // How many times the companion said it had lost a frame and needed one
    // that stands on its own. Reported alongside the rest because it is the
    // one number that says whether a long keyframe interval is costing
    // anything: a link losing nothing shows zero here, and the periodic
    // keyframe burst it no longer pays for is real latency saved.
    unsigned stats_keyframe_requests_ = 0;
    // The same count as of the last report, so the line above can say how
    // many arrived in that second while the counter itself keeps running for
    // the whole session -- which is what keyframes_asked_for_test() reads,
    // and what stops a report landing mid-test from erasing what it is
    // waiting to see.
    unsigned stats_keyframe_requests_reported_ = 0;
    // Video keyframes that went out this second, asked for or not.
    unsigned stats_keyframes_sent_ = 0;
    // Set when a companion asks for a keyframe on that screen (see
    // handle_media_datagram) and cleared by the encode that answers it.
    bool keyframe_asked_top_ = false;
    bool keyframe_asked_bottom_ = false;
    // Set in try_encode_video() whenever that screen's encoder (re)opens, so
    // drain_pending_media_sends() knows what rate the data it is draining is
    // actually arriving at. Zero when that screen has never opened a video
    // encoder (tiles, or nothing sent yet).
    uint32_t video_bitrate_kbps_top_ = 0;
    uint32_t video_bitrate_kbps_bottom_ = 0;
    // What the encoders are really handing over, in bytes per millisecond,
    // and the window it is being measured across. The pacer's rate comes from
    // this rather than from the two numbers above -- see
    // media_pacing_bytes_per_ms().
    // The size of the frames actually sent, since the last report.
    //
    // A stream whose frames are all much of a size puts a steady trickle on
    // the wire; one whose frames swing several to one puts lumps on it, and
    // the frame queued behind a lump waits for it -- which is where a tail of
    // late arrivals comes from on a link that is losing nothing at all. The
    // far end can measure that tail and cannot see what caused it, so the
    // cause is reported from this end.
    std::size_t stats_frame_bytes_worst_ = 0;
    uint64_t stats_frame_bytes_total_ = 0;
    uint64_t stats_frames_counted_ = 0;
    double media_produced_bytes_per_ms_ = 0.0;
    uint64_t media_produced_window_bytes_ = 0;
    uint64_t media_produced_window_at_ms_ = 0;
    void note_produced_media_rate(uint64_t now_ms);
    // One counter per AudioStreamId, so the companion can tell a genuinely
    // missing packet from ordinary jitter -- see AudioMessage::sequence.
    // Indexed by static_cast<uint8_t>(stream); 3 entries because that enum
    // has exactly Shared/TopDevice/BottomDevice.
    uint32_t audio_sequence_[3] = {0, 0, 0};

    TcpStream companion_;
    std::vector<uint8_t> inbox_;
    bool handshake_done_ = false;
    std::string companion_name_;
    // The encoding of one screen's frame -- tiles if that is what was asked
    // for, otherwise video with nothing standing in for it if video does not
    // actually work; see take_video_error(). try_encode_video() is the half
    // of that which can fail, with why in `reason` -- empty means it simply
    // had nothing new to send, which is not a failure.
    //
    // Video goes to the encoder thread rather than coming back here: see
    // encoder_loop(). Queued means it is on its way and will be sent from
    // there the moment it is encoded.
    enum class Encoded { Nothing, Ready, Queued };
    Encoded encode_one_screen(ScreenId screen,
                              const ScreenPixels& source,
                              std::vector<uint8_t>& previous,
                              FrameMessage& frame);
    // Video that cannot go -- before it is queued, or after the encoder
    // thread tried -- handled the one way wherever it is found out.
    Encoded video_failed(ScreenId screen, const std::string& reason);

    // One screen's picture for the encoder thread, and everything about the
    // session it needs to encode it, copied: the thread touches nothing of
    // the session's own but what media_mutex_ guards.
    struct VideoJob {
        bool close = false;  // close this screen's encoder instead
        ScreenId screen = ScreenId::Top;
        uint64_t serial = 0;
        uint64_t generation = 0;
        std::vector<uint32_t> pixels;  // `width` x `height`, packed
        int width = 0;                 // even, cropped from the source
        int height = 0;
        uint16_t frame_width = 0;      // the source's own, for the frame header
        uint16_t frame_height = 0;
        PixelChannelOrder channel_order = PixelChannelOrder::XrgbBytes;
        int encode_width = 0;
        int encode_height = 0;
        double produced_fps = 60.0;
        bool force_keyframe = false;
        // Changed tiles rather than video -- see set_media_off_thread(). The
        // pixels are then the source's own size, `baseline` the picture the
        // tiles are measured against (the new one once encoded) and
        // `also_unlike` the ones still in flight.
        bool tiles = false;
        bool keyframe = false;
        std::vector<uint8_t> baseline;
        std::vector<std::vector<uint8_t>> also_unlike;
    };
    // A tiles job the encoder thread has finished, for this thread to keep
    // the new picture of as in flight -- see take_finished_tiles().
    struct FinishedTiles {
        ScreenId screen = ScreenId::Top;
        uint64_t serial = 0;
        std::vector<uint8_t> snapshot;
        std::size_t bytes = 0;
        bool sent = false;
    };
    // Queues this screen's picture as a tiles job. `previous` is the baseline
    // it is measured against, taken.
    void queue_tiles(ScreenId screen, const ScreenPixels& source, uint64_t serial, bool keyframe,
                     std::vector<uint8_t>& previous);
    // On the encoder thread; called unlocked, returns with `lock` held.
    void encode_tiles_job(VideoJob& job, std::unique_lock<std::mutex>& lock);
    // Keeps what the encoder thread's tiles jobs made as in flight, on this
    // thread, which owns the snapshots.
    void take_finished_tiles();
    // Counts what a screen's tiles came to, for the codec choice.
    void note_tile_bytes(ScreenId screen, std::size_t bytes);
    // How much of the send queue may go now -- see drain_pending_media_sends().
    // Zero when it is empty. Under media_mutex_.
    std::size_t media_drain_budget(uint64_t now_ms);
    // On the encoder thread: the queued sound, then as much of the queued
    // pictures as pacing allows, sent with media_mutex_ let go.
    void send_media_off_thread(std::unique_lock<std::mutex>& lock);
    // The checks that need nothing but this thread's own state, then the
    // job. False with `reason` when video cannot go at all.
    bool queue_video(ScreenId screen, const ScreenPixels& source, uint64_t serial,
                     std::string& reason);
    // On the encoder thread: the encode itself. `opened_kbps` is the bitrate
    // an encoder was (re)opened at, zero if none was.
    bool encode_video_job(const VideoJob& job, FrameMessage& frame, std::string& reason,
                          uint32_t& opened_kbps);
    void encoder_loop();
    void start_encoder_thread();
    void stop_encoder_thread();
    // What the encoder thread found, applied on this one: see video_failed().
    void take_video_outcomes();

    ScreenId companion_screen_ = ScreenId::Bottom;
    std::string last_notice_;

    SharedState shared_;
    bool session_state_dirty_ = true;
    SaveSlotsMessage save_slots_;
    bool save_slots_known_ = false;
    bool save_slots_dirty_ = false;
    // Encoded, so a change is any change: compared byte for byte.
    std::vector<uint8_t> controllers_view_;
    bool controllers_view_dirty_ = false;
    std::vector<uint8_t> graphic_packs_view_;
    bool graphic_packs_view_dirty_ = false;
    std::vector<uint8_t> game_dialog_;
    bool game_dialog_dirty_ = false;
    std::vector<uint8_t> amiibo_browser_view_;
    bool amiibo_browser_view_dirty_ = false;
    std::vector<uint8_t> menu_scroll_;
    bool menu_scroll_dirty_ = false;
    std::optional<MenuScrollMessage> companion_menu_scroll_;
    std::vector<uint8_t> menu_cursor_;
    bool menu_cursor_dirty_ = false;
    std::optional<MenuCursorMessage> companion_menu_cursor_;
    std::vector<uint8_t> stylus_cursor_;
    bool stylus_cursor_dirty_ = false;
    std::vector<uint8_t> button_mapping_view_;
    bool button_mapping_view_dirty_ = false;
    std::vector<uint8_t> wii_u_player_view_;
    bool wii_u_player_view_dirty_ = false;
    std::deque<GameDialogAnswerMessage> game_dialog_answers_;
    std::optional<UpdateAnswerMessage> companion_update_answer_;

    DsInputState companion_input_;
    bool companion_input_fresh_ = false;
    // The newest input numbered by the companion that has been accepted; an
    // older one, or the same one again, is a copy -- see
    // CompanionSession::send_input() -- and is dropped.
    bool input_sequence_known_ = false;
    uint32_t input_sequence_ = 0;
    // The one companion_input_ currently holds, which is what the game sees
    // -- not the newest, while a release is waiting below.
    uint32_t applied_sequence_ = 0;
    bool echoed_any_ = false;
    uint32_t echoed_sequence_ = 0;
    // A touch that has gone down since the game last ran, and the release
    // that arrived before it did -- held back until one frame has seen the
    // touch. See note_frame_ran().
    bool touch_unseen_ = false;
    bool release_waiting_ = false;
    DsInputState released_input_;
    uint32_t released_sequence_ = 0;
    uint64_t sent_serial_ = 0;
    // The newest serial handed to the encoder, which is not the same as the
    // newest one sent: the encoder pipelines, so a frame goes in one call and
    // its bytes come out of a later one. This is what decides whether there is
    // anything new to publish; sent_serial_ is what the ack gate measures.
    uint64_t submitted_serial_ = 0;
    // Which serial each frame still inside an encoder belongs to, keyed by the
    // pts the encoder gave it -- see try_encode_video().
    std::deque<std::pair<int64_t, uint64_t>> encode_serials_top_;
    std::deque<std::pair<int64_t, uint64_t>> encode_serials_bottom_;
    bool audio_enabled_ = true;
    bool rumble_active_ = false;
    bool have_sent_rumble_ = false;
    bool companion_reports_pads_ = false;
    std::vector<ReportedPad> companion_pads_;
    // Which of the companion's pads were last told their motor is on.
    // How hard each pad was last told to rumble, in percent; 0 for off.
    std::map<uint32_t, int> pad_rumble_sent_;
    CodecChoice codec_choice_ = CodecChoice::Automatic;
    // One per screen: which it is going as, since when, and what tiles have
    // been costing it -- see screen_goes_as_video().
    struct CodecState {
        bool video = false;
        bool video_for_weight = false;
        uint64_t since_ms = 0;
        uint64_t tile_bytes = 0;
        uint64_t tile_window_start_ms = 0;
        double tile_kbytes_per_second = 0.0;
    };
    CodecState codec_top_;
    CodecState codec_bottom_;
    CodecEvidence codec_evidence(ScreenId screen, int width, int height) const;
    bool video_possible() const;
    // Switches a screen's format, doing what each direction needs: a
    // keyframe into video, a whole picture back into tiles.
    void switch_codec(ScreenId screen, bool video, const char* why);
    // Zero means video is not backed off. Set to a future monotonic_millis()
    // after any reason it does not work, so a transient one (a cold hardware
    // encoder, one bad frame) is not retried and reported 60 times a second
    // -- see encode_one_screen() and try_encode_video().
    uint64_t video_retry_at_ms_ = 0;
    // Whether the current failure, if there is one, has already been handed
    // out by take_video_error() -- see its own comment for why this is once
    // per failure rather than once per retry.
    bool video_error_reported_ = false;
    std::string pending_video_error_;
    bool has_pending_video_error_ = false;
    bool companion_menu_open_ = false;
    bool companion_asked_for_menu_ = false;
    int companion_display_width_ = 0;
    int companion_display_height_ = 0;
    // How far ahead of what the companion has acknowledged decoding this end
    // is allowed to get before a new frame is dropped rather than sent -- see
    // kMaxQueuedFrames. UDP has no send queue of its own to also bound the
    // way the old TCP-based backstop did (frames_in_flight(), removed with
    // it): a datagram is either sent now or not at all, so this ack-based
    // limit is the only pacing this needs.
    uint64_t acked_serial_ = 0;

    // Once-a-second streaming summary, for working out on real hardware where
    // a slow stream is actually being lost. Debug level, so --verbose asks for
    // it and an ordinary run is not filled with it.
    void report_stream(uint64_t now_ms);
    std::string encode_cost_summary();
    // When each queued frame was sent, so the ack for it can be timed. The
    // one number none of the rest of this can give: how long a frame actually
    // takes to get there and be decoded, measured on one clock.
    std::deque<std::pair<uint64_t, uint64_t>> frame_sent_at_;
    // What the window is computed from: the round trip this link is actually
    // showing, as a high-water mark that decays, and how fast frames are
    // really being produced. See frames_in_flight_allowed().
    double round_trip_high_ms_ = 0.0;
    // The quickest round trip lately, and a decaying high-water mark of how
    // much of each trip beyond that was spent on the way back rather than on
    // the way out -- see frames_in_flight_allowed().
    double round_trip_floor_ms_ = 0.0;
    double return_delay_high_ms_ = 0.0;
    double stats_return_delay_worst_ms_ = 0.0;
    double produce_period_ms_ = 16.7;
    uint64_t last_produced_at_ms_ = 0;
    unsigned stats_round_trips_ = 0;
    uint64_t stats_round_trip_total_ms_ = 0;
    uint64_t stats_round_trip_worst_ms_ = 0;
    uint64_t stats_at_ms_ = 0;
    uint64_t stats_bytes_ = 0;
    unsigned stats_produced_ = 0;
    // The serial last counted into the above, so a frame offered again is
    // not counted twice.
    uint64_t last_produced_serial_ = 0;
    unsigned stats_sent_ = 0;
    // The last frame of each screen the companion has actually acknowledged
    // decoding, in RGB565 -- what a new tile diff is computed against. The
    // frame *acked* rather than the one sent: over UDP a diff can simply
    // never arrive, and diffing the next one against a frame that was never
    // received patches tiles onto whatever state the companion actually has,
    // not the one this end assumes -- a picture that silently desyncs and
    // never recovers, since every diff after the lost one keeps being computed
    // against ground that was never laid. Rebaselining onto what is actually
    // confirmed instead means a lost diff costs one stale-looking frame, not
    // the rest of the session; the tradeoff is that a still-unacked send has
    // to be recomputed from this same confirmed baseline rather than building
    // incrementally on the one before it, which the fresh-per-send copy in
    // publish_screens() (see tile_snapshots_top_/bottom_ below) is for.
    // Cleared when a companion comes or goes, so the next one starts from a
    // whole frame.
    std::vector<uint8_t> previous_top_;
    std::vector<uint8_t> previous_bottom_;
    // One entry per tile frame sent but not yet acked, holding the full
    // RGB565 state that frame's diff was computed against and left the
    // companion in -- what previous_top_/bottom_ above is promoted to once
    // its ack arrives (see handle_media_datagram()'s FrameAck case), and what
    // is thrown away, unpromoted, for every serial at or before it once that
    // happens: those are superseded, since the diff that reached this serial
    // already accounts for everything they would have.  Bounded by
    // kMaxQueuedFrames, the most frames this end ever lets itself have
    // outstanding at once.
    std::deque<std::pair<uint64_t, std::vector<uint8_t>>> tile_snapshots_top_;
    std::deque<std::pair<uint64_t, std::vector<uint8_t>>> tile_snapshots_bottom_;
    // One per screen: the two are separate streams at separate sizes, and a
    // video encoder's whole value is what it remembers of the frame before.
    video::VideoEncoder video_top_;
    video::VideoEncoder video_bottom_;

    // The encoder thread, and what it shares with the thread that runs the
    // game.
    //
    // Video used to be encoded on the game's thread, which could not afford
    // to wait for it: on a game that keeps the GPU busy the wait was 17ms a
    // frame (Bayonetta 2), so the encoder was run a frame deep instead --
    // a picture going in and the previous one coming out -- which is a whole
    // frame of latency on every picture. Sunshine encodes on a thread of its
    // own and runs VAAPI with nothing queued, and so does this now: the
    // game's thread copies the picture and moves on, and this thread encodes
    // it, waits for it and sends it the moment it is done.
    //
    // media_mutex_ guards everything both touch: the send queue and its
    // pacing, send_frame_fragments()'s counters and kept pieces, the bitrates,
    // the session's token and the companion's address (written only under it,
    // so a send never reads half of one), and the jobs and what came of them.
    // The encoders themselves belong to the encoder thread alone.
    mutable std::mutex media_mutex_;
    std::condition_variable video_jobs_cv_;
    std::deque<VideoJob> video_jobs_;
    struct VideoFailure {
        ScreenId screen = ScreenId::Top;
        std::string reason;
    };
    std::vector<VideoFailure> video_failures_;
    bool video_worked_ = false;
    // Bumped whenever a companion goes, so a picture the encoder thread was
    // in the middle of for it is not sent to whoever comes next.
    uint64_t media_generation_ = 0;
    std::thread encoder_thread_;
    bool encoder_stopping_ = false;
    // See set_media_off_thread().
    std::atomic<bool> media_off_thread_{false};
    // Tiles jobs queued and not yet taken back, per screen (top, bottom). A
    // screen's next picture waits for its last: the tiles it sends are
    // measured against every picture still in flight, and that one is not
    // known to be until it comes back. This thread's alone.
    int tile_jobs_outstanding_[2] = {0, 0};
    std::vector<FinishedTiles> finished_tiles_;
    // Sound waiting for the encoder thread to send it -- see
    // set_media_off_thread().
    std::deque<std::vector<uint8_t>> audio_sends_;
    // Held while the encoder thread sends with media_mutex_ let go, and while
    // anything changes which address the sockets send from.
    std::mutex send_mutex_;
    // A screen that has just switched to video: its next picture stands alone.
    bool video_keyframe_owed_top_ = false;
    bool video_keyframe_owed_bottom_ = false;
    // What encoding cost, per screen, gathered by the encoder thread for the
    // once-a-second report.
    video::VideoEncoder::EncodeCost encode_cost_top_;
    video::VideoEncoder::EncodeCost encode_cost_bottom_;
    bool companion_accepts_video_ = false;
    bool companion_has_pointer_ = false;
    // Send the current screens once more even if nothing has changed, because
    // what the companion needs has.
    bool resend_screens_ = true;
    bool have_request_ = false;
    RequestMessage request_;
    // Bounded: a companion that somehow floods presses backs up the menu it is
    // working rather than this machine's memory.
    static constexpr std::size_t kMaxQueuedMenuPresses = 32;
    std::deque<MenuPress> menu_presses_;
    bool companion_wants_quit_ = false;
};

}  // namespace ds::net
