#include "net/host_session.hpp"

#include <algorithm>

#include "util/log.hpp"

namespace ds::net {
namespace {

// If the companion cannot keep up, dropping frames is the right failure: the
// alternative is an ever-growing queue and input that lags further behind.
constexpr std::size_t kMaxQueuedBytes = 512 * 1024;

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
    if (state.layout_id != shared_.layout_id ||
        state.per_device_layout != shared_.per_device_layout || state.paused != shared_.paused ||
        state.bilinear_filtering != shared_.bilinear_filtering ||
        state.audio_target != shared_.audio_target || state.handedness != shared_.handedness) {
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
    // Nobody is holding a menu open on a device that has gone.
    companion_menu_open_ = false;
    refresh_beacon();
}

bool HostSession::companion_wants(ScreenId screen) const {
    if (!has_companion()) {
        return false;
    }
    // Under a per-device layout the companion only needs its own screen. Under
    // any other it draws the whole thing, so it needs both.
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
            handshake_done_ = true;

            WelcomeMessage welcome;
            welcome.game_title = config_.game_title;
            welcome.rom_sha1 = config_.rom_sha1;
            welcome.granted_screen = companion_screen_;
            welcome.screen_width = kDsScreenWidth;
            welcome.screen_height = kDsScreenHeight;
            welcome.audio_sample_rate = config_.audio_sample_rate;
            std::vector<uint8_t> out;
            encode_welcome(welcome, out);
            companion_.send(out);
            session_state_dirty_ = true;
            last_notice_ = companion_name_ + " connected";
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
            // Clamped rather than trusted: these arrive from another machine.
            companion_input_.touch_x =
                std::min(std::max<int>(input.touch_x, 0), kDsScreenWidth - 1);
            companion_input_.touch_y =
                std::min(std::max<int>(input.touch_y, 0), kDsScreenHeight - 1);
            companion_input_fresh_ = true;
            break;
        }
        case MessageType::Request: {
            RequestMessage request;
            if (!decode_request(payload.data(), payload.size(), request)) {
                return;  // not worth dropping a session over
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
            if (have_request_ && !request.wants_handedness && request_.wants_handedness) {
                request.wants_handedness = true;
                request.handedness = request_.handedness;
            }
            request_ = request;
            have_request_ = true;
            if (request.menu_open != companion_menu_open_) {
                companion_menu_open_ = request.menu_open;
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

    if (!companion_.open()) {
        TcpStream incoming;
        if (listener_.accept(incoming)) {
            companion_ = std::move(incoming);
            inbox_.clear();
            handshake_done_ = false;
            log_info("a device is connecting from " + companion_.peer_address());
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
        std::vector<uint8_t> out;
        encode_session_state(state, out);
        companion_.send(out);
        session_state_dirty_ = false;
    }

    std::string error;
    if (!companion_.pump(inbox_, &error)) {
        drop_companion(error);
        return;
    }

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

void HostSession::publish_audio(const int16_t* interleaved_stereo,
                                std::size_t frames,
                                uint32_t sample_rate) {
    if (!audio_enabled_ || !has_companion() || interleaved_stereo == nullptr || frames == 0) {
        return;
    }
    // Audio is dropped rather than queued when the link is behind, for the same
    // reason as video: a growing queue is heard as drift, not as buffering.
    if (companion_.pending_bytes() > kMaxQueuedBytes) {
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

void HostSession::publish_screens(const uint32_t* top_pixels,
                                  const uint32_t* bottom_pixels,
                                  int width,
                                  int height,
                                  int stride_pixels,
                                  uint64_t serial) {
    if (!has_companion() || serial == sent_serial_) {
        return;
    }
    // Never let the send queue grow without bound: a companion that cannot keep
    // up should see dropped frames, not mounting latency.
    if (companion_.pending_bytes() > kMaxQueuedBytes) {
        return;
    }

    std::vector<uint8_t> out;
    const struct {
        ScreenId screen;
        const uint32_t* pixels;
    } screens[] = {{ScreenId::Top, top_pixels}, {ScreenId::Bottom, bottom_pixels}};

    for (const auto& entry : screens) {
        if (entry.pixels == nullptr || !companion_wants(entry.screen)) {
            continue;
        }
        FrameMessage frame;
        frame.screen = entry.screen;
        frame.serial = serial;
        frame.width = static_cast<uint16_t>(width);
        frame.height = static_cast<uint16_t>(height);
        if (!encode_screen_pixels(entry.pixels, width, height, stride_pixels, frame.payload)) {
            continue;
        }
        encode_frame(frame, out);
    }

    if (!out.empty()) {
        companion_.send(out);
    }
    sent_serial_ = serial;
}

}  // namespace ds::net
