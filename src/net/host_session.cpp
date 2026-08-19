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

void HostSession::set_layout(const std::string& layout_id, bool paused) {
    if (layout_id != layout_id_ || paused != paused_) {
        layout_id_ = layout_id;
        paused_ = paused;
        session_state_dirty_ = true;
    }
}

void HostSession::refresh_beacon() {
    BeaconMessage beacon;
    beacon.device_name = config_.device_name;
    beacon.game_title = config_.game_title;
    beacon.rom_sha1 = config_.rom_sha1;
    beacon.session_port = listener_.port() != 0 ? listener_.port() : kSessionPort;
    // Only the screen the host is not using is on offer, and only while nobody
    // has taken it.
    const bool free_slot = !companion_.open();
    beacon.top_available = free_slot && companion_screen_ == ScreenId::Top;
    beacon.bottom_available = free_slot && companion_screen_ == ScreenId::Bottom;
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
    refresh_beacon();
}

bool HostSession::companion_wants(ScreenId screen) const {
    if (!has_companion()) {
        return false;
    }
    // Under the split layout the companion only needs its own screen. Under any
    // other layout it draws the whole thing, so it needs both.
    if (layout_id_ == "split_devices") {
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

            // The companion asks for a screen; the host grants the one it is not
            // using itself, which is the only arrangement that makes sense.
            companion_name_ = hello.device_name.empty() ? "companion" : hello.device_name;
            handshake_done_ = true;

            WelcomeMessage welcome;
            welcome.game_title = config_.game_title;
            welcome.rom_sha1 = config_.rom_sha1;
            welcome.granted_screen = companion_screen_;
            welcome.screen_width = kDsScreenWidth;
            welcome.screen_height = kDsScreenHeight;
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
        state.layout_id = layout_id_;
        state.companion_screen = companion_screen_;
        state.paused = paused_;
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
