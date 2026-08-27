#include "net/host_session.hpp"

#include <algorithm>

#include "util/log.hpp"

namespace ds::net {
namespace {

// If the companion cannot keep up, dropping frames is the right failure: the
// alternative is an ever-growing queue and input that lags further behind.
//
// Counted in frames, not bytes. Bytes were a stand-in for frames back when a
// frame was the whole picture and they were all much of a size; once frames
// became differences from the last one they stopped being anything of the
// sort. A DS screen with little moving on it compresses to well under a
// hundred bytes, so a byte limit written to hold two or three frames was
// holding well over a thousand -- which is not a backlog limit at all, it is
// half a minute of delay that no frame is ever dropped to relieve.
//
// What the limit is really about is how far behind what the player sees is
// allowed to fall, and that has always been a number of frames.
//
// Eight rather than two or three, because the companion's answer takes a
// moment to come back and nothing may be sent while the host waits for it. At
// most this many frames can be outstanding per round trip, so a tight number
// stops being a backlog limit and becomes a frame rate cap: three frames
// against a 50 ms wifi round trip is 60fps exactly, with nothing left for
// jitter. Eight leaves the pipeline full on a link like that and still bounds
// how far behind the picture can fall at about an eighth of a second.
constexpr std::size_t kMaxQueuedFrames = 8;

// Audio still goes by bytes, because a sound packet is a fixed span of time
// and its size does not wander. This is a little under a fifth of a second of
// 48 kHz stereo.
constexpr std::size_t kMaxQueuedAudioBytes = 32 * 1024;

}  // namespace

bool HostSession::start(const Config& config, std::string* error) {
    stop();
    config_ = config;
    companion_screen_ = (config.host_screen == ScreenId::Top) ? ScreenId::Bottom : ScreenId::Top;

    if (!listener_.start(kSessionPort, error)) {
        return false;
    }
    std::string announce_error;
    if (!announcer_.start(&announce_error)) {
        // Discovery is a convenience; a session still works if a companion is
        // pointed at this address directly.
        log_warn("cannot announce this host on the network: " + announce_error);
    }
    refresh_beacon();
    log_info("hosting " + config_.game_title + " on port " + std::to_string(listener_.port()));
    return true;
}

void HostSession::stop() {
    if (companion_.open()) {
        std::vector<uint8_t> bye;
        encode_bye(bye);
        companion_.send(bye);
        std::string error;
        std::vector<uint8_t> ignored;
        companion_.pump(ignored, &error);  // best effort
    }
    companion_.close();
    listener_.stop();
    announcer_.stop();
    inbox_.clear();
    handshake_done_ = false;
    companion_name_.clear();
    companion_input_ = DsInputState();
    companion_input_fresh_ = false;
    // Not reset by drop_companion() like everything else above -- that is the
    // whole point of it -- so a fresh hosting session has to clear it itself.
    companion_wants_quit_ = false;
}

void HostSession::set_host_screen(ScreenId screen) {
    const ScreenId companion = (screen == ScreenId::Top) ? ScreenId::Bottom : ScreenId::Top;
    if (companion != companion_screen_) {
        companion_screen_ = companion;
        session_state_dirty_ = true;
        refresh_beacon();
    }
    config_.host_screen = screen;
}

void HostSession::set_session_state(const SharedState& state) {
    if (state.per_device_layout != shared_.per_device_layout ||
        state.menu_open != shared_.menu_open) {
        // The set of screens the companion needs depends on both: a menu draws
        // every layout as a preview, whichever device it was opened from.
        resend_screens_ = true;
    }
    if (state.layout_id != shared_.layout_id ||
        state.per_device_layout != shared_.per_device_layout || state.paused != shared_.paused ||
        state.bilinear_filtering != shared_.bilinear_filtering ||
        state.audio_target != shared_.audio_target || state.handedness != shared_.handedness ||
        state.right_stick_mode != shared_.right_stick_mode ||
        state.render_scale != shared_.render_scale ||
        state.screen_gap != shared_.screen_gap || state.menu_open != shared_.menu_open ||
        state.menu_focus != shared_.menu_focus || state.menu_confirm != shared_.menu_confirm ||
        state.max_render_scale != shared_.max_render_scale ||
        state.cursor_choosable != shared_.cursor_choosable ||
        state.right_stick_choosable != shared_.right_stick_choosable ||
        state.cursor_shown != shared_.cursor_shown) {
        shared_ = state;
        session_state_dirty_ = true;
    }
}

void HostSession::refresh_beacon() {
    BeaconMessage beacon;
    beacon.device_name = config_.device_name;
    beacon.game_title = config_.game_title;
    beacon.rom_sha1 = config_.rom_sha1;
    beacon.session_port = listener_.port() != 0 ? listener_.port() : kSessionPort;
    // Both screens are on offer until a companion takes one: the host adapts to
    // whichever it is asked for. Once paired, only the one already taken is
    // shown as unavailable.
    const bool free_slot = !companion_.open();
    beacon.top_available = free_slot;
    beacon.bottom_available = free_slot;
    announcer_.set_beacon(beacon);
}

void HostSession::drop_companion(const std::string& reason) {
    if (companion_.open()) {
        log_info("companion disconnected: " + reason);
        last_notice_ = reason;
    }
    companion_.close();
    inbox_.clear();
    handshake_done_ = false;
    companion_name_.clear();
    companion_input_ = DsInputState();
    companion_input_fresh_ = false;
    have_request_ = false;
    request_ = RequestMessage();
    menu_presses_.clear();
    companion_asked_for_menu_ = false;
    // Nobody is holding a menu open on a device that has gone.
    companion_menu_open_ = false;
    // Whoever connects next says its own size; a stale one from this
    // companion is worse than not knowing, since it might be wrong in either
    // direction.
    companion_display_width_ = 0;
    companion_display_height_ = 0;
    frame_marks_.clear();
    frame_sent_at_.clear();
    resend_screens_ = false;
    sent_serial_ = 0;
    acked_serial_ = 0;
    previous_top_.clear();
    previous_bottom_.clear();
    // The same reasoning as the two lines above, in video form: a stream's
    // frames only mean anything after the keyframe they descend from, so
    // whoever connects next needs a stream of their own rather than the middle
    // of somebody else's. Their answer about decoding video is theirs too.
    video_top_.close();
    video_bottom_.close();
    companion_accepts_video_ = false;
    refresh_beacon();
}

bool HostSession::companion_wants(ScreenId screen) const {
    if (!has_companion()) {
        return false;
    }
    // With its menu up the companion draws every layout in miniature, so it
    // needs both screens whatever the live layout is.
    if (companion_menu_open_) {
        return true;
    }
    // Under a per-device layout it only needs its own screen. Under any other it
    // draws the whole thing, so it needs both.
    if (shared_.per_device_layout) {
        return screen == companion_screen_;
    }
    return true;
}

void HostSession::handle_message(MessageType type, const std::vector<uint8_t>& payload) {
    switch (type) {
        case MessageType::Hello: {
            HelloMessage hello;
            if (!decode_hello(payload.data(), payload.size(), hello)) {
                drop_companion("that device sent something we could not read");
                return;
            }
            if (hello.protocol_version != kProtocolVersion) {
                RejectMessage reject;
                reject.reason = "This device is running a different version of dualscreen (theirs " +
                                std::to_string(hello.protocol_version) + ", ours " +
                                std::to_string(kProtocolVersion) + ").";
                std::vector<uint8_t> out;
                encode_reject(reject, out);
                companion_.send(out);
                std::string ignored_error;
                std::vector<uint8_t> ignored;
                companion_.pump(ignored, &ignored_error);
                drop_companion(reject.reason);
                return;
            }

            // The companion picked which screen it wants to be, so it gets it
            // and the host takes the other. Ignoring this was why "connect as
            // top screen" and "connect as bottom screen" behaved identically.
            companion_screen_ = hello.wanted_screen;
            config_.host_screen =
                (companion_screen_ == ScreenId::Top) ? ScreenId::Bottom : ScreenId::Top;
            companion_name_ = hello.device_name.empty() ? "companion" : hello.device_name;
            companion_accepts_video_ = hello.accepts_video;
            handshake_done_ = true;

            WelcomeMessage welcome;
            welcome.game_title = config_.game_title;
            welcome.rom_sha1 = config_.rom_sha1;
            welcome.granted_screen = companion_screen_;
            welcome.system = config_.system;
            welcome.audio_sample_rate = config_.audio_sample_rate;
            std::vector<uint8_t> out;
            encode_welcome(welcome, out);
            companion_.send(out);
            session_state_dirty_ = true;
            last_notice_ = companion_name_ + " connected";
            log_info(std::string("that device ") +
                     (companion_accepts_video_ ? "can decode video" : "cannot decode video"));
            log_info(companion_name_ + " connected as the " +
                     std::string(to_string(companion_screen_)) + " screen");
            refresh_beacon();
            break;
        }
        case MessageType::Input: {
            InputMessage input;
            if (!decode_input(payload.data(), payload.size(), input)) {
                drop_companion("that device sent malformed input");
                return;
            }
            companion_input_.buttons = input.buttons;
            companion_input_.touching = input.touching;
            // Clamped rather than trusted: these arrive from another machine,
            // and to this console's own touch screen rather than a DS's.
            const ScreenSize touch = system_profile(config_.system).screens.bottom;
            companion_input_.touch_x = std::min(std::max<int>(input.touch_x, 0), touch.width - 1);
            companion_input_.touch_y = std::min(std::max<int>(input.touch_y, 0), touch.height - 1);
            companion_input_.circle_x = input.circle_x;
            companion_input_.circle_y = input.circle_y;
            companion_input_.c_stick_x = input.c_stick_x;
            companion_input_.c_stick_y = input.c_stick_y;
            companion_input_fresh_ = true;
            break;
        }
        case MessageType::Request: {
            RequestMessage request;
            if (!decode_request(payload.data(), payload.size(), request)) {
                return;  // not worth dropping a session over
            }
            // Not one of the choices the app adopts and broadcasts back --
            // just the companion telling us how big a picture it can use.
            // Applied immediately rather than merged sticky-field style: the
            // companion resends this whenever it actually changes, so the
            // latest value is always the current one and there is nothing to
            // carry forward from a request that did not mention it.
            if (request.wants_display_size) {
                companion_display_width_ = request.display_width;
                companion_display_height_ = request.display_height;
            }
            // Held for the app to validate: the host owns these choices and
            // will not adopt a layout id it does not recognise.
            if (have_request_ && request.layout_id.empty()) {
                // Do not let a later request without a layout erase an earlier
                // one that had it; each field means only itself.
                request.layout_id = request_.layout_id;
            }
            if (have_request_ && !request.wants_filtering && request_.wants_filtering) {
                request.wants_filtering = true;
                request.bilinear_filtering = request_.bilinear_filtering;
            }
            if (have_request_ && !request.wants_audio_target && request_.wants_audio_target) {
                request.wants_audio_target = true;
                request.audio_target = request_.audio_target;
            }
            if (have_request_ && !request.wants_right_stick_mode &&
                request_.wants_right_stick_mode) {
                request.wants_right_stick_mode = true;
                request.right_stick_mode = request_.right_stick_mode;
            }
            if (have_request_ && !request.wants_handedness && request_.wants_handedness) {
                request.wants_handedness = true;
                request.handedness = request_.handedness;
            }
            if (have_request_ && !request.wants_render_scale && request_.wants_render_scale) {
                request.wants_render_scale = true;
                request.render_scale = request_.render_scale;
            }
            if (have_request_ && !request.wants_reset && request_.wants_reset) {
                request.wants_reset = true;
            }
            if (have_request_ && !request.wants_screen_gap && request_.wants_screen_gap) {
                request.wants_screen_gap = true;
                request.screen_gap = request_.screen_gap;
            }
            // Pulled out before the rest is merged: presses are events, and
            // sticky-merging one would make it happen again next time the
            // companion asked for anything at all.
            if (request.wants_menu_focus || request.wants_menu_action) {
                MenuPress press;
                press.has_focus = request.wants_menu_focus;
                press.focus = request.menu_focus;
                press.has_action = request.wants_menu_action;
                press.action = request.menu_action;
                if (menu_presses_.size() < kMaxQueuedMenuPresses) {
                    menu_presses_.push_back(press);
                }
                request.wants_menu_focus = false;
                request.wants_menu_action = false;
            }
            request_ = request;
            have_request_ = true;
            if (request.wants_quit) {
                // Latched immediately rather than left for take_request(): the
                // companion's Bye is typically already queued right behind
                // this in the same poll(), and drop_companion() below would
                // otherwise clear request_ before the app next reads it.
                companion_wants_quit_ = true;
            }
            if (request.menu_open && !companion_menu_open_) {
                companion_asked_for_menu_ = true;
            }
            if (request.menu_open != companion_menu_open_) {
                companion_menu_open_ = request.menu_open;
                // What it needs has just changed, and a paused game will not
                // produce a new frame to carry the change across.
                resend_screens_ = true;
                // A menu takes the companion's hands off the game. Whatever it
                // was holding when the menu opened would otherwise stay held
                // for as long as the menu is up, since only changes are sent.
                if (companion_menu_open_) {
                    companion_input_ = DsInputState();
                    companion_input_fresh_ = true;
                }
            }
            break;
        }
        case MessageType::FrameAck: {
            uint64_t serial = 0;
            if (decode_frame_ack(payload.data(), payload.size(), serial)) {
                acked_serial_ = std::max(acked_serial_, serial);
                // Everything up to the acked frame has arrived, so time the
                // newest of them and discard the rest.
                const uint64_t now = monotonic_millis();
                while (!frame_sent_at_.empty() && frame_sent_at_.front().first <= serial) {
                    const uint64_t sent_at = frame_sent_at_.front().second;
                    frame_sent_at_.pop_front();
                    if (frame_sent_at_.empty() || frame_sent_at_.front().first > serial) {
                        const uint64_t trip = now - sent_at;
                        ++stats_round_trips_;
                        stats_round_trip_total_ms_ += trip;
                        stats_round_trip_worst_ms_ = std::max(stats_round_trip_worst_ms_, trip);
                    }
                }
            }
            break;
        }
        case MessageType::Bye:
            drop_companion("the other device left");
            break;
        case MessageType::Ping:
            break;
        default:
            // A message we do not expect from a companion. Ignored rather than
            // fatal, so a newer companion sending extra chatter still works.
            break;
    }
}

void HostSession::poll(uint64_t now_ms) {
    announcer_.poll(now_ms);
    if (has_companion()) {
        report_stream(now_ms);
    }

    if (!companion_.open()) {
        TcpStream incoming;
        if (listener_.accept(incoming)) {
            companion_ = std::move(incoming);
            inbox_.clear();
            handshake_done_ = false;
            // Whatever the last companion was sent means nothing to this one.
            previous_top_.clear();
            previous_bottom_.clear();
            log_info("a device is connecting from " + companion_.peer_address() + ", to this machine at " + companion_.local_address());
            refresh_beacon();
        }
    }

    if (!companion_.open()) {
        return;
    }

    if (session_state_dirty_ && handshake_done_) {
        SessionStateMessage state;
        state.layout_id = shared_.layout_id;
        state.companion_screen = companion_screen_;
        state.paused = shared_.paused;
        state.bilinear_filtering = shared_.bilinear_filtering;
        state.audio_target = shared_.audio_target;
        state.handedness = shared_.handedness;
        state.right_stick_mode = shared_.right_stick_mode;
        state.render_scale = static_cast<uint8_t>(shared_.render_scale);
        state.screen_gap = shared_.screen_gap;
        state.menu_open = shared_.menu_open;
        state.menu_focus = static_cast<uint8_t>(std::max(0, shared_.menu_focus));
        state.menu_confirm = shared_.menu_confirm;
        state.max_render_scale = static_cast<uint8_t>(std::max(1, shared_.max_render_scale));
        state.cursor_choosable = shared_.cursor_choosable;
        state.right_stick_choosable = shared_.right_stick_choosable;
        state.cursor_shown = shared_.cursor_shown;
        std::vector<uint8_t> out;
        encode_session_state(state, out);
        companion_.send(out);
        session_state_dirty_ = false;
    }

    std::string error;
    const bool pumped = companion_.pump(inbox_, &error);

    // Processed even when pump() itself failed: a companion's closing message
    // -- typically a Bye, sent right behind whatever it was last asking for --
    // reads as a clean EOF the moment the socket is drained, and pump() reports
    // that as an error. Bailing out here on that error, before ever looking at
    // what was actually read, would silently drop a request that arrived in
    // the same batch but had not been parsed yet.
    while (true) {
        MessageType type = MessageType::Ping;
        std::vector<uint8_t> payload;
        bool fatal = false;
        if (!take_message(inbox_, type, payload, fatal)) {
            if (fatal) {
                drop_companion("that device sent a malformed message");
            }
            break;
        }
        handle_message(type, payload);
        if (!companion_.open()) {
            break;
        }
    }

    // Only reported now, and only if nothing already handled above (a Bye,
    // most often) beat it to closing the connection.
    if (!pumped && companion_.open()) {
        drop_companion(error);
    }
}

void HostSession::disconnect_companion(const std::string& reason) {
    if (!companion_.open()) {
        return;
    }
    // Say goodbye before closing, so the other end knows this was deliberate.
    std::vector<uint8_t> bye;
    encode_bye(bye);
    companion_.send(bye);
    std::string error;
    std::vector<uint8_t> ignored;
    companion_.pump(ignored, &error);
    drop_companion(reason);
}

bool HostSession::take_request(RequestMessage& out_request) {
    if (!have_request_) {
        return false;
    }
    out_request = request_;
    request_ = RequestMessage();
    have_request_ = false;
    return true;
}

bool HostSession::take_menu_open_request() {
    const bool asked = companion_asked_for_menu_;
    companion_asked_for_menu_ = false;
    return asked;
}

bool HostSession::take_menu_press(MenuPress& out) {
    if (menu_presses_.empty()) {
        return false;
    }
    out = menu_presses_.front();
    menu_presses_.pop_front();
    return true;
}

bool HostSession::take_companion_quit_request() {
    if (!companion_wants_quit_) {
        return false;
    }
    companion_wants_quit_ = false;
    return true;
}

std::size_t HostSession::queue_limit() const { return kMaxQueuedAudioBytes; }

// Where a slow stream is actually being lost is not something that can be
// worked out from the far end, or from a machine that is not on the same
// network. This says, once a second: how many frames the game produced, how
// many were worth sending, how many were dropped for the companion being
// behind, the rate on the wire, and how far behind the companion says it is.
void HostSession::report_stream(uint64_t now_ms) {
    if (stats_at_ms_ == 0) {
        stats_at_ms_ = now_ms;
        stats_bytes_ = companion_.queued_total();
        return;
    }
    const uint64_t elapsed = now_ms - stats_at_ms_;
    if (elapsed < 1000) {
        return;
    }
    const uint64_t bytes = companion_.queued_total() - stats_bytes_;
    const unsigned dropped = stats_produced_ - stats_sent_;
    const uint64_t behind = (acked_serial_ != 0 && sent_serial_ > acked_serial_)
                                ? sent_serial_ - acked_serial_
                                : 0;
    log_debug("stream: " + std::to_string(stats_produced_ * 1000 / elapsed) + " produced/s, " +
              std::to_string(stats_sent_ * 1000 / elapsed) + " sent/s, " +
              std::to_string(dropped) + " dropped, " +
              std::to_string(bytes * 1000 / elapsed / 1024) + " KB/s, companion " +
              std::to_string(behind) + " frames behind, " +
              std::to_string(companion_.backlog_bytes()) + " B unsent, round trip " +
              std::to_string(stats_round_trips_ > 0
                                 ? stats_round_trip_total_ms_ / stats_round_trips_
                                 : 0) +
              "ms avg " + std::to_string(stats_round_trip_worst_ms_) + "ms worst");
    stats_at_ms_ = now_ms;
    stats_bytes_ = companion_.queued_total();
    stats_produced_ = 0;
    stats_sent_ = 0;
    stats_round_trips_ = 0;
    stats_round_trip_total_ms_ = 0;
    stats_round_trip_worst_ms_ = 0;
}

std::size_t HostSession::frames_in_flight() {
    const uint64_t departed = companion_.departed_total();
    while (!frame_marks_.empty() && frame_marks_.front() <= departed) {
        frame_marks_.pop_front();
    }
    return frame_marks_.size();
}

void HostSession::publish_audio(const int16_t* interleaved_stereo,
                                std::size_t frames,
                                uint32_t sample_rate) {
    if (!audio_enabled_ || !has_companion() || interleaved_stereo == nullptr || frames == 0) {
        return;
    }
    // Audio is dropped rather than queued when the link is behind, for the same
    // reason as video: a growing queue is heard as drift, not as buffering.
    if (companion_.pending_bytes() > queue_limit()) {
        return;
    }

    AudioMessage message;
    message.stream = AudioStreamId::Shared;  // DS and 3DS mirror; Wii U will not
    message.sample_rate = sample_rate;
    message.samples.assign(interleaved_stereo, interleaved_stereo + frames * 2);

    std::vector<uint8_t> out;
    encode_audio(message, out);
    companion_.send(out);
}

bool HostSession::use_video_for(const ScreenPixels& source) {
    // Every one of these has to be true, and each is a different party's
    // answer: what this session was asked for, what the far end can decode,
    // and whether this build has an encoder at all.
    if (!video_wanted_ || !companion_accepts_video_ || !video::available()) {
        return false;
    }
    // 4:2:0 cannot represent an odd number of pixels. A screen that is one is
    // not a failure -- it goes down the path that never cared.
    return (source.width % 2) == 0 && (source.height % 2) == 0;
}

bool HostSession::encode_one_screen(ScreenId screen,
                                    const ScreenPixels& source,
                                    std::vector<uint8_t>& previous,
                                    FrameMessage& frame) {
    if (!use_video_for(source)) {
        frame.encoding = FrameEncoding::Rgb565DeflateTiles;
        return encode_screen_tiles(source.pixels, source.width, source.height,
                                   source.stride_pixels, previous, frame.payload,
                                   source.channel_order);
    }

    video::VideoEncoder& encoder = (screen == ScreenId::Top) ? video_top_ : video_bottom_;
    if (!encoder.is_open()) {
        video::EncoderConfig config;
        config.width = source.width;
        config.height = source.height;
        config.fps = 60;
        config.bitrate_kbps = 8000;
        std::string error;
        if (!encoder.open(config, &error)) {
            // Not fatal, and not worth saying twice: the tile codec carries
            // every screen this app has ever sent, just less cheaply.
            log_warn("no video encoder for the " + std::string(to_string(screen)) +
                     " screen, falling back to tiles: " + error);
            video_wanted_ = false;
            frame.encoding = FrameEncoding::Rgb565DeflateTiles;
            return encode_screen_tiles(source.pixels, source.width, source.height,
                                       source.stride_pixels, previous, frame.payload,
                                       source.channel_order);
        }
    }
    // A video frame means nothing without the frames before it, so a companion
    // that has just started needing this screen gets one that stands alone.
    if (resend_screens_) {
        encoder.request_keyframe();
    }

    video::EncodedFrame encoded;
    std::string error;
    if (!encoder.encode(source.pixels, source.stride_pixels, encoded, &error)) {
        log_warn("the video encoder failed, falling back to tiles: " + error);
        encoder.close();
        video_wanted_ = false;
        frame.encoding = FrameEncoding::Rgb565DeflateTiles;
        return encode_screen_tiles(source.pixels, source.width, source.height,
                                   source.stride_pixels, previous, frame.payload,
                                   source.channel_order);
    }
    if (encoded.empty()) {
        return false;  // nothing to send this frame, which is not a failure
    }
    frame.encoding = FrameEncoding::H264;
    frame.payload = std::move(encoded.bytes);
    return true;
}

void HostSession::publish_screens(const ScreenPixels& top,
                                  const ScreenPixels& bottom,
                                  uint64_t serial) {
    if (!has_companion()) {
        return;
    }
    // Normally an unchanged frame costs nothing: the serial only moves when the
    // pixels do. But when the companion starts needing a screen it was not being
    // sent -- it opened its menu, whose previews show both -- the pixels may
    // never change again while it waits, since a menu pauses the game. So a
    // change in what it needs forces one send of whatever is current.
    if (serial == sent_serial_ && !resend_screens_) {
        return;
    }
    ++stats_produced_;
    // Never let the send queue grow without bound: a companion that cannot keep
    // up should see dropped frames, not mounting latency.
    //
    // What the companion says it has decoded is the measure that counts, since
    // it is the only one that includes the queues on its side of the wire. It
    // is trusted only once it has said anything at all, so a companion that
    // never acks falls behind rather than being starved of frames outright.
    if (acked_serial_ != 0 && sent_serial_ >= acked_serial_ + kMaxQueuedFrames) {
        return;
    }
    // Until then, and as a backstop, what has not yet left this machine.
    if (frames_in_flight() >= kMaxQueuedFrames) {
        return;
    }

    std::vector<uint8_t> out;
    const struct {
        ScreenId screen;
        const ScreenPixels& pixels;
        std::vector<uint8_t>& previous;
    } screens[] = {{ScreenId::Top, top, previous_top_},
                   {ScreenId::Bottom, bottom, previous_bottom_}};

    for (const auto& entry : screens) {
        const ScreenPixels& source = entry.pixels;
        if (source.pixels == nullptr || source.width <= 0 || source.height <= 0 ||
            !companion_wants(entry.screen)) {
            continue;
        }
        FrameMessage frame;
        frame.screen = entry.screen;
        frame.serial = serial;
        frame.width = static_cast<uint16_t>(source.width);
        frame.height = static_cast<uint16_t>(source.height);
        if (!encode_one_screen(entry.screen, source, entry.previous, frame)) {
            continue;
        }
        encode_frame(frame, out);
    }

    if (!out.empty()) {
        companion_.send(out);
        // Where this frame ends, so frames_in_flight() can tell later whether
        // it has actually gone.
        frame_marks_.push_back(companion_.queued_total());
        frame_sent_at_.emplace_back(serial, monotonic_millis());
        // A companion that stops acking must not make this grow for ever.
        while (frame_sent_at_.size() > 240) {
            frame_sent_at_.pop_front();
        }
        ++stats_sent_;
    }
    sent_serial_ = serial;
    resend_screens_ = false;
}

}  // namespace ds::net
