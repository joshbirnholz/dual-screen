#include "net/companion_session.hpp"

#include <algorithm>

#include "util/log.hpp"

namespace ds::net {

const char* to_string(CompanionSession::Status status) {
    switch (status) {
        case CompanionSession::Status::Idle: return "idle";
        case CompanionSession::Status::Searching: return "searching";
        case CompanionSession::Status::Connecting: return "connecting";
        case CompanionSession::Status::Connected: return "connected";
        case CompanionSession::Status::Failed: return "failed";
    }
    return "?";
}

CompanionSession::CompanionSession() = default;
CompanionSession::~CompanionSession() { disconnect(); }

bool CompanionSession::begin_search(const std::string& device_name,
                                    ScreenId wanted_screen,
                                    std::string* error) {
    device_name_ = device_name;
    wanted_screen_ = wanted_screen;
    granted_screen_ = wanted_screen;
    if (!browser_.running() && !browser_.start(error)) {
        status_ = Status::Failed;
        status_detail_ = error != nullptr ? *error : "cannot listen for hosts";
        return false;
    }
    status_ = Status::Searching;
    status_detail_.clear();
    return true;
}

void CompanionSession::connect_to(DiscoveredHost host) {
    stream_.close();
    inbox_.clear();
    said_hello_ = false;
    ever_sent_input_ = false;
    // Belt and suspenders alongside disconnect()'s and fail()'s own resets:
    // whichever path the last session ended by, a fresh one starts with
    // nothing carried over from it, this included.
    menu_open_ = false;
    session_token_ = 0;
    reassembly_top_ = FrameReassembly();
    reassembly_bottom_ = FrameReassembly();
    for (int stream = 0; stream < 3; ++stream) {
        audio_sequence_known_[stream] = false;
    }

    std::string error;
    if (!stream_.start_connect(host.address, host.session_port, &error)) {
        fail(error);
        return;
    }
    if (!media_socket_.open() && !media_socket_.start(0, false, &error)) {
        fail(error);
        return;
    }
    status_ = Status::Connecting;
    connect_deadline_ms_ = 0;  // set on the first poll, off the caller's clock
    acked_serial_ = 0;
    received_serial_ = 0;
    next_due_ms_ = 0;
    last_arrival_ms_ = 0;
    status_detail_ = "connecting to " + host.device_name;
    game_title_ = host.game_title;
    log_info("connecting to " + host.device_name + " at " + host.address);
}

void CompanionSession::disconnect() {
    if (stream_.open()) {
        std::vector<uint8_t> bye;
        encode_bye(bye);
        stream_.send(bye);
        std::string error;
        std::vector<uint8_t> ignored;
        stream_.pump(ignored, &error);
    }
    stream_.close();
    media_socket_.stop();
    inbox_.clear();
    said_hello_ = false;
    menu_open_ = false;
    session_token_ = 0;
    reassembly_top_ = FrameReassembly();
    reassembly_bottom_ = FrameReassembly();
    pending_frames_.clear();
    received_serial_ = 0;
    next_due_ms_ = 0;
    last_arrival_ms_ = 0;
    if (status_ == Status::Connected || status_ == Status::Connecting) {
        status_ = Status::Searching;
        status_detail_.clear();
    }
    // A session ending is not the host saying the motor stopped, but leaving
    // this device buzzing until the next one happens to say so is worse than
    // assuming it did.
    if (rumble_callback_) {
        rumble_callback_(false);
    }
}

void CompanionSession::fail(const std::string& reason) {
    stream_.close();
    media_socket_.stop();
    inbox_.clear();
    said_hello_ = false;
    menu_open_ = false;
    session_token_ = 0;
    reassembly_top_ = FrameReassembly();
    reassembly_bottom_ = FrameReassembly();
    status_ = Status::Failed;
    status_detail_ = reason;
    if (rumble_callback_) {
        rumble_callback_(false);
    }
    log_warn("companion session failed: " + reason);
}

void CompanionSession::handle_message(MessageType type, const std::vector<uint8_t>& payload) {
    switch (type) {
        case MessageType::Welcome: {
            WelcomeMessage welcome;
            if (!decode_welcome(payload.data(), payload.size(), welcome)) {
                fail("the host sent something we could not read");
                return;
            }
            game_title_ = welcome.game_title;
            system_ = welcome.system;
            granted_screen_ = welcome.granted_screen;
            audio_sample_rate_ = welcome.audio_sample_rate;
            session_token_ = welcome.session_token;
            status_ = Status::Connected;
            status_detail_ = "showing the " + std::string(to_string(granted_screen_)) +
                             " screen of " + game_title_;
            log_info("connected: " + status_detail_);
            // Which of this machine's own addresses is carrying it. A machine
            // with two ways onto one network picks by routing table, not by
            // which cable is plugged in, and the difference is not visible
            // from anywhere else.
            log_info("this session is going out from " + stream_.local_address() + " to " +
                     stream_.peer_address());
            // The host's own media socket is on a fixed, well-known port, but
            // it does not know this device's until a datagram actually
            // arrives there -- see HostSession::poll(). send_input() and the
            // frame-ack below would eventually do that on their own, but
            // input only sends on a change and the first ack needs a frame
            // that cannot arrive until the host knows where to send it; this
            // breaks that circularity with one immediate, unconditional send.
            host_address_ = stream_.peer_address();
            {
                std::vector<uint8_t> hello;
                encode_media_header(session_token_, MessageType::Ping, hello);
                media_socket_.send_to(host_address_, kMediaPort, hello.data(), hello.size());
            }
            break;
        }
        case MessageType::Reject: {
            RejectMessage reject;
            if (!decode_reject(payload.data(), payload.size(), reject)) {
                fail("the host refused the connection");
                return;
            }
            fail(reject.reason);
            break;
        }
        case MessageType::SessionState: {
            SessionStateMessage state;
            if (!decode_session_state(payload.data(), payload.size(), state)) {
                return;  // not worth dropping a session over
            }
            host_layout_id_ = state.layout_id;
            host_paused_ = state.paused;
            host_filtering_ = state.bilinear_filtering;
            host_audio_target_ = state.audio_target;
            host_handedness_ = state.handedness;
            host_right_stick_mode_ = state.right_stick_mode;
            host_render_scale_ = state.render_scale;
            host_screen_gap_ = state.screen_gap;
            host_menu_open_ = state.menu_open;
            host_settings_open_ = state.settings_open;
            host_menu_focus_ = state.menu_focus;
            host_menu_confirm_ = state.menu_confirm;
            host_max_render_scale_ = state.max_render_scale;
            host_cursor_choosable_ = state.cursor_choosable;
            host_right_stick_choosable_ = state.right_stick_choosable;
            host_cursor_shown_ = state.cursor_shown;
            granted_screen_ = state.companion_screen;
            break;
        }
        case MessageType::Rumble: {
            RumbleMessage rumble;
            if (!decode_rumble(payload.data(), payload.size(), rumble)) {
                return;  // not worth ending a session over
            }
            if (rumble_callback_) {
                rumble_callback_(rumble.active);
            }
            break;
        }
        case MessageType::Bye:
            disconnect();
            status_detail_ = "the host closed the session";
            break;
        case MessageType::Ping:
            break;
        default:
            break;
    }
}

void CompanionSession::handle_media_datagram(MessageType type, const uint8_t* payload,
                                             std::size_t payload_size) {
    switch (type) {
        case MessageType::Frame:
            handle_frame_fragment(payload, payload_size);
            break;
        case MessageType::Audio: {
            AudioMessage audio;
            if (!decode_audio(payload, payload_size, audio)) {
                return;  // a bad audio packet is not worth ending a session over
            }
            if (audio.samples.empty()) {
                return;
            }
            const std::size_t index = static_cast<std::size_t>(audio.stream);
            // A gap means whatever the host actually sent for those missing
            // sequence numbers never played here at all -- not silence, just
            // absent, which is what a click or pop actually is: playback
            // jumping straight from one packet's last sample to the next
            // packet's first with no continuity between them. There is no
            // getting those samples back over a link that never promised
            // them, but there is no need to make the jump any sharper than
            // it has to be either.
            bool gap = false;
            uint32_t missing = 0;
            if (index < 3) {
                if (audio_sequence_known_[index] && audio.sequence != audio_next_sequence_[index]) {
                    gap = true;
                    missing = audio.sequence > audio_next_sequence_[index]
                                  ? audio.sequence - audio_next_sequence_[index]
                                  : 0;
                }
                audio_sequence_known_[index] = true;
                audio_next_sequence_[index] = audio.sequence + 1;
            }
            if (gap) {
                log_warn("audio stream " + std::to_string(index) + " lost " +
                         std::to_string(missing) + " packet(s) -- fading back in");
                // A short linear ramp back to full volume in place of the
                // instant jump a lost packet would otherwise leave: still
                // audibly a gap, but a soft one rather than a click. Capped
                // at the packet's own length, since a very short packet has
                // nothing longer to ramp across.
                constexpr std::size_t kFadeInFrames = 64;
                const std::size_t fade_frames =
                    std::min(kFadeInFrames, audio.samples.size() / 2);
                for (std::size_t frame = 0; frame < fade_frames; ++frame) {
                    const double gain = static_cast<double>(frame) / fade_frames;
                    audio.samples[frame * 2] = static_cast<int16_t>(audio.samples[frame * 2] * gain);
                    audio.samples[frame * 2 + 1] =
                        static_cast<int16_t>(audio.samples[frame * 2 + 1] * gain);
                }
            }
            // Shared is a DS or 3DS's one soundtrack and always plays. A Wii
            // U's TopDevice/BottomDevice streams reach a companion at all
            // only when the host decided this device is the one holding that
            // screen -- see HostSession::publish_audio and the External
            // source's own audio callback in app.cpp -- so whichever one
            // arrives here always plays too; there is nothing left to check.
            if (audio_callback_) {
                audio_callback_(audio.samples.data(), audio.samples.size() / 2,
                                audio.sample_rate != 0 ? audio.sample_rate : audio_sample_rate_);
            }
            break;
        }
        default:
            // Input and FrameAck are companion->host only; anything else on
            // this channel is not this end's to act on.
            break;
    }
}

void CompanionSession::handle_frame_fragment(const uint8_t* payload, std::size_t payload_size) {
    FrameFragmentHeader header;
    const uint8_t* chunk = nullptr;
    std::size_t chunk_size = 0;
    if (!decode_frame_fragment(payload, payload_size, header, chunk, chunk_size)) {
        return;
    }
    // Bounded before allocating: these dimensions came off a socket, and
    // decode_screen_pixels() sizes its buffers from them alone, before it has
    // even looked at the payload. 4096 is not tied to any one console -- it
    // only has to comfortably clear whatever a real screen, read back at
    // whatever resolution the recipient asked for, could actually come out to
    // (a 3DS top screen at its maximum render scale is 1200 wide), with
    // headroom for larger consoles this app plays later rather than needing
    // to be raised again the next time one arrives.
    constexpr uint16_t kMaxFrameDimension = 4096;
    if (header.width == 0 || header.height == 0 || header.width > kMaxFrameDimension ||
        header.height > kMaxFrameDimension || header.fragment_count == 0 ||
        header.fragment_index >= header.fragment_count) {
        return;  // not worth ending a session over -- see the class comment
    }

    FrameReassembly& slot = (header.screen == ScreenId::Top) ? reassembly_top_ : reassembly_bottom_;
    // A fragment for anything other than what is already in progress starts a
    // fresh reassembly, discarding whatever partial fragments were held for
    // the old one -- the sender has moved on to a newer frame, and the two
    // can never be pieced together into one anyway. Anything older than the
    // one already in progress is ignored outright.
    //
    // A fragment_count that disagrees with what is already recorded for this
    // same serial is treated the same as a newer one: two sends can only
    // ever legitimately share a serial by describing the exact same frame,
    // so a mismatch here means this is actually a second, different
    // transmission of it (a resend re-encoded after the first had already
    // started arriving, most likely) -- mixing fragments from the two would
    // reassemble into bytes that were never one real frame to begin with,
    // which is corruption a decoder has no way to recover from.
    const bool same_transmission =
        slot.active && header.serial == slot.serial && header.fragment_count == slot.fragment_count;
    if (!same_transmission && (!slot.active || header.serial >= slot.serial)) {
        slot = FrameReassembly();
        slot.active = true;
        slot.serial = header.serial;
        slot.screen = header.screen;
        slot.encoding = header.encoding;
        slot.width = header.width;
        slot.height = header.height;
        slot.fragment_count = header.fragment_count;
        slot.fragments.resize(header.fragment_count);
        slot.received.assign(header.fragment_count, false);
    } else if (!same_transmission) {
        return;
    }
    if (!slot.received[header.fragment_index]) {
        slot.fragments[header.fragment_index].assign(chunk, chunk + chunk_size);
        slot.received[header.fragment_index] = true;
        ++slot.received_count;
    }
    if (slot.received_count < slot.fragment_count) {
        return;
    }

    FrameMessage frame;
    frame.screen = slot.screen;
    frame.serial = slot.serial;
    frame.width = slot.width;
    frame.height = slot.height;
    frame.encoding = slot.encoding;
    for (const std::vector<uint8_t>& fragment : slot.fragments) {
        frame.payload.insert(frame.payload.end(), fragment.begin(), fragment.end());
    }
    slot = FrameReassembly();

    // Held rather than applied here: poll() shows frames on a clock, so the
    // cadence on screen is even whatever the arrivals did.
    if (frame.serial != received_serial_) {
        const uint64_t now = monotonic_millis();
        if (last_arrival_ms_ != 0 && now > last_arrival_ms_) {
            // Smoothed, because any one gap is mostly jitter. This is how
            // fast the host is really producing, which is what the picture
            // should advance at.
            const double gap = static_cast<double>(now - last_arrival_ms_);
            if (gap >= 4.0 && gap <= 100.0) {
                frame_period_ms_ = frame_period_ms_ * 0.95 + gap * 0.05;
            }
        }
        last_arrival_ms_ = now;
        received_serial_ = frame.serial;
    }
    pending_frames_.push_back(std::move(frame));
}

void CompanionSession::poll(uint64_t now_ms) {
    browser_.poll(now_ms);

    if (!stream_.open()) {
        return;
    }

    // Joining has to either happen or stop happening. A host that has gone
    // since it last announced itself, or one reachable at an address this
    // device cannot route to -- a machine that broadcasts from more than one
    // interface offers several, and only some of them work from here -- leaves
    // a connect sitting in the operating system's own retries for two minutes,
    // during which nothing on screen changes and no button appears to do
    // anything. The deadline covers the whole of joining rather than just the
    // socket: a host that accepts the connection and then never says hello is
    // the same problem wearing a different hat.
    if (status_ == Status::Connecting) {
        if (connect_deadline_ms_ == 0) {
            connect_deadline_ms_ = now_ms + kJoinTimeoutMs;
        } else if (now_ms >= connect_deadline_ms_) {
            fail("that device did not answer");
            return;
        }
    }

    if (status_ == Status::Connected) {
        report_stream(now_ms);
    }

    std::string error;
    if (!stream_.pump(inbox_, &error)) {
        fail(error);
        return;
    }

    // The hello waits until the TCP handshake has actually completed, so it is
    // never queued against a socket that turns out to be unreachable.
    if (!said_hello_ && stream_.connected()) {
        HelloMessage hello;
        hello.device_name = device_name_;
        hello.wanted_screen = wanted_screen_;
        // Said rather than assumed: a build without FFmpeg cannot decode video,
        // and the host has to know before it picks how to send a screen.
        hello.accepts_video = video::available();
        std::vector<uint8_t> out;
        encode_hello(hello, out);
        stream_.send(out);
        said_hello_ = true;
    }

    while (true) {
        MessageType type = MessageType::Ping;
        std::vector<uint8_t> payload;
        bool fatal = false;
        if (!take_message(inbox_, type, payload, fatal)) {
            if (fatal) {
                fail("the host sent a malformed message");
            }
            break;
        }
        handle_message(type, payload);
        if (!stream_.open()) {
            break;
        }
    }

    // Frame, Audio, Input and FrameAck all ride the media channel instead --
    // see kMediaPort. Drained the same way the TCP inbox above is, and
    // ignored (not treated as a session failure) if it does not check out:
    // UDP has no stream to desynchronise, so a bad or stray datagram is just
    // one bad datagram.
    {
        std::vector<uint8_t> datagram;
        std::string from_address;
        while (media_socket_.receive(datagram, from_address)) {
            uint64_t token = 0;
            MessageType type = MessageType::Ping;
            const uint8_t* payload = nullptr;
            std::size_t payload_size = 0;
            if (!decode_media_header(datagram.data(), datagram.size(), token, type, payload,
                                     payload_size)) {
                continue;
            }
            if (session_token_ == 0 || token != session_token_) {
                continue;
            }
            handle_media_datagram(type, payload, payload_size);
        }
    }

    // Advance the picture on the host's cadence rather than this device's.
    //
    // A frame that arrived early waits until it is due, which is what pays for
    // the one that arrives late; the two are the same jitter either side of
    // the same clock. Beyond a few frames waiting is a backlog rather than
    // jitter, and holding that would only show old pictures late, so it is
    // caught up on instead.
    constexpr std::size_t kJitterFrames = 3;
    if (!pending_frames_.empty()) {
        const uint64_t period = frame_period();
        const std::size_t waiting = buffered_frames();
        std::size_t budget = waiting > kJitterFrames ? waiting - kJitterFrames : 0;
        if (next_due_ms_ == 0 || now_ms >= next_due_ms_) {
            budget = std::max<std::size_t>(budget, 1);
        }
        std::size_t shown = 0;
        for (; shown < budget && decode_next_frame(); ++shown) {
        }
        if (shown > 0) {
            // Advanced from when it was due, not from now, so the pace does
            // not drift slower by however late each pass happened to run. A
            // clock that has fallen far behind is started again instead.
            const uint64_t step = period * shown;
            next_due_ms_ = (next_due_ms_ == 0 || now_ms > next_due_ms_ + period * 4)
                               ? now_ms + period
                               : next_due_ms_ + step;
        }
    }

    // Tell the host which frame has actually been decoded here. This is the
    // only thing that lets it know how far behind this device is: everything
    // else it could measure sits on its own side of the wire, and a frame that
    // has left the host may still be waiting in this machine's receive buffer.
    //
    // After the messages above, not before them, and flushed here rather than
    // left for the next poll: an ack that describes the state of things two
    // polls ago reads to the host as a device two frames further behind than
    // it is, and the host throttles on exactly that number.
    if (status_ == Status::Connected && received_serial_ != acked_serial_) {
        const uint64_t newest = received_serial_;
        acked_serial_ = newest;
        std::vector<uint8_t> ack;
        encode_media_header(session_token_, MessageType::FrameAck, ack);
        encode_frame_ack_payload(newest, ack);
        media_socket_.send_to(host_address_, kMediaPort, ack.data(), ack.size());
    }
}

void CompanionSession::send_input(const DsInputState& state) {
    if (status_ != Status::Connected || !stream_.connected()) {
        return;
    }
    // Only changes go out. Holding a button steady costs nothing.
    if (ever_sent_input_ && state.buttons == last_sent_input_.buttons &&
        state.touching == last_sent_input_.touching &&
        state.circle_x == last_sent_input_.circle_x &&
        state.circle_y == last_sent_input_.circle_y &&
        state.c_stick_x == last_sent_input_.c_stick_x &&
        state.c_stick_y == last_sent_input_.c_stick_y &&
        (!state.touching || (state.touch_x == last_sent_input_.touch_x &&
                             state.touch_y == last_sent_input_.touch_y))) {
        return;
    }

    InputMessage message;
    message.buttons = state.buttons;
    message.touching = state.touching;
    message.touch_x = static_cast<int16_t>(state.touch_x);
    message.touch_y = static_cast<int16_t>(state.touch_y);
    message.circle_x = state.circle_x;
    message.circle_y = state.circle_y;
    message.c_stick_x = state.c_stick_x;
    message.c_stick_y = state.c_stick_y;
    std::vector<uint8_t> out;
    encode_media_header(session_token_, MessageType::Input, out);
    encode_input_payload(message, out);
    media_socket_.send_to(host_address_, kMediaPort, out.data(), out.size());

    last_sent_input_ = state;
    ever_sent_input_ = true;
}

const std::vector<uint32_t>& CompanionSession::ScreenBuffer::expanded() const {
    // A video frame is already in this form. There is nothing to expand and
    // nothing to cache.
    if (video) {
        return pixels;
    }
    if (!ever_expanded || expanded_serial != serial) {
        expand_rgb565(rgb565, pixels);
        expanded_serial = serial;
        ever_expanded = true;
    }
    return pixels;
}

CompanionSession::ScreenPixels CompanionSession::screen_pixels(ScreenId screen) const {
    const ScreenBuffer& buffer = (screen == ScreenId::Top) ? top_ : bottom_;
    ScreenPixels out;
    if (!buffer.has_frame() || buffer.width <= 0 || buffer.height <= 0) {
        return out;
    }
    out.pixels = buffer.expanded().data();
    out.width = buffer.width;
    out.height = buffer.height;
    out.serial = buffer.serial;
    return out;
}

CompanionSession::ScreenBytes CompanionSession::screen_rgb565(ScreenId screen) const {
    const ScreenBuffer& buffer = (screen == ScreenId::Top) ? top_ : bottom_;
    ScreenBytes out;
    if (buffer.rgb565.empty() || buffer.width <= 0 || buffer.height <= 0) {
        return out;
    }
    out.pixels = buffer.rgb565.data();
    out.width = buffer.width;
    out.height = buffer.height;
    out.serial = buffer.serial;
    return out;
}

bool CompanionSession::has_screen(ScreenId screen) const {
    return screen_pixels(screen).valid();
}

uint64_t CompanionSession::received_serial(ScreenId screen) const {
    return (screen == ScreenId::Top) ? top_.serial : bottom_.serial;
}

uint32_t CompanionSession::sample_pixel(ScreenId screen) const {
    const ScreenPixels frame = screen_pixels(screen);
    return frame.valid() ? (frame.pixels[0] & 0x00ffffffu) : 0u;
}

void CompanionSession::send_request(RequestMessage request) {
    if (status_ != Status::Connected || !stream_.connected()) {
        return;
    }
    // Every request carries the menu state, so the host cannot miss it even if
    // one message is the only one that ever goes out.
    request.menu_open = menu_open_;
    std::vector<uint8_t> out;
    encode_request(request, out);
    stream_.send(out);
}

void CompanionSession::request_layout(const std::string& layout_id) {
    if (layout_id.empty()) {
        return;
    }
    RequestMessage request;
    request.layout_id = layout_id;
    send_request(request);
}

void CompanionSession::request_filtering(bool bilinear) {
    RequestMessage request;
    request.wants_filtering = true;
    request.bilinear_filtering = bilinear;
    send_request(request);
}

void CompanionSession::request_audio_target(AudioTarget target) {
    RequestMessage request;
    request.wants_audio_target = true;
    request.audio_target = target;
    send_request(request);
}

void CompanionSession::request_right_stick_mode(RightStickMode mode) {
    RequestMessage request;
    request.wants_right_stick_mode = true;
    request.right_stick_mode = mode;
    send_request(request);
}

void CompanionSession::request_handedness(Handedness handedness) {
    RequestMessage request;
    request.wants_handedness = true;
    request.handedness = handedness;
    send_request(request);
}

void CompanionSession::request_render_scale(int scale) {
    RequestMessage request;
    request.wants_render_scale = true;
    request.render_scale = static_cast<uint8_t>(scale < 1 ? 1 : scale);
    send_request(request);
}

void CompanionSession::request_screen_gap(bool include_gap) {
    RequestMessage request;
    request.wants_screen_gap = true;
    request.screen_gap = include_gap;
    send_request(request);
}

void CompanionSession::request_menu_action(MenuAction action) {
    RequestMessage request;
    request.wants_menu_action = true;
    request.menu_action = action;
    send_request(request);
}

void CompanionSession::request_menu_focus(int focus) {
    if (focus < 0) {
        return;
    }
    RequestMessage request;
    request.wants_menu_focus = true;
    request.menu_focus = static_cast<uint8_t>(focus);
    send_request(request);
}

void CompanionSession::request_menu_press(int focus, MenuAction action) {
    if (focus < 0) {
        request_menu_action(action);
        return;
    }
    RequestMessage request;
    request.wants_menu_focus = true;
    request.menu_focus = static_cast<uint8_t>(focus);
    request.wants_menu_action = true;
    request.menu_action = action;
    send_request(request);
}

void CompanionSession::request_reset() {
    RequestMessage request;
    request.wants_reset = true;
    send_request(request);
}

void CompanionSession::request_quit() {
    RequestMessage request;
    request.wants_quit = true;
    send_request(request);
}

void CompanionSession::request_display_size(int width, int height) {
    RequestMessage request;
    request.wants_display_size = true;
    // Clamped rather than rejected: a window can be zero-sized for a moment
    // while minimised, and 1 is a harmless value to send rather than a value
    // worth guarding against on the host too.
    request.display_width = static_cast<uint16_t>(std::clamp(width, 1, 65535));
    request.display_height = static_cast<uint16_t>(std::clamp(height, 1, 65535));
    send_request(request);
}

void CompanionSession::set_menu_open(bool open) {
    if (open == menu_open_) {
        return;
    }
    menu_open_ = open;
    send_request(RequestMessage());
}

bool CompanionSession::advance_frame(std::string* error) {
    // Frames arrive from poll(); there is nothing to drive here. A failed
    // session is reported so the app can show it rather than a frozen picture.
    if (status_ == Status::Failed) {
        if (error != nullptr) *error = status_detail_;
        return false;
    }
    return true;
}

const ScreenTextures& CompanionSession::screens() {
    for (ScreenBuffer* buffer : {&top_, &bottom_}) {
        const bool stale =
            !buffer->texture.valid() || buffer->serial != buffer->uploaded_serial;
        if (stale && buffer->has_frame()) {
            // Uploaded in the form it arrived in. Expanding 5-6-5 to 8 bits a
            // channel first would be a whole pass over the frame and twice the
            // bytes to the GPU, for a picture that is 5-6-5 either way; a video
            // frame is already eight, and going back would only lose.
            if (buffer->video) {
                buffer->texture.upload_xrgb8888(buffer->pixels.data(), buffer->width,
                                                buffer->height, buffer->width);
            } else {
                buffer->texture.upload_rgb565(buffer->rgb565.data(), buffer->width,
                                              buffer->height);
            }
            buffer->uploaded_serial = buffer->serial;
        }
    }
    textures_.top = top_.texture.valid() ? &top_.texture : nullptr;
    textures_.bottom = bottom_.texture.valid() ? &bottom_.texture : nullptr;
    return textures_;
}

void CompanionSession::report_stream(uint64_t now_ms) {
    if (stats_at_ms_ == 0) {
        stats_at_ms_ = now_ms;
        return;
    }
    const uint64_t elapsed = now_ms - stats_at_ms_;
    if (elapsed < 1000) {
        return;
    }
    log_debug("stream: " + std::to_string(stats_decoded_ * 1000 / elapsed) +
              " decoded/s, " + std::to_string(stats_drawn_ * 1000 / elapsed) +
              " drawn/s, newest is " + std::to_string(acked_serial_));
    stats_at_ms_ = now_ms;
    stats_decoded_ = 0;
    stats_drawn_ = 0;
}

// How long each frame should be on screen: what the host is producing at,
// clamped so that a stall or a burst cannot leave the picture crawling or
// racing.
uint64_t CompanionSession::frame_period() const {
    const double clamped = std::min(40.0, std::max(8.0, frame_period_ms_));
    return static_cast<uint64_t>(clamped + 0.5);
}

void CompanionSession::apply_frame(const FrameMessage& frame) {
    ScreenBuffer& target = (frame.screen == ScreenId::Top) ? top_ : bottom_;
    if (frame.encoding == FrameEncoding::H264) {
        if (!apply_video_frame(frame, target)) {
            return;
        }
    } else {
        // A tiled frame is a difference from the last one, so it is applied
        // onto what is already here; a whole one replaces it.
        const bool decoded =
            frame.encoding == FrameEncoding::Rgb565DeflateTiles
                ? decode_screen_tiles(frame.payload, frame.width, frame.height, target.rgb565)
                : decode_screen_rgb565(frame.payload, frame.width, frame.height, target.rgb565);
        if (!decoded) {
            fail("a frame from the host could not be decompressed");
            return;
        }
        target.video = false;
        target.width = frame.width;
        target.height = frame.height;
    }
    target.serial = frame.serial;
    ++stats_decoded_;
}

bool CompanionSession::apply_video_frame(const FrameMessage& frame, ScreenBuffer& target) {
    video::VideoDecoder& decoder = (frame.screen == ScreenId::Top) ? video_top_ : video_bottom_;
    if (!decoder.is_open()) {
        // The host only sends these to a device that said it could decode them,
        // so failing here means the two ends disagree about what was agreed --
        // a broken session rather than one bad frame.
        video::DecoderConfig config;
        std::string error;
        if (!decoder.open(config, &error)) {
            fail("the host sent video this device cannot decode: " + error);
            return false;
        }
    }
    video::DecodedFrame decoded;
    std::string error;
    if (!decoder.decode(frame.payload.data(), frame.payload.size(), decoded, &error)) {
        fail("a frame from the host could not be decoded: " + error);
        return false;
    }
    if (!decoded.valid()) {
        // Nothing out yet. A stream that has not reached its first keyframe has
        // nothing to show, which is a wait rather than a failure.
        return false;
    }
    target.video = true;
    target.pixels = std::move(decoded.pixels);
    target.width = decoded.width;
    target.height = decoded.height;
    // Whatever the tile codec left here is not this picture, and keeping it
    // would let a later tiled frame patch onto a frame from another codec.
    target.rgb565.clear();
    return true;
}

// One whole frame: every screen the host sent under the same serial, so the
// two screens never show different moments.
bool CompanionSession::decode_next_frame() {
    if (pending_frames_.empty()) {
        return false;
    }
    const uint64_t serial = pending_frames_.front().serial;
    while (!pending_frames_.empty() && pending_frames_.front().serial == serial) {
        apply_frame(pending_frames_.front());
        pending_frames_.pop_front();
    }
    return true;
}

std::size_t CompanionSession::buffered_frames() const {
    std::size_t frames = 0;
    bool counting = false;
    uint64_t previous = 0;
    for (const FrameMessage& frame : pending_frames_) {
        if (!counting || frame.serial != previous) {
            ++frames;
            previous = frame.serial;
            counting = true;
        }
    }
    return frames;
}

void CompanionSession::forget_screen_textures() {
    for (ScreenBuffer* buffer : {&top_, &bottom_}) {
        buffer->texture = Texture();
        buffer->uploaded_serial = 0;
    }
    textures_ = ScreenTextures();
}

SystemScreens CompanionSession::screens_geometry() const {
    // The host says which console it is running and both devices run the same
    // build, so the sizes are looked up rather than measured from whatever
    // frame happened to arrive first.
    return system_profile(system_).screens;
}

}  // namespace ds::net
