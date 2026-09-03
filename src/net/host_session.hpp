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
    // What the companion said it could decode. Tiles are always possible; video
    // needs a decoder at the far end and an encoder at this one.
    bool companion_accepts_video() const { return companion_accepts_video_; }

    // Ask for a compressed video stream where both ends can manage one. Off by
    // default: every system this runs today has flat-shaded screens that the
    // tile codec carries in a fraction of the bytes. See
    // docs/research-notes.md.
    void set_video_wanted(bool wanted);
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
        // The settings screen, which is this device's own rather than shared:
        // the companion is told only that it is up. See
        // SessionStateMessage::settings_open.
        bool settings_open = false;
        // What decides which entries that menu has, so the companion builds
        // the identical row and the two cursors mean the same thing.
        int max_render_scale = 1;
        bool cursor_choosable = false;
        bool right_stick_choosable = false;
        bool cursor_shown = true;
    };

    // Cheap to call every frame; nothing is sent unless something changed.
    void set_session_state(const SharedState& state);

    // Sends one of the game's audio streams to the companion. DS and 3DS have
    // one soundtrack and always pass Shared; a Wii U's TV and GamePad are
    // independent and pass TopDevice/BottomDevice, each checked here against
    // that stream's own target rather than the coarse enabled flag below,
    // since the two can differ (TV muted, GamePad not, or the reverse).
    void publish_audio(AudioStreamId stream, const int16_t* interleaved_stereo, std::size_t frames,
                       uint32_t sample_rate);
    // Governs only the Shared stream -- see publish_audio.
    void set_audio_enabled(bool enabled) { audio_enabled_ = enabled; }
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
        uint8_t focus = 0;
        bool has_action = false;
        MenuAction action = MenuAction::Confirm;
    };
    bool take_menu_press(MenuPress& out);

    // The companion has asked for the menu. Latched on the edge rather than
    // read as a level, because the host holds the one menu now: a request still
    // in flight when the menu closed would otherwise reopen it, and a level
    // that nobody ever cleared would keep the game paused forever.
    bool take_menu_open_request();

    // Set once when video was asked for -- see set_video_wanted() -- and then
    // turns out not to work: no codec built into this binary, the companion
    // said it cannot decode, a screen size 4:2:0 cannot carry, or the encoder
    // itself failing to open or to encode a frame. There is no fallback to
    // report around: see ScreenCodec's own comment for why sending tiles
    // instead, silently, is exactly what this is for telling apart from
    // working video. Latched the same way take_menu_open_request() is above --
    // once per failure that starts, not once per frame it keeps failing on --
    // and cleared the moment video works again, so the caller can show it once
    // rather than fighting to keep only one copy on screen.
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

private:
    void refresh_beacon();
    void drop_companion(const std::string& reason);
    void handle_message(MessageType type, const std::vector<uint8_t>& payload);
public:
    // Total bytes ever handed to the media socket for a frame. Lets a test see
    // whether a frame was actually sent, which is what the ack-based backlog
    // limit decides -- UDP has no send queue of its own to inspect the way
    // the TCP connection this used to ride did.
    uint64_t queued_bytes_for_test() const { return media_bytes_sent_; }
    // How many frames have been sent that the companion has not yet said it
    // has, which is what the frame limit throttles on. Frames, not the gap
    // between two serials -- see the gate in publish_screens() for why those
    // are not the same number. A companion's own smoothing must not show up
    // here.
    uint64_t companion_frames_behind_for_test() const {
        return static_cast<uint64_t>(frame_sent_at_.size());
    }
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
    // Set in try_encode_video() whenever that screen's encoder (re)opens, so
    // drain_pending_media_sends() knows what rate the data it is draining is
    // actually arriving at. Zero when that screen has never opened a video
    // encoder (tiles, or nothing sent yet).
    uint32_t video_bitrate_kbps_top_ = 0;
    uint32_t video_bitrate_kbps_bottom_ = 0;
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
    bool encode_one_screen(ScreenId screen,
                           const ScreenPixels& source,
                           std::vector<uint8_t>& previous,
                           FrameMessage& frame);
    bool try_encode_video(ScreenId screen, const ScreenPixels& source, FrameMessage& frame,
                          std::string& reason);

    ScreenId companion_screen_ = ScreenId::Bottom;
    std::string last_notice_;

    SharedState shared_;
    bool session_state_dirty_ = true;

    DsInputState companion_input_;
    bool companion_input_fresh_ = false;
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
    // When each queued frame was sent, so the ack for it can be timed. The
    // one number none of the rest of this can give: how long a frame actually
    // takes to get there and be decoded, measured on one clock.
    std::deque<std::pair<uint64_t, uint64_t>> frame_sent_at_;
    unsigned stats_round_trips_ = 0;
    uint64_t stats_round_trip_total_ms_ = 0;
    uint64_t stats_round_trip_worst_ms_ = 0;
    uint64_t stats_at_ms_ = 0;
    uint64_t stats_bytes_ = 0;
    unsigned stats_produced_ = 0;
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
    bool companion_accepts_video_ = false;
    // Send the current screens once more even if nothing has changed, because
    // what the companion needs has.
    bool resend_screens_ = true;
    bool video_wanted_ = false;
    bool have_request_ = false;
    RequestMessage request_;
    // Bounded: a companion that somehow floods presses backs up the menu it is
    // working rather than this machine's memory.
    static constexpr std::size_t kMaxQueuedMenuPresses = 32;
    std::deque<MenuPress> menu_presses_;
    bool companion_wants_quit_ = false;
};

}  // namespace ds::net
