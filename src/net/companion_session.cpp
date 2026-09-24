#include "net/companion_session.hpp"

#include <algorithm>
#include <random>
#include <cmath>

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
    // Each trip to the host list asks again, in case the last answer never
    // came -- see knock_on_the_local_network().
    knocked_.clear();
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
    // A new host counts its sent frames from the start, so nothing is known
    // about where they are up to until the first one lands.
    sent_index_known_top_ = false;
    sent_index_known_bottom_ = false;
    // Whatever gap there was before this session began says nothing about the
    // one starting; the first poll of it measures nothing.
    last_poll_ms_ = 0;
    recent_poll_gaps_.clear();
    next_poll_gap_ = 0;
    typical_poll_gap_ms_ = 0;
    // A keyframe wanted by a session that has ended is not one to ask the
    // next host for.
    keyframe_wanted_top_ = false;
    keyframe_wanted_bottom_ = false;
    keyframe_asked_at_top_ = 0;
    keyframe_asked_at_bottom_ = 0;
    keyframe_wait_top_ms_ = kKeyframeRequestIntervalMs;
    keyframe_wait_bottom_ms_ = kKeyframeRequestIntervalMs;
    // Whatever this device settled on for the last host is not a judgement
    // about the next one, which may be sending a quite different picture.
    display_scale_ = 1.0;
    adapt_decode_ms_ = 0;
    adapt_readback_micros_ = 0;
    adapt_lock_micros_ = 0;
    adapt_frames_ = 0;
    adapt_cost_before_step_ms_ = 0.0;
    adapt_width_before_step_ = 0;
    adapt_last_width_ = 0;
    adapt_last_height_ = 0;
    adapt_arrived_scale_ = 0.0;
    adapt_awaiting_from_width_ = 0;
    adapt_awaiting_since_ms_ = 0;
    adapt_restart_window_ = false;
    adapt_serial_at_start_ = 0;
    adapt_arrivals_ = 0;
    adapt_size_decides_cost_ = true;
    adapt_at_ms_ = 0;
    adapt_easy_windows_ = 0;
    too_expensive_scale_ = 2.0;
    too_expensive_at_ms_ = 0;
    decode_failures_top_ = 0;
    decode_failures_bottom_ = 0;
    // A host that has gone is not asking for anything, motion included -- so
    // the client following this switches its sensors back off.
    host_wants_motion_ = false;
    for (int stream = 0; stream < 3; ++stream) {
        audio_sequence_known_[stream] = false;
    }

    joining_ = std::move(host);
    connect_attempts_ = 0;
    waiting_on_local_network_ = false;
    retry_connect_ = false;
    retry_connect_at_ms_ = 0;
    connect_failure_.clear();
    connect_deadline_ms_ = 0;  // set on the first poll, off the caller's clock
    acked_serial_ = 0;
    received_serial_ = 0;
    next_due_ms_ = 0;
    next_due_exact_ms_ = 0.0;
    behind_since_ms_ = 0;
    last_shed_ms_ = 0;
    last_arrival_ms_ = 0;
    last_sent_at_ms_ = 0;
    jitter_.reset();
    game_title_ = joining_.game_title;
    log_info("connecting to " + joining_.device_name + " at " + joining_.address);
    begin_connect();
}

void CompanionSession::begin_connect() {
    ++connect_attempts_;
    std::string error;
    if (!stream_.start_connect(joining_.address, joining_.session_port, &error)) {
        if (connect_is_undecided(stream_.connect_error())) {
            owe_another_attempt(error);
            return;
        }
        fail(error);
        return;
    }
    if (!media_socket_.open() && !media_socket_.start(0, false, &error)) {
        fail(error);
        return;
    }
    status_ = Status::Connecting;
    status_detail_ = "connecting to " + joining_.device_name;
}

void CompanionSession::owe_an_attempt_for_test() {
    owe_another_attempt("refused for a test");
}

void CompanionSession::owe_another_attempt(const std::string& reason) {
    // Said once per join rather than once per attempt: several a second, all
    // saying the same thing, would bury the line that matters if the join does
    // go on to fail.
    if (connect_failure_.empty()) {
        log_info("the operating system has not let this app onto the local network yet; "
                 "trying again until it does or the join runs out of time (" + reason + ")");
    }
    connect_failure_ = reason;
    waiting_on_local_network_ = true;
    log_debug("attempt " + std::to_string(connect_attempts_) +
              " at reaching that device was refused by the operating system; trying again");
    stream_.close();
    retry_connect_ = true;
    retry_connect_at_ms_ = 0;  // due a moment from the next poll, on its clock
    status_ = Status::Connecting;
    status_detail_ = "connecting to " + joining_.device_name;
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
    retry_connect_ = false;
    retry_connect_at_ms_ = 0;
    connect_failure_.clear();
    waiting_on_local_network_ = false;
    inbox_.clear();
    said_hello_ = false;
    menu_open_ = false;
    session_token_ = 0;
    reassembly_top_ = FrameReassembly();
    reassembly_bottom_ = FrameReassembly();
    // A new host counts its sent frames from the start, so nothing is known
    // about where they are up to until the first one lands.
    sent_index_known_top_ = false;
    sent_index_known_bottom_ = false;
    // Whatever gap there was before this session began says nothing about the
    // one starting; the first poll of it measures nothing.
    last_poll_ms_ = 0;
    recent_poll_gaps_.clear();
    next_poll_gap_ = 0;
    typical_poll_gap_ms_ = 0;
    // A keyframe wanted by a session that has ended is not one to ask the
    // next host for.
    keyframe_wanted_top_ = false;
    keyframe_wanted_bottom_ = false;
    keyframe_asked_at_top_ = 0;
    keyframe_asked_at_bottom_ = 0;
    keyframe_wait_top_ms_ = kKeyframeRequestIntervalMs;
    keyframe_wait_bottom_ms_ = kKeyframeRequestIntervalMs;
    // Whatever this device settled on for the last host is not a judgement
    // about the next one, which may be sending a quite different picture.
    display_scale_ = 1.0;
    adapt_decode_ms_ = 0;
    adapt_readback_micros_ = 0;
    adapt_lock_micros_ = 0;
    adapt_frames_ = 0;
    adapt_cost_before_step_ms_ = 0.0;
    adapt_width_before_step_ = 0;
    adapt_last_width_ = 0;
    adapt_last_height_ = 0;
    adapt_arrived_scale_ = 0.0;
    adapt_awaiting_from_width_ = 0;
    adapt_awaiting_since_ms_ = 0;
    adapt_restart_window_ = false;
    adapt_serial_at_start_ = 0;
    adapt_arrivals_ = 0;
    adapt_size_decides_cost_ = true;
    adapt_at_ms_ = 0;
    adapt_easy_windows_ = 0;
    too_expensive_scale_ = 2.0;
    too_expensive_at_ms_ = 0;
    decode_failures_top_ = 0;
    decode_failures_bottom_ = 0;
    // A host that has gone is not asking for anything, motion included -- so
    // the client following this switches its sensors back off.
    host_wants_motion_ = false;
    pending_frames_.clear();
    received_serial_ = 0;
    next_due_ms_ = 0;
    next_due_exact_ms_ = 0.0;
    behind_since_ms_ = 0;
    last_shed_ms_ = 0;
    last_arrival_ms_ = 0;
    last_sent_at_ms_ = 0;
    jitter_.reset();
    if (status_ == Status::Connected || status_ == Status::Connecting) {
        status_ = Status::Searching;
        status_detail_.clear();
    }
    // A session ending is not the host saying the motor stopped, but leaving
    // this device buzzing until the next one happens to say so is worse than
    // assuming it did.
    if (rumble_callback_) {
        rumble_callback_(false, RumbleMessage::kWholeDevice, 0);
    }
}

void CompanionSession::fail(const std::string& reason) {
    stream_.close();
    retry_connect_ = false;
    retry_connect_at_ms_ = 0;
    waiting_on_local_network_ = false;
    media_socket_.stop();
    inbox_.clear();
    said_hello_ = false;
    menu_open_ = false;
    session_token_ = 0;
    reassembly_top_ = FrameReassembly();
    reassembly_bottom_ = FrameReassembly();
    // A new host counts its sent frames from the start, so nothing is known
    // about where they are up to until the first one lands.
    sent_index_known_top_ = false;
    sent_index_known_bottom_ = false;
    // Whatever gap there was before this session began says nothing about the
    // one starting; the first poll of it measures nothing.
    last_poll_ms_ = 0;
    recent_poll_gaps_.clear();
    next_poll_gap_ = 0;
    typical_poll_gap_ms_ = 0;
    // A keyframe wanted by a session that has ended is not one to ask the
    // next host for.
    keyframe_wanted_top_ = false;
    keyframe_wanted_bottom_ = false;
    keyframe_asked_at_top_ = 0;
    keyframe_asked_at_bottom_ = 0;
    keyframe_wait_top_ms_ = kKeyframeRequestIntervalMs;
    keyframe_wait_bottom_ms_ = kKeyframeRequestIntervalMs;
    // Whatever this device settled on for the last host is not a judgement
    // about the next one, which may be sending a quite different picture.
    display_scale_ = 1.0;
    adapt_decode_ms_ = 0;
    adapt_readback_micros_ = 0;
    adapt_lock_micros_ = 0;
    adapt_frames_ = 0;
    adapt_cost_before_step_ms_ = 0.0;
    adapt_width_before_step_ = 0;
    adapt_last_width_ = 0;
    adapt_last_height_ = 0;
    adapt_arrived_scale_ = 0.0;
    adapt_awaiting_from_width_ = 0;
    adapt_awaiting_since_ms_ = 0;
    adapt_restart_window_ = false;
    adapt_serial_at_start_ = 0;
    adapt_arrivals_ = 0;
    adapt_size_decides_cost_ = true;
    adapt_at_ms_ = 0;
    adapt_easy_windows_ = 0;
    too_expensive_scale_ = 2.0;
    too_expensive_at_ms_ = 0;
    decode_failures_top_ = 0;
    decode_failures_bottom_ = 0;
    // A host that has gone is not asking for anything, motion included -- so
    // the client following this switches its sensors back off.
    host_wants_motion_ = false;
    status_ = Status::Failed;
    status_detail_ = reason;
    if (rumble_callback_) {
        rumble_callback_(false, RumbleMessage::kWholeDevice, 0);
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
            // And the media channel goes out the same way. Without this the
            // input and acks travelling back to the host would be routed
            // independently of the connection they belong to, which on a
            // machine that is on one subnet twice can be a different
            // interface entirely -- see UdpSocket::set_source_address().
            media_socket_.set_source_address(stream_.local_address());
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
            host_left_handed_layouts_ = state.left_handed_layouts;
            host_right_stick_mode_ = state.right_stick_mode;
            host_render_scale_ = state.render_scale;
            host_screen_gap_ = state.screen_gap;
            host_wants_motion_ = state.motion_wanted;
            host_menu_open_ = state.menu_open;
            host_settings_open_ = state.settings_open;
            host_menu_focus_ = state.menu_focus;
            host_menu_confirm_ = state.menu_confirm;
            host_max_render_scale_ = state.max_render_scale;
            host_cursor_choosable_ = state.cursor_choosable;
            host_right_stick_choosable_ = state.right_stick_choosable;
            host_left_stick_choosable_ = state.left_stick_choosable;
            host_left_stick_press_ = state.left_stick_press;
            host_cursor_shown_ = state.cursor_shown;
            host_save_points_available_ = state.save_points_available;
            granted_screen_ = state.companion_screen;
            break;
        }
        case MessageType::SaveSlots: {
            SaveSlotsMessage slots;
            if (!decode_save_slots(payload.data(), payload.size(), slots)) {
                return;  // the menu goes without pictures; not worth a session
            }
            host_save_slots_ = std::move(slots);
            ++host_save_slots_revision_;
            break;
        }
        case MessageType::Rumble: {
            RumbleMessage rumble;
            if (!decode_rumble(payload.data(), payload.size(), rumble)) {
                return;  // not worth ending a session over
            }
            if (rumble_callback_) {
                rumble_callback_(rumble.active, rumble.pad, rumble.ping_ms);
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
        case MessageType::InputEcho: {
            InputEchoMessage echo;
            // Only a picture not yet on screen can say when the input came
            // back: one already shown -- a game that drew nothing new -- would
            // time nothing.
            if (decode_input_echo_payload(payload, payload_size, echo) &&
                echo.serial > shown_serial_ && echoes_waiting_.size() < 16) {
                echoes_waiting_.push_back(echo);
            }
            break;
        }
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

unsigned CompanionSession::note_frames_that_never_arrived(const FrameFragmentHeader& header) {
    const bool top = header.screen == ScreenId::Top;
    uint16_t& last = top ? last_sent_index_top_ : last_sent_index_bottom_;
    bool& known = top ? sent_index_known_top_ : sent_index_known_bottom_;
    if (!known) {
        // Nothing to compare the first one against. Whatever the host is up
        // to, this is where counting starts.
        known = true;
        last = header.sent_index;
        return 0;
    }
    const unsigned missing = frames_missing_between(last, header.sent_index);
    // Only a fragment of something newer moves the mark. The others -- the
    // rest of the frame already being put together, or one that overtook its
    // neighbours on the way -- are not loss and are not progress either.
    if (static_cast<int16_t>(header.sent_index - last) > 0) {
        last = header.sent_index;
    }
    return missing;
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

    const unsigned missing = note_frames_that_never_arrived(header);
    if (missing > 0) {
        stats_frames_lost_ += missing;
        ((header.screen == ScreenId::Top) ? keyframe_wanted_top_ : keyframe_wanted_bottom_) = true;
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
        if (slot.active && slot.received_count < slot.fragment_count) {
            // A frame the host definitely sent that this end definitely never
            // got whole -- the sender has moved on, so the missing pieces are
            // not coming. Everything decoded from here on is built on a
            // reference frame that was never complete, which is exactly what
            // a picture that stands on its own repairs. This is the only
            // place loss can be seen at all: a gap in serials means nothing
            // (the host legitimately skips frames it could not send), and a
            // decoder handed a stream with a hole in it mostly carries on
            // producing plausible-looking rubbish rather than failing.
            ++stats_frames_lost_;
            ((slot.screen == ScreenId::Top) ? keyframe_wanted_top_ : keyframe_wanted_bottom_) =
                true;
        }
        slot = FrameReassembly();
        slot.active = true;
        slot.serial = header.serial;
        slot.screen = header.screen;
        slot.encoding = header.encoding;
        slot.width = header.width;
        slot.height = header.height;
        slot.fragment_count = header.fragment_count;
        // Started here and then kept as the latest of them -- see below.
        slot.sent_at_ms = header.sent_at_ms;
        slot.fragments.resize(header.fragment_count);
        slot.received.assign(header.fragment_count, false);
    } else if (!same_transmission) {
        return;
    }
    // The latest departure among this picture's pieces, not the first.
    //
    // The pieces of one frame no longer leave together: each is stamped as it
    // actually goes out, so a big picture's eighty-odd datagrams carry eighty
    // different stamps spread across the sender's pacing queue. That spread is
    // the sender's, not the link's, and a frame is not usable until its last
    // piece lands -- so what says how long the link took is the newest stamp
    // against the arrival of the piece that completed it. Taking the first
    // instead would fold the whole of the sender's pacing back into the number
    // this device buys buffer against. Signed, so the stamp wrapping every 49
    // days does not read as a jump backwards.
    if (static_cast<int32_t>(header.sent_at_ms - slot.sent_at_ms) > 0) {
        slot.sent_at_ms = header.sent_at_ms;
    }
    if (!slot.received[header.fragment_index]) {
        slot.fragments[header.fragment_index].assign(chunk, chunk + chunk_size);
        slot.received[header.fragment_index] = true;
        ++slot.received_count;
    }
    if (slot.received_count < slot.fragment_count) {
        return;
    }
    if (header.keyframe) {
        // The answer landed: this picture stands on its own, so whatever was
        // lost before it no longer matters. Cleared here rather than when the
        // request was sent, because a request can be lost -- see
        // keyframe_wanted_top_.
        bool& wanted =
            (slot.screen == ScreenId::Top) ? keyframe_wanted_top_ : keyframe_wanted_bottom_;
        wanted = false;
        // Asking worked, so the next thing that goes wrong starts from a
        // short wait again rather than from however long the last spell of
        // trouble had stretched it to.
        ((slot.screen == ScreenId::Top) ? keyframe_wait_top_ms_ : keyframe_wait_bottom_ms_) =
            kKeyframeRequestIntervalMs;
    }

    // A picture that only says what changed since the one before it, with the
    // one before it lost, is not shown. H.264 carries on decoding such a
    // picture without complaint and produces exactly what was seen on a
    // Switch over a bad stretch of wifi: the last good frame smeared and
    // broken up by changes made to one it never had, until the keyframe
    // asked for above arrived. The last good picture stays up instead --
    // a tenth of a second held, usually, rather than a tenth of a second of
    // rubbish -- and the picture resumes clean from the keyframe. Still
    // acked below, so the host keeps sending.
    //
    // Only video: a tile picture is built against a baseline the host
    // rebases on acks, and is whole in itself as far as this end goes.
    const bool awaiting_keyframe =
        slot.encoding == FrameEncoding::H264 && !header.keyframe &&
        ((slot.screen == ScreenId::Top) ? keyframe_wanted_top_ : keyframe_wanted_bottom_);

    FrameMessage frame;
    frame.screen = slot.screen;
    frame.serial = slot.serial;
    frame.width = slot.width;
    frame.height = slot.height;
    frame.encoding = slot.encoding;
    const uint32_t sent_at_ms = slot.sent_at_ms;
    for (const std::vector<uint8_t>& fragment : slot.fragments) {
        frame.payload.insert(frame.payload.end(), fragment.begin(), fragment.end());
    }
    slot = FrameReassembly();

    // Held rather than applied here: poll() shows frames on a clock, so the
    // cadence on screen is even whatever the arrivals did.
    if (frame.serial != received_serial_) {
        const uint64_t now = monotonic_millis();
        // How long this frame took to get here, on two clocks that disagree by
        // an unknown constant. Only ever compared with other transits, where
        // that constant cancels -- see VideoJitter.
        //
        // Both sides are truncated to 32 bits on the wire, so this is done in
        // 32-bit arithmetic and allowed to wrap: the difference comes out
        // right across the wrap, which a wider subtraction would get wrong
        // once every 49 days.
        const uint32_t sent = sent_at_ms;
        double link_delay_ms = -1.0;
        if (sent != 0 && this_pass_measures_the_link_) {
            const uint32_t arrived = static_cast<uint32_t>(now);
            const double transit = static_cast<double>(static_cast<int32_t>(arrived - sent));
            jitter_.add(transit);
            link_delay_ms = jitter_.above_quietest(transit);
        }
        // Separately: how fast frames are being sent, which is what the
        // picture is advanced at. A frame the host never sent cannot be shown,
        // so the cadence on screen is the delivery's and not the source's --
        // but read off the host's own send stamps, not off when frames happen
        // to be read here. Arrivals are bunched by the link and by this
        // device's loop, which only looks once a refresh: after a wifi hiccup
        // two frames land together, one of the gaps is nothing and the other
        // is two periods, and the short one was being thrown away as noise.
        // That read a 60fps stream as slower than it is, the clock ran slow
        // by the difference, and the queue grew behind it. The sender's clock
        // has none of that in it.
        if (sent != 0 && last_sent_at_ms_ != 0) {
            const double gap = static_cast<double>(static_cast<int32_t>(sent - last_sent_at_ms_));
            if (gap >= 4.0 && gap <= 100.0) {
                frame_period_ms_ = frame_period_ms_ * 0.95 + gap * 0.05;
            }
        } else if (sent == 0 && last_arrival_ms_ != 0 && now > last_arrival_ms_) {
            // A host that stamps nothing: arrivals are all there is.
            const double gap = static_cast<double>(now - last_arrival_ms_);
            if (gap >= 4.0 && gap <= 100.0) {
                frame_period_ms_ = frame_period_ms_ * 0.95 + gap * 0.05;
            }
        }
        last_arrival_ms_ = now;
        last_sent_at_ms_ = sent;
        received_serial_ = frame.serial;
        ++adapt_arrivals_;
        complete_at_.push_back(Arrival{frame.serial, now, link_delay_ms});
        // A frame that is never shown (a serial dropped for being behind)
        // would otherwise leave its entry here for ever.
        while (complete_at_.size() > 240) {
            complete_at_.pop_front();
        }
    }
    if (awaiting_keyframe) {
        ++stats_frames_skipped_;
        return;
    }
    pending_frames_.push_back(std::move(frame));
}

void CompanionSession::poll(uint64_t now_ms) {
    browser_.poll(now_ms);
    knock_on_the_local_network();

    // Whether what arrives this pass can be measured against the link at all.
    // Decided before anything is read, from how long this device was away
    // rather than from anything the datagrams themselves say -- they have no
    // way to know they were sitting in a socket here. The first pass of a
    // session has nothing to compare against and is taken at face value.
    const uint64_t poll_gap_ms = (last_poll_ms_ == 0 || now_ms < last_poll_ms_)
                                     ? 0
                                     : now_ms - last_poll_ms_;
    if (last_poll_ms_ != 0) {
        if (recent_poll_gaps_.size() < kPollGapWindow) {
            recent_poll_gaps_.push_back(poll_gap_ms);
        } else {
            recent_poll_gaps_[next_poll_gap_] = poll_gap_ms;
            next_poll_gap_ = (next_poll_gap_ + 1) % kPollGapWindow;
        }
        typical_poll_gap_ms_ =
            *std::min_element(recent_poll_gaps_.begin(), recent_poll_gaps_.end());
    }
    this_pass_measures_the_link_ = poll_gap_measures_the_link(poll_gap_ms, typical_poll_gap_ms_);
    last_poll_ms_ = now_ms;

    // Nothing to do without a socket -- except in the one state where making
    // one is the whole job: an attempt that was refused has closed its socket
    // and owes another, and this pass is what makes it. Returning early here
    // regardless is what left the retry below as code that never ran: the
    // join said it was trying again and then sat on "Connecting" for ever,
    // never retrying and never even reaching its own deadline, until the
    // player gave up and started over -- which is exactly what it looked
    // like from the outside.
    if (!stream_.open() && !retry_connect_) {
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
        // A join being refused the local network is not waiting on a device,
        // it is waiting on this machine -- possibly on a person answering a
        // prompt that a fullscreen window may be sitting on top of. Giving up
        // in six seconds guarantees they cannot. Nothing is lost by waiting:
        // the way out is on screen the whole time.
        const uint64_t allowed_ms =
            waiting_on_local_network_ ? kLocalNetworkJoinTimeoutMs : kJoinTimeoutMs;
        if (connect_deadline_ms_ == 0) {
            connect_deadline_ms_ = now_ms + allowed_ms;
        } else if (waiting_on_local_network_ &&
                   connect_deadline_ms_ < now_ms + kLocalNetworkJoinTimeoutMs - kJoinTimeoutMs) {
            // Learned only once the first attempt came back, so the deadline
            // set before that is lengthened rather than left where it was.
            connect_deadline_ms_ = now_ms + allowed_ms;
        } else if (now_ms >= connect_deadline_ms_) {
            // What went wrong, where anything did. A join that spent its whole
            // deadline being refused the local network must not tell the
            // player the other device stayed quiet: it never heard the ask.
            fail(connect_failure_.empty() ? std::string("that device did not answer")
                                          : connect_failure_);
            return;
        }
        // An attempt the operating system had not made its mind up about is
        // owed another one. Until that one is due there is no socket here to
        // pump, so this is where the poll ends.
        if (retry_connect_) {
            if (retry_connect_at_ms_ == 0) {
                retry_connect_at_ms_ = now_ms + kConnectRetryMs;
            }
            if (now_ms < retry_connect_at_ms_) {
                return;
            }
            retry_connect_ = false;
            retry_connect_at_ms_ = 0;
            begin_connect();
            if (status_ != Status::Connecting || retry_connect_) {
                return;
            }
        }
    }

    if (!stream_.open()) {
        // The attempt made just above did not get as far as a socket, and the
        // rest of this pass is all about one.
        return;
    }

    if (status_ == Status::Connected) {
        report_stream(now_ms);
    }

    std::string error;
    if (!stream_.pump(inbox_, &error)) {
        // A refusal the operating system may take back in a moment is not the
        // end of the join, whichever of the two ways it arrives -- straight
        // out of connect(), or later through the socket's own error. Both land
        // here; see connect_is_undecided().
        if (status_ == Status::Connecting && connect_is_undecided(stream_.connect_error())) {
            owe_another_attempt(error);
            return;
        }
        fail(error);
        return;
    }

    // The hello waits until the TCP handshake has actually completed, so it is
    // never queued against a socket that turns out to be unreachable.
    if (!said_hello_ && stream_.connected()) {
        // Whatever was holding this up has let go: the handshake is through.
        waiting_on_local_network_ = false;
        if (connect_attempts_ > 1) {
            // Worth saying out loud: it means the waiting was the gate opening
            // and not the other device being slow, and it is the only way to
            // tell whether the retry is doing its job at all.
            log_info("reached that device on attempt " + std::to_string(connect_attempts_) +
                     "; the first ones were refused by this machine, not by it");
        }
        HelloMessage hello;
        hello.device_name = device_name_;
        hello.wanted_screen = wanted_screen_;
        // Said rather than assumed: a build that cannot decode H.264 has to
        // say so, because the host picks how to send a screen from this and
        // will send none at all if the answer is no.
        //
        // can_decode(), not have_ffmpeg(). iOS and tvOS have no FFmpeg and
        // decode through VideoToolbox, and asking the FFmpeg question got the
        // answer "no video" out of a device that decodes video perfectly well.
        hello.accepts_video = video::can_decode();
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
        const auto take = [&](const std::vector<uint8_t>& datagram) {
            uint64_t token = 0;
            MessageType type = MessageType::Ping;
            const uint8_t* payload = nullptr;
            std::size_t payload_size = 0;
            if (!decode_media_header(datagram.data(), datagram.size(), token, type, payload,
                                     payload_size)) {
                return;
            }
            if (session_token_ == 0 || token != session_token_) {
                return;
            }
            handle_media_datagram(type, payload, payload_size);
        };
        // For measuring only: DUALSCREEN_TEST_LINK=wifi holds each datagram
        // back the way a home wireless link does -- a few milliseconds as a
        // rule, and now and then twenty or thirty -- so the pacing can be
        // judged against jitter on a machine whose loopback has none.
        static const bool simulate_wifi = [] {
            const char* value = std::getenv("DUALSCREEN_TEST_LINK");
            return value != nullptr && std::string(value) == "wifi";
        }();
        std::vector<uint8_t> datagram;
        std::string from_address;
        while (media_socket_.receive(datagram, from_address)) {
            if (!simulate_wifi) {
                take(datagram);
                continue;
            }
            static std::mt19937 random(12345);
            std::exponential_distribution<double> spread(1.0 / 2.0);
            std::uniform_real_distribution<double> chance(0.0, 1.0);
            double delay = 2.0 + spread(random);
            // From when the host sent it, where that is known, rather than
            // from now: it has already sat in this machine's receive buffer
            // since then, waiting for this pass, and a real link's delay
            // overlaps that wait instead of starting after it. Both ends run
            // on one machine's clock here.
            uint64_t sent_ms = now_ms;
            {
                uint64_t token = 0;
                MessageType type = MessageType::Ping;
                const uint8_t* payload = nullptr;
                std::size_t payload_size = 0;
                FrameFragmentHeader header;
                const uint8_t* body = nullptr;
                std::size_t body_size = 0;
                if (decode_media_header(datagram.data(), datagram.size(), token, type, payload,
                                        payload_size) &&
                    type == MessageType::Frame &&
                    decode_frame_fragment(payload, payload_size, header, body, body_size) &&
                    header.sent_at_ms != 0) {
                    const auto ago = static_cast<int32_t>(static_cast<uint32_t>(now_ms) -
                                                          header.sent_at_ms);
                    if (ago >= 0 && ago < 100) sent_ms = now_ms - static_cast<uint64_t>(ago);
                }
            }
            // And about twice a second the air is busy for twenty or thirty
            // milliseconds, and everything sent meanwhile waits it out.
            static uint64_t stall_from_ms = 0;
            static uint64_t stall_until_ms = 0;
            while (sent_ms >= stall_until_ms) {
                stall_from_ms = std::max(stall_until_ms, sent_ms) +
                                static_cast<uint64_t>(
                                    std::exponential_distribution<double>(1.0 / 500.0)(random));
                stall_until_ms =
                    stall_from_ms + 15 + static_cast<uint64_t>(chance(random) * 20.0);
            }
            if (sent_ms >= stall_from_ms) {
                delay = std::max(delay, static_cast<double>(stall_until_ms - sent_ms));
            }
            const uint64_t due = std::max(simulated_link_last_due_ms_,
                                          sent_ms + static_cast<uint64_t>(delay + 0.5));
            simulated_link_last_due_ms_ = due;
            simulated_link_.push_back({due, datagram});
        }
        while (!simulated_link_.empty() && simulated_link_.front().first <= now_ms) {
            take(simulated_link_.front().second);
            simulated_link_.pop_front();
        }
    }

    // Advance the picture on the host's cadence rather than this device's.
    //
    // A frame that arrived early waits until it is due, which is what pays for
    // the one that arrives late; the two are the same jitter either side of
    // the same clock. Beyond what this link's own jitter calls for, waiting is
    // a backlog rather than cover, and holding that would only show old
    // pictures late, so it is caught up on instead.
    if (!pending_frames_.empty()) {
        const uint64_t period = frame_period();
        const std::size_t waiting = buffered_frames();
        // Skipping frames is the last resort here, not the ordinary way of
        // keeping up: every frame past the first in one pass is a frame that
        // is decoded, applied and then immediately overwritten, so nobody ever
        // sees it. Ordinary backlog is given back by pacing instead -- see
        // video_pace_step_ms() -- and only a queue past the ceiling, which is
        // further behind than gentle pacing will clear in any reasonable time,
        // is worth catching up on this way.
        std::size_t budget =
            waiting > kMaxVideoBufferFrames ? waiting - kMaxVideoBufferFrames : 0;
        if (next_due_ms_ == 0 || now_ms >= next_due_ms_) {
            budget = std::max<std::size_t>(budget, 1);
        }
        // The clock above shows one frame per period, which keeps the cadence
        // even -- and conserves whatever depth the queue happens to have,
        // because one frame in is one frame out for ever. So a backlog built
        // during a single hiccup is never given back: it becomes latency for
        // the rest of the session. Measured on real hardware, a session that
        // started out showing frames the moment they arrived climbed over
        // about twenty seconds to sitting two whole frames -- 66ms at this
        // stream's rate -- behind, and stayed exactly there. Capping the depth
        // does not help, because the cap is what it settles at.
        //
        // Whether that backlog is currently worth giving back. What is done
        // about it is below, where the clock is advanced: showing an extra
        // frame here would drain it in one pass, but an extra frame shown is
        // an extra frame *skipped*, since both land in the same pass and only
        // the last is ever seen. That was tried, and traded the latency for
        // visible judder several times a second -- a bad bargain, and a much
        // more noticeable one than the delay it bought back.
        const uint64_t allowance =
            video_hold_allowance_ms(jitter_.estimate(), period, extra_hold_ms_, video_hold_);
        const bool draining =
            !complete_at_.empty() && now_ms > complete_at_.front().at_ms + allowance;
        // A frame behind on top of the cover -- a second frame already here
        // behind the one due, and the one due held past the cover by half a
        // period or more -- for long enough to call it a backlog rather than
        // a moment: one frame goes unseen. See shed_a_frame_to_catch_up().
        // Half a period, not a whole one: a single frame queued behind the
        // one on screen waits just under a period at the display's own rate,
        // and demanding a whole one left exactly that frame standing for good
        // after every burst.
        const bool behind = waiting >= 2 && !complete_at_.empty() &&
                            now_ms > complete_at_.front().at_ms + allowance + period / 2;
        if (!behind) {
            behind_since_ms_ = 0;
        } else if (behind_since_ms_ == 0) {
            behind_since_ms_ = now_ms;
        }
        if (behind && budget >= 1 &&
            shed_a_frame_to_catch_up(now_ms - behind_since_ms_,
                                     last_shed_ms_ == 0 ? UINT64_MAX : now_ms - last_shed_ms_,
                                     period, video_hold_)) {
            budget = std::max<std::size_t>(budget, 2);
            last_shed_ms_ = now_ms;
            behind_since_ms_ = 0;
            ++stats_shed_;
        }
        std::size_t shown = 0;
        for (; shown < budget && decode_next_frame(); ++shown) {
        }
        if (shown > 0) {
            // Whether this picture missed its turn, which is the only form of
            // running dry a player can see.
            //
            // Not "the queue was empty when the clock came due": at the least
            // latency this can run at, that is the *normal* state -- the clock
            // comes due, the frame arrives a moment later, and it is shown on
            // time with nothing ever queued behind it. Counting that read as
            // thirty stalls a second on a stream that was keeping up perfectly,
            // and drove the cover below straight to its ceiling, which is
            // exactly the delay it exists to avoid buying. A frame is late when
            // it is *shown* late.
            // And not on a pass this device was late to: the frame is late
            // either way, but a stall here is one no amount of cover could
            // have covered -- the queue stands still exactly as long as the
            // loop does. Buying delay for it is paying twice for one pause.
            // And not at a cadence this device cannot pace at all: at 120
            // frames a second the deadline is 4ms and this loop's quickest
            // pass is 9, so every frame is late by construction and the cover
            // bought for it covers nothing. See lateness_is_worth_covering().
            if (this_pass_measures_the_link_ && next_due_ms_ != 0 &&
                lateness_is_worth_covering(period, typical_poll_gap_ms_) &&
                now_ms > next_due_ms_ + lateness_that_counts_ms(period, typical_poll_gap_ms_) &&
                last_arrival_ms_ != 0 &&
                now_ms - last_arrival_ms_ <= kStreamIsLiveMs) {
                // And only for what the link did. See lateness_was_the_links().
                if (lateness_was_the_links(shown_link_delay_ms_, period)) {
                    ++stats_starved_;
                } else {
                    ++stats_late_from_host_;
                }
            }
            // Advanced from when it was due, not from now, so the pace does
            // not drift slower by however late each pass happened to run. A
            // clock that has fallen far behind is started again instead.
            const double exact_period = std::min(40.0, std::max(8.0, frame_period_ms_));
            if (next_due_ms_ == 0 || now_ms > next_due_ms_ + period * 4) {
                next_due_exact_ms_ = static_cast<double>(now_ms) + exact_period;
            } else {
                next_due_exact_ms_ += video_pace_step_exact_ms(exact_period, shown, draining);
            }
            next_due_ms_ = static_cast<uint64_t>(std::llround(next_due_exact_ms_));
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
    ask_for_keyframes(now_ms);
    adapt_to_what_this_device_can_decode(now_ms);

    if (status_ == Status::Connected && received_serial_ != acked_serial_) {
        const uint64_t newest = received_serial_;
        acked_serial_ = newest;
        std::vector<uint8_t> ack;
        encode_media_header(session_token_, MessageType::FrameAck, ack);
        encode_frame_ack_payload(newest, ack);
        media_socket_.send_to(host_address_, kMediaPort, ack.data(), ack.size());
    }
}

// Reaches out to each host as soon as it is found, before anybody has picked
// one.
//
// macOS decides whether an app may use the local network the first time the
// app actually tries to, and the attempt that asks the question is the one
// that gets refused -- which is why joining used to fail and then work. The
// retry in poll() covers that, but only after the player has already pressed
// the button and is watching a screen that says "Connecting"; if a prompt is
// what the system decides to show, they are watching it during the seconds
// they have to answer a prompt they may not even be able to see behind a
// fullscreen window.
//
// So the question gets asked while they are still reading the list. One
// datagram per host, the first time it appears: the host's media socket reads
// the token, does not recognise it, and drops it -- see
// HostSession::poll_media(). Nothing on the far end has to know about this.
//
// Only where there is a gate to trigger. Everywhere else this would be a
// stray datagram for no reason at all.
void CompanionSession::knock_on_the_local_network() {
#if defined(__APPLE__)
    if (status_ != Status::Searching) {
        return;
    }
    for (const DiscoveredHost& host : browser_.hosts()) {
        if (host.address.empty() ||
            std::find(knocked_.begin(), knocked_.end(), host.address) != knocked_.end()) {
            continue;
        }
        knocked_.push_back(host.address);
        if (!media_socket_.open()) {
            std::string error;
            if (!media_socket_.start(0, false, &error)) {
                return;
            }
        }
        // A media header with a token nobody will match. The point is the
        // send, not the contents.
        std::vector<uint8_t> hello;
        encode_media_header(0, MessageType::Ping, hello);
        media_socket_.send_to(host.address, kMediaPort, hello.data(), hello.size());
        log_debug("asked the operating system for the local network early, so that joining " +
                  host.device_name + " does not have to wait for it");
    }
#endif
}

void CompanionSession::ask_for_keyframes(uint64_t now_ms) {
    if (status_ != Status::Connected) {
        return;
    }
    const struct {
        ScreenId screen;
        bool& wanted;
        uint64_t& asked_at;
        uint64_t& wait_ms;
    } screens[] = {
        {ScreenId::Top, keyframe_wanted_top_, keyframe_asked_at_top_, keyframe_wait_top_ms_},
        {ScreenId::Bottom, keyframe_wanted_bottom_, keyframe_asked_at_bottom_,
         keyframe_wait_bottom_ms_}};
    for (const auto& entry : screens) {
        if (!entry.wanted) {
            continue;
        }
        // Repeated, not sent once: the request itself can be lost, and so can
        // the keyframe answering it. Slower than the frame rate on purpose --
        // a keyframe is tens of times the size of an ordinary frame, so
        // asking again before the last answer could plausibly have arrived
        // would pile several of those onto a link that has just shown it is
        // dropping things.
        if (entry.asked_at != 0 && now_ms - entry.asked_at < entry.wait_ms) {
            continue;
        }
        entry.asked_at = now_ms;
        // Longer every time asking does not work -- see
        // kMaxKeyframeRequestIntervalMs for why repeating at full speed makes
        // a bad link worse rather than better.
        entry.wait_ms = std::min(entry.wait_ms * 2, kMaxKeyframeRequestIntervalMs);
        KeyframeRequestMessage request;
        request.screen = entry.screen;
        request.last_good_serial = received_serial_;
        std::vector<uint8_t> out;
        encode_media_header(session_token_, MessageType::KeyframeRequest, out);
        encode_keyframe_request_payload(request, out);
        media_socket_.send_to(host_address_, kMediaPort, out.data(), out.size());
    }
}

namespace {

bool same_pads(const std::vector<ReportedPad>& a, const std::vector<ReportedPad>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t index = 0; index < a.size(); ++index) {
        const DsInputState& x = a[index].held;
        const DsInputState& y = b[index].held;
        if (a[index].id != b[index].id || a[index].name != b[index].name ||
            a[index].slot != b[index].slot ||
            x.buttons != y.buttons || x.circle_x != y.circle_x ||
            x.circle_y != y.circle_y || x.c_stick_x != y.c_stick_x ||
            x.c_stick_y != y.c_stick_y || x.motion != y.motion) {
            return false;
        }
    }
    return true;
}

}  // namespace

void CompanionSession::send_input(const DsInputState& state,
                                  const std::vector<ReportedPad>* pads) {
    if (status_ != Status::Connected || !stream_.connected()) {
        return;
    }
    // Only changes go out. Holding a button steady costs nothing.
    //
    // Motion is the exception that swallows the rule when it is switched on:
    // a device being held by a person is never quite still, so the reading
    // differs every frame and this sends every frame. That is the point --
    // a gyro is only any use as a stream -- and it is why nothing turns the
    // sensors on until the host says it reads them. One datagram of about
    // fifty bytes per frame is nothing next to the video going the other way.
    if (ever_sent_input_ && state.motion == last_sent_input_.motion &&
        state.buttons == last_sent_input_.buttons &&
        state.touching == last_sent_input_.touching &&
        state.circle_x == last_sent_input_.circle_x &&
        state.circle_y == last_sent_input_.circle_y &&
        state.c_stick_x == last_sent_input_.c_stick_x &&
        state.c_stick_y == last_sent_input_.c_stick_y &&
        (!state.touching || (state.touch_x == last_sent_input_.touch_x &&
                             state.touch_y == last_sent_input_.touch_y)) &&
        (pads != nullptr) == last_sent_reported_pads_ &&
        (pads == nullptr || same_pads(*pads, last_sent_pads_))) {
        // Nothing new -- but the last change goes out again for a pass or
        // two, as the same numbered input, in case the first copy was lost.
        if (input_repeats_left_ > 0 && !last_input_datagram_.empty()) {
            --input_repeats_left_;
            media_socket_.send_to(host_address_, kMediaPort, last_input_datagram_.data(),
                                  last_input_datagram_.size());
        }
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
    // Only when the host said it reads it -- see host_wants_motion(). A
    // device whose sensors are running anyway (they are shared with whatever
    // else on this machine wanted them) should still not be filling the
    // host's pad with readings nothing there will look at.
    if (host_wants_motion_) {
        message.motion = state.motion;
    }
    if (pads != nullptr) {
        message.reports_pads = true;
        message.pads = *pads;
        // Motion on the same terms as the device's own, above.
        if (!host_wants_motion_) {
            for (ReportedPad& pad : message.pads) pad.held.motion = DsMotionState();
        }
    }
    // Numbered where there is somewhere to put the number -- see
    // InputMessage::sequence.
    if (message.reports_pads) {
        message.has_sequence = true;
        message.sequence = ++input_sequence_;
        constexpr std::size_t kSentInputsKept = 64;
        if (input_sent_at_.size() >= kSentInputsKept) {
            input_sent_at_.erase(input_sent_at_.begin());
        }
        input_sent_at_.push_back(SentInput{message.sequence, monotonic_millis()});
    }
    std::vector<uint8_t> out;
    encode_media_header(session_token_, MessageType::Input, out);
    encode_input_payload(message, out);
    media_socket_.send_to(host_address_, kMediaPort, out.data(), out.size());
    // Sent twice more on the passes that follow; numbered, so the host takes
    // it once. An unnumbered one would be taken again, which for a press is
    // harmless but is still not worth the doubt.
    if (message.has_sequence) {
        last_input_datagram_ = out;
        input_repeats_left_ = 2;
    } else {
        last_input_datagram_.clear();
        input_repeats_left_ = 0;
    }

    last_sent_input_ = state;
    last_sent_reported_pads_ = pads != nullptr;
    last_sent_pads_ = pads != nullptr ? *pads : std::vector<ReportedPad>();
    ever_sent_input_ = true;
}

namespace {

// The same arithmetic the fragment shader does, for the one caller that wants
// pixels on the CPU. Full-range BT.601, matching what the encoder signals.
void expand_planar_yuv(video::DecodedLayout layout, const std::vector<uint8_t>* plane,
                       const int* stride, int width, int height,
                       std::vector<uint32_t>& out) {
    out.assign(static_cast<std::size_t>(std::max(0, width)) * std::max(0, height), 0u);
    if (width <= 0 || height <= 0 || plane[0].empty() || plane[1].empty()) {
        return;
    }
    const bool nv12 = layout == video::DecodedLayout::Nv12;
    if (!nv12 && plane[2].empty()) {
        return;
    }
    const auto clamp8 = [](float value) {
        return static_cast<uint32_t>(std::min(255.0f, std::max(0.0f, value)));
    };
    for (int y = 0; y < height; ++y) {
        const uint8_t* luma_row = plane[0].data() + static_cast<std::size_t>(y) * stride[0];
        const int chroma_y = y / 2;
        for (int x = 0; x < width; ++x) {
            const int chroma_x = x / 2;
            float cb = 0.0f;
            float cr = 0.0f;
            if (nv12) {
                const uint8_t* row = plane[1].data() + static_cast<std::size_t>(chroma_y) * stride[1];
                cb = row[chroma_x * 2] - 128.0f;
                cr = row[chroma_x * 2 + 1] - 128.0f;
            } else {
                cb = plane[1][static_cast<std::size_t>(chroma_y) * stride[1] + chroma_x] - 128.0f;
                cr = plane[2][static_cast<std::size_t>(chroma_y) * stride[2] + chroma_x] - 128.0f;
            }
            const float luma = static_cast<float>(luma_row[x]);
            const uint32_t r = clamp8(luma + 1.402f * cr);
            const uint32_t g = clamp8(luma - 0.344136f * cb - 0.714136f * cr);
            const uint32_t b = clamp8(luma + 1.772f * cb);
            out[static_cast<std::size_t>(y) * width + x] = (r << 16) | (g << 8) | b;
        }
    }
}

}  // namespace

const std::vector<uint32_t>& CompanionSession::ScreenBuffer::expanded() const {
    // A video frame that came back already converted is in this form
    // already. There is nothing to expand and nothing to cache.
    if (video && layout == video::DecodedLayout::Xrgb8888) {
        return pixels;
    }
    // One that came back as planes has to be converted for a caller that
    // wants pixels on the CPU -- which the renderer never does, since it
    // uploads the planes and converts while drawing. This is the fallback for
    // anything else that asks (tests, and a client with no GL path), computed
    // once per frame rather than on every ask.
    if (video) {
        if (!ever_expanded || expanded_serial != serial) {
            // A picture still in the decoder's memory is copied out first --
            // the slow read the renderer never does, for a caller that wants
            // pixels anyway.
            video::DecodedFrame copied;
            if (plane[0].empty() && video::copy_native_planes(native, copied)) {
                expand_planar_yuv(copied.layout, copied.plane, copied.stride, width, height,
                                  pixels);
            } else {
                expand_planar_yuv(layout, plane, plane_stride, width, height, pixels);
            }
            expanded_serial = serial;
            ever_expanded = true;
        }
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
    // Asked of the buffer rather than by expanding it: turning a video frame
    // into pixels on the CPU is a pass over every one of them, to answer a
    // yes or no.
    const ScreenBuffer& buffer = (screen == ScreenId::Top) ? top_ : bottom_;
    return buffer.has_frame() && buffer.width > 0 && buffer.height > 0;
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

void CompanionSession::request_left_stick_press(LeftStickPress press) {
    RequestMessage request;
    request.wants_left_stick_press = true;
    request.left_stick_press = press;
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
    // Remembered rather than sent straight out, because what actually goes to
    // the host is this scaled by whatever this device has shown it can keep up
    // with -- see display_scale_. A caller resizing its window is still saying
    // the same thing it always was; how much of that is worth asking for is
    // this end's own business.
    wanted_display_width_ = width;
    wanted_display_height_ = height;
    send_display_size();
}

bool picture_shrank_enough_to_judge(int before_width, int after_width) {
    if (before_width <= 0 || after_width <= 0) {
        return false;
    }
    return static_cast<double>(after_width) <= static_cast<double>(before_width) * 0.9;
}

bool shrinking_is_worth_it(double before_ms, double after_ms) {
    if (!(before_ms > 0.0)) {
        return true;  // nothing to compare against yet: try it and find out
    }
    return after_ms <= before_ms * 0.9;
}

double scale_the_picture_arrived_at(int width, int height, int box_width, int box_height) {
    if (width <= 0 || height <= 0 || box_width <= 0 || box_height <= 0) {
        return 0.0;
    }
    return std::max(static_cast<double>(width) / box_width,
                    static_cast<double>(height) / box_height);
}

bool host_held_pictures_back(uint64_t serials_advanced, uint64_t arrived) {
    if (serials_advanced == 0) {
        return false;
    }
    return arrived * 10 < serials_advanced * 9;
}

bool picture_size_changed_enough(int before_width, int after_width) {
    return picture_shrank_enough_to_judge(before_width, after_width) ||
           picture_shrank_enough_to_judge(after_width, before_width);
}

double lowest_display_scale(int panel_width) {
    if (panel_width <= 0) {
        return kMinDisplayScale;
    }
    // The lower of the two bars, so no device loses headroom it had: on a
    // panel narrower than twice kMinDisplayWidth the fraction is still what
    // binds, and on anything wider the picture size is.
    const double by_pixels = static_cast<double>(kMinDisplayWidth) / panel_width;
    return std::min(kMinDisplayScale, by_pixels);
}

void CompanionSession::send_display_size() {
    if (wanted_display_width_ <= 0 || wanted_display_height_ <= 0) {
        return;
    }
    // Even numbers: the host encodes 4:2:0, which cannot represent an odd
    // dimension, and it would only round this down again itself.
    const int width = std::max(2, static_cast<int>(wanted_display_width_ * display_scale_) & ~1);
    const int height = std::max(2, static_cast<int>(wanted_display_height_ * display_scale_) & ~1);
    // Whatever cover was bought is about to stop being about anything.
    //
    // extra_hold_ms_ is delay this device bought itself after the picture ran
    // dry -- see hold_extra_ms(). It is bought a quarter of a frame at a time
    // and given back only after ten calm seconds, which is the right shape for
    // a link that twitches and the wrong one for this: every session starts
    // with the ladder walking down, every rung of that walk starves, and by
    // the time it settles this device is holding up to two frames of delay
    // bought against a stream that no longer exists. On a 30fps stream that
    // is 66ms, on a link whose whole round trip is 40ms, and it takes the best
    // part of twenty seconds to come back -- longer, because one bad second
    // anywhere resets the calm count.
    //
    // A different picture size is a different stream. The evidence goes with
    // it.
    if (extra_hold_ms_ > 0) {
        log_info("the picture size changed, so the " + std::to_string(extra_hold_ms_) +
                 "ms of delay bought back after earlier stalls is dropped: it was bought "
                 "against a stream that no longer exists");
        extra_hold_ms_ = 0;
        calm_seconds_ = 0;
    }
    RequestMessage request;
    request.wants_display_size = true;
    // Clamped rather than rejected: a window can be zero-sized for a moment
    // while minimised, and 1 is a harmless value to send rather than a value
    // worth guarding against on the host too.
    request.display_width = static_cast<uint16_t>(std::clamp(width, 1, 65535));
    request.display_height = static_cast<uint16_t>(std::clamp(height, 1, 65535));
    send_request(request);
}

void CompanionSession::adapt_to_what_this_device_can_decode(uint64_t stale_now_ms) {
    if (status_ != Status::Connected || wanted_display_width_ <= 0) {
        return;
    }
    // The clock read here, not the one poll() was handed.
    //
    // poll() is given the time at the top of the loop and decodes on the way
    // down -- see the decode_next_frame() loop above -- so by the time this
    // runs, the timestamp is older than the work it is about to divide by.
    // Every window therefore counted one poll's decoding against an elapsed
    // time that did not contain it, and a poll that decodes several frames is
    // a hundred milliseconds of that. It showed: an Apple TV reported "1114ms
    // of every second", which is not a thing that can happen on one thread.
    //
    // Same clock as decode_next_frame() times itself with, so the two are
    // comparable at all.
    (void)stale_now_ms;
    const uint64_t now_ms = monotonic_millis();
    if (adapt_at_ms_ == 0) {
        adapt_at_ms_ = now_ms;
        adapt_decode_ms_ = 0;
        // And this one, which was left behind when the rest was cleared: a
        // window that starts with the previous session's microseconds already
        // in it reports a readback cost that nothing in this window paid.
        adapt_readback_micros_ = 0;
        adapt_lock_micros_ = 0;
        adapt_frames_ = 0;
        adapt_serial_at_start_ = received_serial_;
        adapt_arrivals_ = 0;
        return;
    }
    // A step whose pictures have not arrived yet has nothing to be judged by
    // -- see adapt_awaiting_from_width_. The window starts again when they
    // do; one that never comes is judged as it stands.
    if (adapt_awaiting_from_width_ > 0) {
        if (now_ms - adapt_awaiting_since_ms_ < kAwaitNewSizeMs) {
            return;
        }
        adapt_awaiting_from_width_ = 0;
    }
    const uint64_t elapsed = now_ms - adapt_at_ms_;
    if (elapsed < kAdaptWindowMs) {
        return;
    }
    const uint64_t per_second = adapt_decode_ms_ * 1000 / elapsed;
    const uint64_t readback_per_second = adapt_readback_micros_ / 1000 * 1000 / elapsed;
    // The same second, per picture. This is the number that says whether the
    // picture size decides anything: per second saturates on a device that is
    // the bottleneck, because the smaller the frames the more of them arrive.
    const uint64_t frames = adapt_frames_;
    const double cost_per_frame_ms =
        frames > 0 ? static_cast<double>(adapt_decode_ms_) / static_cast<double>(frames) : 0.0;
    const double readback_per_frame_ms =
        frames > 0 ? static_cast<double>(adapt_readback_micros_) / 1000.0 /
                         static_cast<double>(frames)
                   : 0.0;
    const double lock_per_frame_ms =
        frames > 0
            ? static_cast<double>(adapt_lock_micros_) / 1000.0 / static_cast<double>(frames)
            : 0.0;
    const double arrived_scale = adapt_arrived_scale_;
    const uint64_t serials_advanced = (adapt_serial_at_start_ != 0 &&
                                       received_serial_ > adapt_serial_at_start_)
                                          ? received_serial_ - adapt_serial_at_start_
                                          : 0;
    const bool held_back = host_held_pictures_back(serials_advanced, adapt_arrivals_);
    const uint64_t arrivals = adapt_arrivals_;
    adapt_serial_at_start_ = received_serial_;
    adapt_arrivals_ = 0;
    adapt_at_ms_ = now_ms;
    adapt_decode_ms_ = 0;
    adapt_readback_micros_ = 0;
    adapt_lock_micros_ = 0;
    adapt_frames_ = 0;
    adapt_arrived_scale_ = 0.0;

    // Did the last step down actually buy anything? Asked before deciding to
    // take another one, and only once there is a step to judge -- and only
    // once that step actually made the picture smaller. A request the host was
    // already below changes nothing, and comparing a picture with itself
    // proves nothing about size.
    const bool picture_actually_shrank =
        picture_shrank_enough_to_judge(adapt_width_before_step_, adapt_last_width_);
    if (adapt_size_decides_cost_ && adapt_cost_before_step_ms_ > 0.0 && frames > 0 &&
        !picture_actually_shrank) {
        // The ask did not bite yet. Nothing to learn from it; keep going.
        adapt_cost_before_step_ms_ = 0.0;
        adapt_width_before_step_ = 0;
    } else if (adapt_size_decides_cost_ && adapt_cost_before_step_ms_ > 0.0 && frames > 0) {
        adapt_size_decides_cost_ =
            shrinking_is_worth_it(adapt_cost_before_step_ms_, cost_per_frame_ms);
        if (!adapt_size_decides_cost_ && !said_size_does_not_decide_) {
            said_size_does_not_decide_ = true;
            log_info("a smaller picture did not make this device any quicker (" +
                     std::to_string(static_cast<int>(adapt_cost_before_step_ms_ + 0.5)) +
                     "ms a frame before, " +
                     std::to_string(static_cast<int>(cost_per_frame_ms + 0.5)) +
                     "ms after), so what it costs is not the size of the picture but the "
                     "handling of each one; holding the size it has rather than spending "
                     "more of it for nothing");
        }
        adapt_cost_before_step_ms_ = 0.0;
        adapt_width_before_step_ = 0;
    }

    const double floor_scale = lowest_display_scale(wanted_display_width_);
    if (per_second >= kBusyDecodeMsPerSecond && display_scale_ > floor_scale &&
        adapt_size_decides_cost_) {
        // From the size the pictures are actually arriving at, when that is
        // less than what was asked for -- see scale_the_picture_arrived_at().
        const bool stepping_from_arrived = arrived_scale > 0.0 && arrived_scale < display_scale_;
        const double from = stepping_from_arrived ? arrived_scale : display_scale_;
        // Remembered before stepping down, so the way back up knows where the
        // ceiling is -- see too_expensive_scale_. The size that was measured,
        // not the size that was asked for and never sent.
        too_expensive_scale_ = std::min(too_expensive_scale_, from);
        too_expensive_at_ms_ = now_ms;
        // And remembered so the next window can say whether this was worth it
        // -- the width as well as the cost, because a step that does not make
        // the picture any smaller says nothing about whether size matters.
        adapt_cost_before_step_ms_ = cost_per_frame_ms;
        adapt_width_before_step_ = adapt_last_width_;
        display_scale_ = std::max(floor_scale, from * kDisplayScaleStep);
        adapt_easy_windows_ = 0;
        adapt_awaiting_from_width_ = adapt_last_width_;
        adapt_awaiting_since_ms_ = now_ms;
        // Which half of it, because the two have different answers. Time in
        // the decoder is the decoder's; time in the readback is this app
        // copying a picture out of memory the GPU already has, and that is a
        // copy that does not have to happen at all. Per frame as well as per
        // second, because only one of the two can be compared across a step.
        log_info("this device is spending " + std::to_string(per_second) +
                 "ms of every second turning frames into pictures (" +
                 std::to_string(readback_per_second) +
                 "ms of it copying them out of the decoder), which is more than it can "
                 "keep up with; " +
                 std::to_string(frames) + " pictures, " +
                 std::to_string(static_cast<int>(cost_per_frame_ms + 0.5)) + "ms each (" +
                 std::to_string(static_cast<int>(readback_per_frame_ms + 0.5)) +
                 "ms copying, of which " +
                 std::to_string(static_cast<int>(lock_per_frame_ms + 0.5)) +
                 "ms waiting for the decoder to let go); " +
                 (stepping_from_arrived
                      ? "the pictures were arriving at " +
                            std::to_string(static_cast<int>(from * 100.0 + 0.5)) +
                            "% of it, so asking for "
                      : std::string("asking for ")) +
                 std::to_string(static_cast<int>(display_scale_ * 100.0 + 0.5)) +
                 "% of its display size instead");
        send_display_size();
        return;
    }
    if (per_second <= kEasyDecodeMsPerSecond && display_scale_ < 1.0 && held_back) {
        // Cheap because most of the stream never came -- see
        // host_held_pictures_back(). Not evidence of anything, and it breaks
        // a run of evidence that was building.
        adapt_easy_windows_ = 0;
        if (!said_held_back_is_not_room_) {
            said_held_back_is_not_room_ = true;
            log_info("not asking for a larger picture: this second looked easy only because the "
                     "host held pictures back (" +
                     std::to_string(arrivals) + " of " + std::to_string(serials_advanced) +
                     " arrived)");
        }
        return;
    }
    if (per_second <= kEasyDecodeMsPerSecond && display_scale_ < 1.0) {
        // Several quiet windows, not one: a moment of still picture decodes
        // cheaply on any device, and stepping back up on that alone is how a
        // session ends up oscillating. Deliberately slower than the way down
        // -- three seconds of evidence to ask for more, one to ask for less.
        if (++adapt_easy_windows_ < 3) {
            return;
        }
        const double larger = std::min(1.0, display_scale_ / kDisplayScaleStep);
        // Not back into a size already shown not to work, unless enough time
        // has passed that the game may be doing something else entirely.
        const bool stale = too_expensive_at_ms_ != 0 &&
                           now_ms - too_expensive_at_ms_ >= kRetryLargerAfterMs;
        if (larger >= too_expensive_scale_ && !stale) {
            return;
        }
        if (stale) {
            too_expensive_scale_ = 2.0;
            too_expensive_at_ms_ = 0;
        }
        adapt_easy_windows_ = 0;
        display_scale_ = larger;
        adapt_awaiting_from_width_ = adapt_last_width_;
        adapt_awaiting_since_ms_ = now_ms;
        log_info("this device has room to spare (" + std::to_string(per_second) +
                 "ms of every second); asking for " +
                 std::to_string(static_cast<int>(display_scale_ * 100.0 + 0.5)) +
                 "% of its display size");
        send_display_size();
        return;
    }
    adapt_easy_windows_ = 0;
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
            if (buffer->video && buffer->layout == video::DecodedLayout::Nv12 &&
                buffer->native != nullptr) {
                // Drawn from the decoder's own memory, with nothing copied --
                // see DecodedFrame::native.
                if (buffer->texture.adopt_native_nv12(buffer->native, buffer->width,
                                                      buffer->height)) {
                    native_texture_failures_ = 0;
                } else {
                    // The old way for this picture, and after a few of these
                    // for every picture: a device that cannot map one cannot
                    // map the next, and asking it to every frame only adds a
                    // failure to the copy it ends up making anyway.
                    if (++native_texture_failures_ == kNativeTextureFailuresToGiveUp) {
                        video::set_native_frames_usable(false);
                        log_warn("video: pictures could not be drawn from the decoder's memory, "
                                 "so from now on they are copied out of it instead");
                    }
                    video::DecodedFrame copied;
                    if (video::copy_native_planes(buffer->native, copied)) {
                        buffer->texture.upload_nv12(copied.plane[0].data(), copied.stride[0],
                                                    copied.plane[1].data(), copied.stride[1],
                                                    buffer->width, buffer->height);
                    }
                }
            } else if (buffer->video && buffer->layout == video::DecodedLayout::Nv12) {
                // Straight to the GPU in the planes the decoder produced --
                // the conversion to colour happens in the shader, which is the
                // whole point of asking for them. See DecodedLayout.
                buffer->texture.upload_nv12(buffer->plane[0].data(), buffer->plane_stride[0],
                                            buffer->plane[1].data(), buffer->plane_stride[1],
                                            buffer->width, buffer->height);
            } else if (buffer->video && buffer->layout == video::DecodedLayout::Yuv420p) {
                buffer->texture.upload_yuv420p(
                    buffer->plane[0].data(), buffer->plane_stride[0], buffer->plane[1].data(),
                    buffer->plane_stride[1], buffer->plane[2].data(), buffer->plane_stride[2],
                    buffer->width, buffer->height);
            } else if (buffer->video) {
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

    // Being drawn now: every input whose picture this is has come back.
    shown_serial_ = std::max({shown_serial_, top_.uploaded_serial, bottom_.uploaded_serial});
    if (!echoes_waiting_.empty()) {
        const uint64_t now_ms = monotonic_millis();
        std::vector<InputEchoMessage> still_waiting;
        for (const InputEchoMessage& echo : echoes_waiting_) {
            if (echo.serial > shown_serial_) {
                still_waiting.push_back(echo);
                continue;
            }
            for (const SentInput& sent : input_sent_at_) {
                if (sent.sequence == echo.sequence && now_ms >= sent.at_ms) {
                    stats_input_to_picture_ms_.push_back(now_ms - sent.at_ms);
                    break;
                }
            }
        }
        echoes_waiting_ = std::move(still_waiting);
    }
    return textures_;
}

std::string late_pictures_summary(unsigned held_up_on_the_way, unsigned sent_late) {
    if (held_up_on_the_way == 0 && sent_late == 0) {
        return std::string();
    }
    std::string summary = "; pictures late this second: ";
    summary += std::to_string(held_up_on_the_way) + " held up on the way here";
    summary += ", " + std::to_string(sent_late) +
               " sent late by the host (no buffer covers those, so none is bought for them)";
    return summary;
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
              " drawn/s, newest is " + std::to_string(acked_serial_) + ", held " +
              std::to_string(stats_waits_ > 0 ? stats_wait_total_ms_ / stats_waits_ : 0) +
              "ms avg " + std::to_string(stats_wait_worst_ms_) + "ms worst in a buffer of " +
              std::to_string(target_buffer_frames()) + ", covering up to " +
              std::to_string(video_hold_allowance_ms(jitter_.estimate(), frame_period(),
                                                     extra_hold_ms_, video_hold_)) +
              "ms of which " + std::to_string(extra_hold_ms_) +
              "ms was bought back after the picture ran dry, " +
              std::to_string(stats_starved_) + " times this second (jitter " +
              std::to_string(static_cast<uint64_t>(jitter_.estimate() + 0.5)) + "ms), decode " +
              std::to_string(stats_decodes_ > 0 ? stats_decode_total_ms_ / stats_decodes_ : 0) +
              "ms avg, " + std::to_string(stats_frames_lost_) + " frames lost, " +
              std::to_string(stats_frames_skipped_) +
              " passed over waiting for a keyframe, this device's own loop comes round every " +
              std::to_string(typical_poll_gap_ms_) + "ms at its quickest");
    // And the latency breakdown, at a level the console sees.
    //
    // Everything above is a debug line, which on a device with no command line
    // means a file that needs Xcode and a container download to read -- so the
    // one question a player actually asks, "why is this behind?", has been the
    // one thing invisible in every log sent back. These are the three terms
    // that add up to the answer: what this device is holding on purpose, what
    // the link costs, and what turning a frame into a picture costs.
    log_info("latency: holding " +
             std::to_string(video_hold_allowance_ms(jitter_.estimate(), frame_period(),
                                                    extra_hold_ms_, video_hold_)) +
             "ms of buffer (" + std::to_string(extra_hold_ms_) +
             "ms of it bought back after stalls), " +
             std::to_string(stats_decodes_ > 0 ? stats_decode_total_ms_ / stats_decodes_ : 0) +
             "ms a frame turning them into pictures, and this device's loop comes round every " +
             std::to_string(typical_poll_gap_ms_) + "ms at its quickest" +
             late_pictures_summary(stats_starved_, stats_late_from_host_) +
             (stats_shed_ > 0 ? "; let one picture go unseen to catch up" : ""));
    stats_shed_ = 0;
    // The number the rest of the line adds up to, for anything played by
    // touch or by feel: from an input leaving this device to the first
    // picture the game drew with it being drawn here. Not the display's own
    // scan-out after that, which no software can see.
    if (!stats_input_to_picture_ms_.empty()) {
        std::vector<uint64_t> sorted = stats_input_to_picture_ms_;
        std::sort(sorted.begin(), sorted.end());
        log_info("latency: input to picture " + std::to_string(sorted[sorted.size() / 2]) +
                 "ms typical, " + std::to_string(sorted.front()) + " to " +
                 std::to_string(sorted.back()) + "ms, over " + std::to_string(sorted.size()) +
                 (sorted.size() == 1 ? " input" : " inputs"));
        stats_input_to_picture_ms_.clear();
    }

    // What the buffer is actually being sized against. One number cannot say
    // whether a link is evenly a little late or mostly prompt with a bad tail,
    // and those two want quite different amounts of buffer -- so the curve
    // goes in the log and the choice of where to sit on it (VideoJitter::
    // kCoverage) can be argued from something.
    const VideoTransitShape shape = jitter_.shape();
    log_debug("stream: above the quietest arrival in the window, half of them are within " +
              std::to_string(static_cast<uint64_t>(shape.median_ms + 0.5)) +
              "ms, three quarters within " +
              std::to_string(static_cast<uint64_t>(shape.three_quarters_ms + 0.5)) +
              "ms, nineteen in twenty within " +
              std::to_string(static_cast<uint64_t>(shape.worst_twentieth_ms + 0.5)) + "ms");
    // Once a second, what the stalls have earned. See hold_extra_periods().
    extra_hold_ms_ =
        hold_extra_ms(extra_hold_ms_, frame_period(), stats_starved_, calm_seconds_, video_hold_);
    calm_seconds_ = (stats_starved_ == 0) ? calm_seconds_ + 1 : 0;
    stats_starved_ = 0;
    stats_late_from_host_ = 0;
    stats_at_ms_ = now_ms;
    stats_decoded_ = 0;
    stats_drawn_ = 0;
    stats_wait_total_ms_ = 0;
    stats_wait_worst_ms_ = 0;
    stats_waits_ = 0;
    stats_decode_total_ms_ = 0;
    stats_decodes_ = 0;
    stats_frames_lost_ = 0;
    stats_frames_skipped_ = 0;
}

// How long each frame should be on screen: what the host is producing at,
// clamped so that a stall or a burst cannot leave the picture crawling or
// racing.
uint64_t CompanionSession::frame_period() const {
    const double clamped = std::min(40.0, std::max(8.0, frame_period_ms_));
    return static_cast<uint64_t>(clamped + 0.5);
}

void VideoJitter::reset() {
    recent_.clear();
    next_ = 0;
    estimate_ = 0.0;
}

double transit_quantile_above_quietest(const std::vector<double>& recent, double coverage) {
    if (recent.empty()) {
        return 0.0;
    }
    // Measured up from the quietest transit in the window, which is this link
    // with nothing queued on it -- and the only baseline available at all,
    // since the two clocks' offset is unknown and only differences between
    // transits mean anything.
    //
    // Not the maximum, which one bad moment owns for the whole window; not a
    // mean either, which a long tail of prompt arrivals drags under what the
    // late ones actually need.
    std::vector<double> sorted(recent);
    const std::size_t index = static_cast<std::size_t>(
        std::min<double>(static_cast<double>(sorted.size()) - 1.0,
                         coverage * static_cast<double>(sorted.size())));
    std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(index),
                     sorted.end());
    const double quietest = *std::min_element(recent.begin(), recent.end());
    return std::max(0.0, sorted[index] - quietest);
}

double VideoJitter::above_quietest(double transit_ms) const {
    if (recent_.empty()) {
        return -1.0;
    }
    return std::max(0.0, transit_ms - *std::min_element(recent_.begin(), recent_.end()));
}

void VideoJitter::add(double transit_ms) {
    if (recent_.size() < kWindow) {
        recent_.push_back(transit_ms);
    } else {
        recent_[next_] = transit_ms;
        next_ = (next_ + 1) % kWindow;
    }
    estimate_ = transit_quantile_above_quietest(recent_, kCoverage);
}

VideoTransitShape VideoJitter::shape() const {
    VideoTransitShape out;
    out.median_ms = transit_quantile_above_quietest(recent_, 0.50);
    out.three_quarters_ms = transit_quantile_above_quietest(recent_, 0.75);
    out.worst_twentieth_ms = transit_quantile_above_quietest(recent_, 0.95);
    return out;
}

bool poll_gap_measures_the_link(uint64_t poll_gap_ms, uint64_t typical_gap_ms) {
    if (typical_gap_ms == 0) {
        return true;  // nothing to compare against yet
    }
    // Three times the quickest pass, plus a few milliseconds so that a loop
    // running very fast does not reject its own ordinary variation. A pass
    // that missed a refresh or two is still a pass; one that took six times
    // as long is this device having been somewhere else.
    return poll_gap_ms <= typical_gap_ms * 3 + 5;
}

std::size_t video_buffer_target_frames(double jitter_ms, double period_ms) {
    if (period_ms <= 0.0 || !(jitter_ms > 0.0)) {
        return kMinVideoBufferFrames;
    }
    // Whole frames, rounded up: cover for half a frame of jitter still costs a
    // whole frame to hold, since a frame is the smallest thing that can be
    // held back at all.
    const double frames = std::ceil(jitter_ms / period_ms);
    const double target = static_cast<double>(kMinVideoBufferFrames) + frames;
    if (target >= static_cast<double>(kMaxVideoBufferFrames)) {
        return kMaxVideoBufferFrames;
    }
    return static_cast<std::size_t>(target);
}

uint64_t video_pace_step_ms(uint64_t period_ms, std::size_t shown, bool draining) {
    const uint64_t step = period_ms * (shown > 0 ? shown : 1);
    if (!draining) {
        return step;
    }
    // A few percent fast, and never less than one millisecond of a step so a
    // tiny period cannot round the clock away entirely.
    constexpr uint64_t kDrainPercent = 8;
    const uint64_t drained = step * (100 - kDrainPercent) / 100;
    return drained > 0 ? drained : 1;
}

bool lateness_is_worth_covering(uint64_t period_ms, uint64_t quickest_poll_gap_ms) {
    if (quickest_poll_gap_ms == 0) {
        return true;
    }
    // Half a period is the deadline the stall test starts from, so half a
    // period is what the loop has to be able to resolve for a miss to mean
    // anything at all. Being able to resolve it is not the same as being able
    // to see it on time, though -- see lateness_that_counts_ms().
    return quickest_poll_gap_ms <= period_ms / 2;
}

bool lateness_was_the_links(double link_delay_ms, uint64_t period_ms) {
    if (link_delay_ms < 0.0) {
        return true;
    }
    return link_delay_ms >= static_cast<double>(period_ms) / 2.0;
}

uint64_t lateness_that_counts_ms(uint64_t period_ms, uint64_t quickest_poll_gap_ms) {
    // Half a period, plus however long this device takes to come round and
    // look.
    //
    // Without the second term a device counts its own sampling rate as the
    // link stalling. On a 30fps stream half a period is 16ms and an Apple TV's
    // loop comes round every 14 to 16 -- so a frame that arrived exactly on
    // time is *seen* a tick later and read as late, over and over. Measured:
    // the hold swung between 33ms and 140ms for a whole session, buying cover
    // for stalls that were never there, decaying, and buying it again. A
    // sawtooth a hundred milliseconds deep, on a link running 30 frames a
    // second with nothing dropped.
    //
    // A picture is late when it is late by more than this device could
    // possibly have noticed. Anything inside that is arithmetic about when the
    // loop happened to run, and no amount of buffer fixes when the loop
    // happened to run.
    return period_ms / 2 + quickest_poll_gap_ms;
}

double video_pace_step_exact_ms(double period_ms, std::size_t shown, bool draining) {
    const double step = period_ms * static_cast<double>(shown > 0 ? shown : 1);
    // Eight percent fast while giving delay back, as video_pace_step_ms().
    return draining ? step * 0.92 : step;
}

uint64_t video_hold_floor_ms(uint64_t period_ms) {
    return std::max<uint64_t>(2, period_ms / 8);
}

bool shed_a_frame_to_catch_up(uint64_t behind_for_ms, uint64_t since_last_ms, uint64_t period_ms,
                              video::VideoHold hold) {
    uint64_t patience_ms = 0;
    uint64_t at_most_every_ms = 0;
    switch (hold) {
        // The moment it is seen: every frame of backlog is a frame every tap
        // lands late by, and this setting is the one that says so.
        case video::VideoHold::Responsive: break;
        // Two frames, which lets a burst the pacing is about to absorb go
        // unshed, and no more than four a second.
        case video::VideoHold::Balanced:
            patience_ms = period_ms * 2;
            at_most_every_ms = 250;
            break;
        case video::VideoHold::Smooth:
            patience_ms = period_ms * 6;
            at_most_every_ms = 500;
            break;
    }
    return behind_for_ms >= patience_ms && since_last_ms >= at_most_every_ms;
}

uint64_t video_hold_measured_ceiling_ms(video::VideoHold hold, uint64_t period_ms) {
    // The most the measured spread alone may ask for: a period for
    // Responsive, two otherwise. The least is video_hold_floor_ms().
    return hold == video::VideoHold::Responsive ? period_ms : period_ms * 2;
}

uint64_t video_hold_extra_ceiling_ms(video::VideoHold hold, uint64_t period_ms) {
    switch (hold) {
        // Nothing earned. A stall buys no delay, which is the whole of what
        // this setting means.
        case video::VideoHold::Responsive: return 0;
        // Two periods above what the measurement can ask for. Past that the
        // picture is not being smoothed, it is being delayed: a link that
        // still runs dry with that much in hand is losing frames or falling
        // behind, and neither is something more waiting will fix.
        case video::VideoHold::Balanced: return period_ms * 2;
        case video::VideoHold::Smooth: break;
    }
    // Which is an argument, not a proof -- on a bad enough tail the extra
    // waiting really is what fixes it, and that is this end of the setting.
    return period_ms * 4;
}

uint64_t video_hold_allowance_ms(double jitter_ms, uint64_t period_ms, uint64_t extra_ms,
                                 video::VideoHold hold) {
    const auto measured = static_cast<uint64_t>(std::max(0.0, jitter_ms) + 0.5);
    const uint64_t ceiling = video_hold_measured_ceiling_ms(hold, period_ms);
    return std::clamp(measured, video_hold_floor_ms(period_ms), ceiling) +
           std::min(extra_ms, video_hold_extra_ceiling_ms(hold, period_ms));
}

uint64_t hold_extra_ms(uint64_t current_ms, uint64_t period_ms, unsigned starved_last_second,
                       unsigned calm_seconds, video::VideoHold hold) {
    // A quarter of a period at a time, not a whole one.
    //
    // This used to step in whole frames, and on a 30fps stream that is 33ms a
    // step -- so a link needing 43ms of cover was given 66ms and the picture
    // waited the difference for nothing. Measured: 99ms of cover held against
    // a spread whose worst one-in-twenty was 76ms, and every frame paying the
    // 23ms of rounding. The cover has no reason to come in frame-sized pieces;
    // only the pacing clock does.
    const uint64_t step = std::max<uint64_t>(period_ms / 4, 1);
    const uint64_t ceiling = video_hold_extra_ceiling_ms(hold, period_ms);
    // Bought on the first second that stalls, because one stall a second is
    // already what a player notices -- this is not a threshold to be tuned up
    // until the complaints stop.
    if (starved_last_second > 0) {
        return std::min(current_ms + step, ceiling);
    }
    // Given back slowly, and only after long enough that a quiet spell is a
    // quiet link rather than a quiet moment. Asymmetric on purpose: buying
    // cover costs delay once, and giving it back too eagerly costs a stall
    // every time the link twitches.
    //
    // Three seconds, not ten. Ten is not a wait, it is a streak, and a streak
    // is not something a real link supplies: measured on an Apple TV, a rough
    // patch bought 73ms of cover, the stream went back to 30 frames a second
    // with nothing dropped and a 35ms round trip -- and the cover sat at 73ms
    // for the rest of the session, because a single bad second every dozen or
    // so put the count back to zero before it ever reached ten. Cover that can
    // only be bought is not cover, it is a ratchet.
    //
    // Three is a judgement rather than a measurement: long enough that one
    // quiet second is not mistaken for a quiet link, short enough to be
    // reached on a link that twitches now and then, which is every real one.
    // The rate is unchanged, so giving back 73ms still takes nine seconds of
    // calm -- it is the streak that was wrong, not the pace.
    constexpr unsigned kCalmSecondsBeforeGivingBack = 3;
    if (calm_seconds >= kCalmSecondsBeforeGivingBack && current_ms > 0) {
        return current_ms - std::min(current_ms, step);
    }
    return current_ms;
}

std::size_t CompanionSession::target_buffer_frames() const {
    return video_buffer_target_frames(jitter_.estimate(), static_cast<double>(frame_period()));
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
        // Nor keep hold of a decoder's picture this screen has stopped showing.
        target.native.reset();
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
        // Frames come back as planes wherever the decoder produces them: the
        // conversion to colour then happens in the fragment shader, which is
        // a few multiplies per fragment instead of a pass over every pixel on
        // the CPU. On a Switch at 1280x720 that pass was most of the 22ms a
        // frame that kept it from ever reaching 60. See DecodedLayout.
        decoder.set_planar_output_wanted(true);
        if (!decoder.open(config, &error)) {
            fail("the host sent video this device cannot decode: " + error);
            return false;
        }
    }
    video::DecodedFrame decoded;
    std::string error;
    if (!decoder.decode(frame.payload.data(), frame.payload.size(), decoded, &error)) {
        // Not the end of the session. The overwhelmingly likely cause is a
        // frame this end never received whole -- the decoder is being handed
        // a stream with a hole in it, and what it needs is a picture that
        // stands on its own, which is exactly what asking gets. Ending the
        // session over it would turn every dropped datagram on a busy
        // wireless link into a disconnection.
        //
        // A reopen rather than just a request, because a decoder that has
        // refused a packet does not reliably pick up again from the next
        // keyframe on its own; the keyframe carries its own parameter sets
        // (nothing here asks for out-of-band ones), so a fresh decoder has
        // everything it needs the moment one arrives.
        decoder.close();
        unsigned& failures =
            (frame.screen == ScreenId::Top) ? decode_failures_top_ : decode_failures_bottom_;
        if (++failures >= kMaxConsecutiveDecodeFailures) {
            fail("a frame from the host could not be decoded: " + error);
            return false;
        }
        bool& wanted =
            (frame.screen == ScreenId::Top) ? keyframe_wanted_top_ : keyframe_wanted_bottom_;
        wanted = true;
        return false;
    }
    if (!decoded.valid()) {
        // Nothing out yet. A stream that has not reached its first keyframe has
        // nothing to show, which is a wait rather than a failure.
        return false;
    }
    // A picture came out, so whatever went wrong before it is behind us --
    // see the run counted above, which only means anything unbroken.
    ((frame.screen == ScreenId::Top) ? decode_failures_top_ : decode_failures_bottom_) = 0;
    adapt_readback_micros_ += static_cast<uint64_t>(decoded.readback_micros);
    adapt_lock_micros_ += static_cast<uint64_t>(decoded.lock_micros);
    adapt_last_width_ = decoded.width;
    adapt_last_height_ = decoded.height;
    adapt_arrived_scale_ =
        std::max(adapt_arrived_scale_,
                 scale_the_picture_arrived_at(decoded.width, decoded.height,
                                              wanted_display_width_, wanted_display_height_));
    // The first picture of the size a step asked for: the window starts
    // again once this one is counted -- see adapt_awaiting_from_width_.
    if (adapt_awaiting_from_width_ > 0 &&
        picture_size_changed_enough(adapt_awaiting_from_width_, decoded.width)) {
        adapt_awaiting_from_width_ = 0;
        adapt_restart_window_ = true;
    }
    ++adapt_frames_;
    target.video = true;
    target.layout = decoded.layout;
    target.pixels = std::move(decoded.pixels);
    for (int index = 0; index < 3; ++index) {
        target.plane[index] = std::move(decoded.plane[index]);
        target.plane_stride[index] = decoded.stride[index];
    }
    target.native = std::move(decoded.native);
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
    const uint64_t started_ms = monotonic_millis();
    while (!complete_at_.empty() && complete_at_.front().serial < serial) {
        complete_at_.pop_front();  // never shown; nothing to measure
    }
    shown_link_delay_ms_ = -1.0;
    if (!complete_at_.empty() && complete_at_.front().serial == serial) {
        const uint64_t waited =
            started_ms > complete_at_.front().at_ms ? started_ms - complete_at_.front().at_ms : 0;
        shown_link_delay_ms_ = complete_at_.front().link_delay_ms;
        stats_wait_total_ms_ += waited;
        stats_wait_worst_ms_ = std::max(stats_wait_worst_ms_, waited);
        ++stats_waits_;
        complete_at_.pop_front();
    }
    while (!pending_frames_.empty() && pending_frames_.front().serial == serial) {
        apply_frame(pending_frames_.front());
        pending_frames_.pop_front();
    }
    const uint64_t finished_ms = monotonic_millis();
    const uint64_t cost = finished_ms > started_ms ? finished_ms - started_ms : 0;
    stats_decode_total_ms_ += cost;
    // The same measurement, on its own clock, for the decision in
    // adapt_to_what_this_device_can_decode() -- which must not have its
    // numbers cleared out from under it by the once-a-second stream report.
    adapt_decode_ms_ += cost;
    ++stats_decodes_;
    if (adapt_restart_window_) {
        adapt_restart_window_ = false;
        adapt_at_ms_ = finished_ms;
        adapt_serial_at_start_ = received_serial_;
        adapt_arrivals_ = 0;
        adapt_decode_ms_ = 0;
        adapt_readback_micros_ = 0;
        adapt_lock_micros_ = 0;
        adapt_frames_ = 0;
        // What this picture filled is still true of the stream, and is what
        // the next step would go from.
        adapt_arrived_scale_ = scale_the_picture_arrived_at(
            adapt_last_width_, adapt_last_height_, wanted_display_width_, wanted_display_height_);
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
