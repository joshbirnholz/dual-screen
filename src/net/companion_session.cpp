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

    std::string error;
    if (!stream_.start_connect(host.address, host.session_port, &error)) {
        fail(error);
        return;
    }
    status_ = Status::Connecting;
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
    inbox_.clear();
    said_hello_ = false;
    if (status_ == Status::Connected || status_ == Status::Connecting) {
        status_ = Status::Searching;
        status_detail_.clear();
    }
}

void CompanionSession::fail(const std::string& reason) {
    stream_.close();
    inbox_.clear();
    said_hello_ = false;
    status_ = Status::Failed;
    status_detail_ = reason;
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
            status_ = Status::Connected;
            status_detail_ = "showing the " + std::string(to_string(granted_screen_)) +
                             " screen of " + game_title_;
            log_info("connected: " + status_detail_);
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
            host_render_scale_ = state.render_scale;
            granted_screen_ = state.companion_screen;
            break;
        }
        case MessageType::Frame: {
            FrameMessage frame;
            if (!decode_frame(payload.data(), payload.size(), frame)) {
                fail("the host sent a frame we could not read");
                return;
            }
            // Bounded before allocating: these dimensions came off a socket,
            // and decode_screen_pixels() sizes its buffers from them alone,
            // before it has even looked at the payload. 4096 is not tied to
            // any one console -- it only has to comfortably clear whatever a
            // real screen, read back at whatever resolution the recipient
            // asked for, could actually come out to (a 3DS top screen at its
            // maximum render scale is 1200 wide), with headroom for larger
            // consoles this app plays later rather than needing to be raised
            // again the next time one arrives.
            constexpr uint16_t kMaxFrameDimension = 4096;
            if (frame.width == 0 || frame.height == 0 || frame.width > kMaxFrameDimension ||
                frame.height > kMaxFrameDimension) {
                fail("the host sent a frame with impossible dimensions");
                return;
            }
            ScreenBuffer& target = (frame.screen == ScreenId::Top) ? top_ : bottom_;
            if (!decode_screen_pixels(frame.payload, frame.width, frame.height, target.pixels)) {
                fail("a frame from the host could not be decompressed");
                return;
            }
            target.width = frame.width;
            target.height = frame.height;
            target.serial = frame.serial;
            break;
        }
        case MessageType::Audio: {
            AudioMessage audio;
            if (!decode_audio(payload.data(), payload.size(), audio)) {
                return;  // a bad audio packet is not worth ending a session over
            }
            // Only the shared stream means anything for DS and 3DS. Per-device
            // streams arrive with Wii U and are ignored until then rather than
            // played on the wrong device.
            if (audio.stream != AudioStreamId::Shared || audio.samples.empty()) {
                return;
            }
            if (audio_callback_) {
                audio_callback_(audio.samples.data(), audio.samples.size() / 2,
                                audio.sample_rate != 0 ? audio.sample_rate : audio_sample_rate_);
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

void CompanionSession::poll(uint64_t now_ms) {
    browser_.poll(now_ms);

    if (!stream_.open()) {
        return;
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
    std::vector<uint8_t> out;
    encode_input(message, out);
    stream_.send(out);

    last_sent_input_ = state;
    ever_sent_input_ = true;
}

bool CompanionSession::has_screen(ScreenId screen) const {
    const ScreenBuffer& buffer = (screen == ScreenId::Top) ? top_ : bottom_;
    return buffer.width > 0 && !buffer.pixels.empty();
}

uint64_t CompanionSession::received_serial(ScreenId screen) const {
    return (screen == ScreenId::Top) ? top_.serial : bottom_.serial;
}

uint32_t CompanionSession::sample_pixel(ScreenId screen) const {
    const ScreenBuffer& buffer = (screen == ScreenId::Top) ? top_ : bottom_;
    return buffer.pixels.empty() ? 0u : (buffer.pixels[0] & 0x00ffffffu);
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
        if (buffer->serial != buffer->uploaded_serial && !buffer->pixels.empty()) {
            buffer->texture.upload_xrgb8888(buffer->pixels.data(), buffer->width, buffer->height,
                                            buffer->width);
            buffer->uploaded_serial = buffer->serial;
        }
    }
    textures_.top = top_.texture.valid() ? &top_.texture : nullptr;
    textures_.bottom = bottom_.texture.valid() ? &bottom_.texture : nullptr;
    return textures_;
}

SystemScreens CompanionSession::screens_geometry() const {
    // The host says which console it is running and both devices run the same
    // build, so the sizes are looked up rather than measured from whatever
    // frame happened to arrive first.
    return system_profile(system_).screens;
}

}  // namespace ds::net
