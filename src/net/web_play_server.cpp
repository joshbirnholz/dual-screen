#include "net/web_play_server.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>

#include <mbedtls/sha1.h>
#include <nlohmann/json.hpp>

#include "net/launch_service.hpp"
#include "net/socket_api.hpp"
#include "util/log.hpp"

#if DS_WEB_PLAY
namespace ds::assets {
extern const unsigned char kWebPlayPage[];
extern const unsigned long kWebPlayPage_size;
}  // namespace ds::assets
#endif

namespace ds::net {

namespace {

// How far behind a browser may fall, in bytes not yet sent, before its video
// is dropped until the next keyframe: a few frames of 720p, and well under a
// second on any link that can carry the stream at all. Sound keeps going a
// while longer, since it is small and a gap in it is worse than a frozen frame.
constexpr std::size_t kVideoBacklogLimit = 768 * 1024;
constexpr std::size_t kAudioBacklogLimit = 2 * 1024 * 1024;
// A browser that has said nothing for this long is gone: the page sends its
// pads at least once a second, even with none.
constexpr uint64_t kSilentForMs = 15000;
// Longest name kept.
constexpr std::size_t kNameLimit = 32;

uint64_t now_ms() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}

std::string lowercase(std::string text) {
    for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

std::string trim(const std::string& text) {
    std::size_t begin = 0, end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
    return text.substr(begin, end - begin);
}

// Cut at a character boundary, not through one.
std::string clip_utf8(const std::string& text, std::size_t limit) {
    if (text.size() <= limit) return text;
    std::size_t end = limit;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) --end;
    return text.substr(0, end);
}

void put_u16(std::vector<uint8_t>& out, uint16_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xFF));
    out.push_back(static_cast<uint8_t>(value >> 8));
}

void put_u32(std::vector<uint8_t>& out, uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) out.push_back(static_cast<uint8_t>((value >> shift) & 0xFF));
}

const char* screen_word(ScreenId screen) { return screen == ScreenId::Top ? "tv" : "gamepad"; }

const char* view_word(WebView view) {
    switch (view) {
        case WebView::Tv: return "tv";
        case WebView::Both: return "both";
        case WebView::Own: break;
    }
    return "gamepad";
}

std::string http_response(const std::string& status, const std::string& type, const std::string& body,
                          const std::string& extra = "") {
    return "HTTP/1.1 " + status + "\r\nContent-Type: " + type + "\r\nContent-Length: " +
           std::to_string(body.size()) + "\r\nCache-Control: no-store\r\nConnection: close\r\n" + extra + "\r\n" +
           body;
}

}  // namespace

std::string websocket_accept_key(const std::string& client_key) {
    const std::string joined = client_key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::vector<uint8_t> digest(20);
    mbedtls_sha1(reinterpret_cast<const unsigned char*>(joined.data()), joined.size(), digest.data());
    return base64_encode(digest);
}

std::vector<uint8_t> websocket_frame(uint8_t opcode, const uint8_t* data, std::size_t size) {
    std::vector<uint8_t> frame;
    frame.reserve(size + 10);
    frame.push_back(static_cast<uint8_t>(0x80 | (opcode & 0x0F)));
    if (size < 126) {
        frame.push_back(static_cast<uint8_t>(size));
    } else if (size <= 0xFFFF) {
        frame.push_back(126);
        frame.push_back(static_cast<uint8_t>(size >> 8));
        frame.push_back(static_cast<uint8_t>(size & 0xFF));
    } else {
        frame.push_back(127);
        for (int shift = 56; shift >= 0; shift -= 8) {
            frame.push_back(static_cast<uint8_t>((static_cast<uint64_t>(size) >> shift) & 0xFF));
        }
    }
    if (size != 0) frame.insert(frame.end(), data, data + size);
    return frame;
}

bool WebSocketReader::feed(const uint8_t* data, std::size_t size, std::vector<Message>& out) {
    buffer_.insert(buffer_.end(), data, data + size);
    std::size_t at = 0;
    for (;;) {
        if (buffer_.size() - at < 2) break;
        const uint8_t first = buffer_[at];
        const uint8_t second = buffer_[at + 1];
        if ((first & 0x70) != 0) return false;  // no extensions were agreed
        const bool fin = (first & 0x80) != 0;
        const uint8_t opcode = first & 0x0F;
        const bool masked = (second & 0x80) != 0;
        uint64_t length = second & 0x7F;
        std::size_t header = 2;
        if (length == 126) {
            if (buffer_.size() - at < 4) break;
            length = (static_cast<uint64_t>(buffer_[at + 2]) << 8) | buffer_[at + 3];
            header = 4;
        } else if (length == 127) {
            if (buffer_.size() - at < 10) break;
            length = 0;
            for (int i = 0; i < 8; ++i) length = (length << 8) | buffer_[at + 2 + i];
            header = 10;
        }
        // Nothing a page sends is anywhere near this.
        if (length > (16u << 20)) return false;
        const std::size_t mask_at = at + header;
        if (masked) header += 4;
        if (buffer_.size() - at < header + length) break;
        const uint8_t* payload = &buffer_[at + header];
        std::vector<uint8_t> bytes(payload, payload + length);
        if (masked) {
            for (std::size_t i = 0; i < bytes.size(); ++i) bytes[i] ^= buffer_[mask_at + (i & 3)];
        }
        at += header + static_cast<std::size_t>(length);

        if (opcode >= 8) {
            // Control frames come whole, even in the middle of a fragmented one.
            out.push_back(Message{opcode, std::move(bytes)});
            continue;
        }
        if (opcode != 0) {
            fragment_opcode_ = opcode;
            fragments_ = std::move(bytes);
        } else {
            fragments_.insert(fragments_.end(), bytes.begin(), bytes.end());
        }
        if (fin && fragment_opcode_ != 0) {
            out.push_back(Message{fragment_opcode_, std::move(fragments_)});
            fragments_.clear();
            fragment_opcode_ = 0;
        }
    }
    buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(at));
    return true;
}

struct WebPlayServer::Viewer {
    uint32_t id = 0;
    SecureStream stream;
    std::string address;
    std::vector<uint8_t> inbox;
    bool websocket = false;
    WebSocketReader reader;
    bool joined = false;
    std::string name;
    ScreenId screen = ScreenId::Top;
    bool touching = false;
    uint16_t touch_x = 0;
    uint16_t touch_y = 0;
    DsMotionState motion;
    uint64_t motion_ms = 0;
    // Holding the GamePad, what they asked to see of it: its screen, the
    // TV's, or both.
    WebView view = WebView::Own;
    bool waiting_keyframe[2] = {true, true};
    // The quick menu's picture is still owed to them: it came while they
    // were too far behind, or before they joined.
    bool menu_owed = false;
    std::map<uint8_t, WebPad> pads;
    // Whether they are sent this screen's sound: the TV's always, and the
    // GamePad's too while it is theirs, whatever they are looking at -- the
    // page mutes either.
    bool hears(ScreenId which) const { return which == ScreenId::Top || screen == ScreenId::Bottom; }
    // Whether they are sent this screen's picture.
    bool watches(ScreenId which) const {
        if (screen == ScreenId::Top) return which == ScreenId::Top;
        switch (view) {
            case WebView::Own: return which == ScreenId::Bottom;
            case WebView::Tv: return which == ScreenId::Top;
            case WebView::Both: return true;
        }
        return false;
    }
    uint64_t heard_ms = 0;
    bool finished = false;
};

WebPlayServer::WebPlayServer() = default;
WebPlayServer::~WebPlayServer() { stop(); }

bool WebPlayServer::start(const Config& config, std::string* error) {
    stop();
    config_ = config;
    tls_on_ = !config.identity.empty();
    if (tls_on_ && !tls_.start_server(config.identity, error)) return false;
    if (!listener_.start(config.port, error)) return false;
    return true;
}

void WebPlayServer::stop() {
    const bool had = !viewers_.empty();
    viewers_.clear();
    listener_.stop();
    if (had) ++roster_serial_;
}

std::vector<int> WebPlayServer::descriptors() const {
    std::vector<int> out;
    // The listener's own isn't reachable through TcpListener; a caller sleeps
    // a few milliseconds at most, which covers it.
    for (const auto& viewer : viewers_) {
        if (viewer->stream.open()) out.push_back(viewer->stream.descriptor());
    }
    return out;
}

bool WebPlayServer::wants_to_write() const {
    for (const auto& viewer : viewers_) {
        if (viewer->stream.pending_bytes() != 0 || !viewer->stream.secure_ready()) return true;
    }
    return false;
}

void WebPlayServer::pump() {
    if (!listener_.listening()) return;
    for (int accepted = 0; accepted < 8; ++accepted) {
        TcpStream incoming;
        if (!listener_.accept(incoming)) break;
        auto viewer = std::make_unique<Viewer>();
        viewer->id = next_viewer_id_++;
        if (next_viewer_id_ > 0xFFFF) next_viewer_id_ = 1;
        viewer->address = incoming.peer_address();
        viewer->heard_ms = now_ms();
        std::string error;
        if (!viewer->stream.start(tls_on_ ? &tls_ : nullptr, incoming.release(), &error)) {
            log_warn("Web Play: " + error);
            continue;
        }
        viewers_.push_back(std::move(viewer));
    }
    for (auto& viewer : viewers_) {
        pump_viewer(*viewer);
        if (viewer->menu_owed && viewer->joined && !viewer->finished) send_menu_to(*viewer);
        // Read the clock after pumping: what was just heard is stamped now,
        // and a clock read before it would make that a wrap-around, not 0.
        const uint64_t now = now_ms();
        if (!viewer->finished && now > viewer->heard_ms && now - viewer->heard_ms > kSilentForMs) {
            if (viewer->joined) log_info("Web Play: " + viewer->name + " stopped answering");
            viewer->finished = true;
        }
    }
    for (const auto& viewer : viewers_) {
        if (viewer->finished && viewer->joined) {
            log_info("Web Play: " + viewer->name + " left");
            ++roster_serial_;
        }
    }
    viewers_.erase(std::remove_if(viewers_.begin(), viewers_.end(),
                                  [](const std::unique_ptr<Viewer>& viewer) { return viewer->finished; }),
                   viewers_.end());
}

void WebPlayServer::pump_viewer(Viewer& viewer) {
    if (viewer.finished) return;
    const std::size_t before = viewer.inbox.size();
    std::string error;
    if (!viewer.stream.pump(viewer.inbox, &error)) {
        if (viewer.joined && error != "closed" && error != "finished") {
            log_info("Web Play: lost " + viewer.name + ": " + error);
        }
        viewer.finished = true;
        return;
    }
    if (viewer.inbox.size() != before) viewer.heard_ms = now_ms();
    if (!viewer.websocket) {
        handle_http(viewer);
        return;
    }
    if (viewer.inbox.empty()) return;
    std::vector<WebSocketReader::Message> messages;
    const bool ok = viewer.reader.feed(viewer.inbox.data(), viewer.inbox.size(), messages);
    viewer.inbox.clear();
    if (!ok) {
        log_info("Web Play: " + viewer.name + " sent something that wasn't WebSocket");
        viewer.finished = true;
        return;
    }
    for (const auto& message : messages) {
        handle_message(viewer, message);
        if (viewer.finished) return;
    }
}

void WebPlayServer::handle_http(Viewer& viewer) {
    if (viewer.stream.closing()) {
        viewer.inbox.clear();
        return;
    }
    static const char kEnd[] = "\r\n\r\n";
    const auto end = std::search(viewer.inbox.begin(), viewer.inbox.end(), kEnd, kEnd + 4);
    if (end == viewer.inbox.end()) {
        if (viewer.inbox.size() > 16384) viewer.finished = true;
        return;
    }
    const std::string head(viewer.inbox.begin(), end);
    viewer.inbox.clear();

    std::vector<std::string> lines;
    std::size_t at = 0;
    while (at <= head.size()) {
        const std::size_t next = head.find("\r\n", at);
        lines.push_back(head.substr(at, next == std::string::npos ? std::string::npos : next - at));
        if (next == std::string::npos) break;
        at = next + 2;
    }
    std::string method, target;
    {
        const std::string& request = lines.front();
        const std::size_t space = request.find(' ');
        const std::size_t second = space == std::string::npos ? std::string::npos : request.find(' ', space + 1);
        if (space == std::string::npos || second == std::string::npos) {
            viewer.finished = true;
            return;
        }
        method = request.substr(0, space);
        target = request.substr(space + 1, second - space - 1);
    }
    std::map<std::string, std::string> headers;
    for (std::size_t i = 1; i < lines.size(); ++i) {
        const std::size_t colon = lines[i].find(':');
        if (colon == std::string::npos) continue;
        headers[lowercase(trim(lines[i].substr(0, colon)))] = trim(lines[i].substr(colon + 1));
    }
    const std::size_t query = target.find('?');
    const std::string path = query == std::string::npos ? target : target.substr(0, query);

    if (viewer.stream.looks_like_plain_http()) {
        // Typed http:// at a port that speaks only https://: sent where it
        // meant to go.
        const std::string host = headers.count("host") ? headers["host"] : std::string();
        if (host.empty()) {
            viewer.stream.send(http_response("400 Bad Request", "text/plain", "Use https://\n"));
        } else {
            viewer.stream.send(http_response("301 Moved Permanently", "text/plain", "",
                                             "Location: https://" + host + target + "\r\n"));
        }
        viewer.stream.close_after_sending();
        return;
    }

    if (method != "GET") {
        viewer.stream.send(http_response("405 Method Not Allowed", "text/plain", "GET only\n"));
        viewer.stream.close_after_sending();
        return;
    }
    if (path == "/ws") {
        const std::string key = headers.count("sec-websocket-key") ? headers["sec-websocket-key"] : std::string();
        if (key.empty() || lowercase(headers["upgrade"]) != "websocket") {
            viewer.stream.send(http_response("400 Bad Request", "text/plain", "WebSocket only\n"));
            viewer.stream.close_after_sending();
            return;
        }
        viewer.stream.send("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                           "Sec-WebSocket-Accept: " +
                           websocket_accept_key(key) + "\r\n\r\n");
        viewer.websocket = true;
        nlohmann::json info = {{"type", "info"}, {"device", config_.device_name}, {"game", config_.game_title}};
        const std::string text = info.dump();
        send_frame(viewer, 1, reinterpret_cast<const uint8_t*>(text.data()), text.size());
        return;
    }
    if (path == "/" || path == "/index.html") {
        std::string page = config_.page;
#if DS_WEB_PLAY
        if (page.empty()) {
            page.assign(reinterpret_cast<const char*>(assets::kWebPlayPage), assets::kWebPlayPage_size);
        }
#endif
        viewer.stream.send(http_response("200 OK", "text/html; charset=utf-8", page));
        viewer.stream.close_after_sending();
        return;
    }
    viewer.stream.send(http_response("404 Not Found", "text/plain", "Not found\n"));
    viewer.stream.close_after_sending();
}

void WebPlayServer::handle_message(Viewer& viewer, const WebSocketReader::Message& message) {
    switch (message.opcode) {
        case 8:
            send_frame(viewer, 8, nullptr, 0);
            viewer.stream.close_after_sending();
            return;
        case 9:
            send_frame(viewer, 10, message.data.data(), message.data.size());
            return;
        case 10:
            return;
        case 1: {
            const nlohmann::json json =
                nlohmann::json::parse(message.data.begin(), message.data.end(), nullptr, false);
            if (!json.is_object()) return;
            const std::string type = json.value("type", std::string());
            if (type == "hello") {
                std::string name = clip_utf8(trim(json.value("name", std::string())), kNameLimit);
                if (name.empty()) name = "Guest";
                const bool first = !viewer.joined;
                viewer.name = name;
                viewer.joined = true;
                want_keyframes(viewer);
                viewer.menu_owed = menu_open_;
                ++roster_serial_;
                if (first) log_info("Web Play: " + name + " joined from " + viewer.address);
                nlohmann::json welcome = {{"type", "welcome"},
                                          {"id", viewer.id},
                                          {"screen", screen_word(viewer.screen)},
                                          {"view", view_word(viewer.view)},
                                          {"menu", menu_open_}};
                send_text(viewer.id, welcome.dump());
            } else if (type == "keyframe") {
                // A decoder of its lost its place: the one named, or all.
                const std::string which = json.value("screen", std::string());
                for (const ScreenId each : {ScreenId::Top, ScreenId::Bottom}) {
                    if (!which.empty() && which != screen_word(each)) continue;
                    viewer.waiting_keyframe[each == ScreenId::Top ? 0 : 1] = true;
                    if (viewer.watches(each)) keyframe_wanted_[each == ScreenId::Top ? 0 : 1] = true;
                }
            } else if (type == "view") {
                const std::string word = json.value("view", std::string());
                const WebView view = word == "tv" ? WebView::Tv : word == "both" ? WebView::Both : WebView::Own;
                if (view != viewer.view) {
                    viewer.view = view;
                    viewer.touching = false;
                    want_keyframes(viewer);
                    ++roster_serial_;
                }
            } else if (type == "pads" && json.contains("pads") && json["pads"].is_array()) {
                std::map<uint8_t, WebPad> pads;
                for (const auto& entry : json["pads"]) {
                    if (!entry.is_object()) continue;
                    const int index = entry.value("index", -1);
                    if (index < 0 || index > 15) continue;
                    WebPad pad;
                    const auto old = viewer.pads.find(static_cast<uint8_t>(index));
                    if (old != viewer.pads.end()) pad = old->second;
                    pad.viewer = viewer.id;
                    pad.index = static_cast<uint8_t>(index);
                    pad.id = web_pad_id(viewer.id, pad.index);
                    pad.name = clip_utf8(trim(entry.value("name", std::string("Controller"))), 64);
                    pads[pad.index] = pad;
                }
                bool changed = pads.size() != viewer.pads.size();
                for (const auto& [index, pad] : pads) {
                    const auto old = viewer.pads.find(index);
                    if (old == viewer.pads.end() || old->second.name != pad.name) changed = true;
                }
                viewer.pads = std::move(pads);
                if (changed && viewer.joined) ++roster_serial_;
            }
            return;
        }
        case 2: {
            const std::vector<uint8_t>& data = message.data;
            if (data.size() >= 6 && data[0] == static_cast<uint8_t>(WebMessage::Touch)) {
                viewer.touching = data[1] != 0;
                viewer.touch_x = static_cast<uint16_t>(data[2] | (data[3] << 8));
                viewer.touch_y = static_cast<uint16_t>(data[4] | (data[5] << 8));
                return;
            }
            if (data.size() >= 26 && data[0] == static_cast<uint8_t>(WebMessage::Motion)) {
                float values[6];
                for (int i = 0; i < 6; ++i) std::memcpy(&values[i], &data[2 + i * 4], 4);  // little-endian, as every host here is
                DsMotionState reading;
                reading.present = true;
                reading.gyro_x = values[0];
                reading.gyro_y = values[1];
                reading.gyro_z = values[2];
                reading.accel_x = values[3];
                reading.accel_y = values[4];
                reading.accel_z = values[5];
                const DisplayTurn turn = data[1] == 1   ? DisplayTurn::TopToTheRight
                                         : data[1] == 2 ? DisplayTurn::TopToTheLeft
                                         : data[1] == 3 ? DisplayTurn::UpsideDown
                                                        : DisplayTurn::None;
                // A phone's frame, turned to its screen, then held like a
                // controller -- as this app's own phones send theirs.
                reading = motion_held_like_a_controller(motion_turned_to_display(reading, turn));
                viewer.motion_ms = now_ms();
                reading.sampled_us = static_cast<uint32_t>(viewer.motion_ms * 1000u);
                viewer.motion = reading;
                return;
            }
            if (data.size() < 2 || data[0] != static_cast<uint8_t>(WebMessage::Pads)) return;
            const std::size_t count = data[1];
            constexpr std::size_t kEach = 1 + 4 + 12;
            if (data.size() < 2 + count * kEach) return;
            for (std::size_t n = 0; n < count; ++n) {
                const uint8_t* at = &data[2 + n * kEach];
                const auto found = viewer.pads.find(at[0]);
                if (found == viewer.pads.end()) continue;
                RawPadState& raw = found->second.raw;
                const uint32_t bits = static_cast<uint32_t>(at[1]) | (static_cast<uint32_t>(at[2]) << 8) |
                                      (static_cast<uint32_t>(at[3]) << 16) | (static_cast<uint32_t>(at[4]) << 24);
                for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX && b < 32; ++b) raw.buttons[b] = (bits >> b) & 1u;
                for (int a = 0; a < 6 && a < SDL_CONTROLLER_AXIS_MAX; ++a) {
                    raw.axes[a] = static_cast<int16_t>(static_cast<uint16_t>(at[5 + a * 2]) |
                                                       (static_cast<uint16_t>(at[6 + a * 2]) << 8));
                }
            }
            return;
        }
        default:
            return;
    }
}

void WebPlayServer::send_frame(Viewer& viewer, uint8_t opcode, const uint8_t* data, std::size_t size) {
    viewer.stream.send(websocket_frame(opcode, data, size));
}

std::vector<WebViewer> WebPlayServer::viewers() const {
    std::vector<WebViewer> out;
    for (const auto& viewer : viewers_) {
        if (!viewer->joined || viewer->finished) continue;
        WebViewer view;
        view.id = viewer->id;
        view.name = viewer->name;
        view.address = viewer->address;
        view.screen = viewer->screen;
        view.view = viewer->view;
        // Only on the GamePad: on the TV there is nothing to touch.
        view.touching = viewer->touching && viewer->screen == ScreenId::Bottom && viewer->watches(ScreenId::Bottom);
        view.touch_x = viewer->touch_x;
        view.touch_y = viewer->touch_y;
        // Lately, or it has stopped: a phone put down in another tab says
        // nothing more, and its last reading is not how it is held now.
        const uint64_t now = now_ms();
        if (viewer->motion_ms != 0 && now >= viewer->motion_ms && now - viewer->motion_ms < 500) view.motion = viewer->motion;
        out.push_back(view);
    }
    return out;
}

std::vector<WebPad> WebPlayServer::pads() const {
    std::vector<WebPad> out;
    for (const auto& viewer : viewers_) {
        if (!viewer->joined || viewer->finished) continue;
        for (const auto& [index, pad] : viewer->pads) out.push_back(pad);
    }
    return out;
}

void WebPlayServer::set_viewer_screen(uint32_t id, ScreenId screen) {
    for (auto& viewer : viewers_) {
        if (viewer->id != id || viewer->screen == screen) continue;
        viewer->screen = screen;
        viewer->touching = false;
        want_keyframes(*viewer);
        if (viewer->websocket) {
            const std::string text = nlohmann::json{{"type", "screen"}, {"screen", screen_word(screen)}}.dump();
            send_frame(*viewer, 1, reinterpret_cast<const uint8_t*>(text.data()), text.size());
        }
        ++roster_serial_;
    }
}

bool WebPlayServer::screen_watched(ScreenId screen) const {
    for (const auto& viewer : viewers_) {
        if (viewer->joined && !viewer->finished && viewer->watches(screen)) return true;
    }
    return false;
}

bool WebPlayServer::screen_heard(ScreenId screen) const {
    for (const auto& viewer : viewers_) {
        if (viewer->joined && !viewer->finished && viewer->hears(screen)) return true;
    }
    return false;
}

bool WebPlayServer::take_keyframe_request(ScreenId screen) {
    bool& wanted = keyframe_wanted_[screen == ScreenId::Top ? 0 : 1];
    const bool was = wanted;
    wanted = false;
    return was;
}

void WebPlayServer::send_video(ScreenId screen, const uint8_t* data, std::size_t size, bool keyframe, int width,
                               int height) {
    std::vector<uint8_t> payload;
    for (auto& viewer : viewers_) {
        if (!viewer->joined || viewer->finished || !viewer->watches(screen)) continue;
        bool& waiting = viewer->waiting_keyframe[screen == ScreenId::Top ? 0 : 1];
        if (waiting && !keyframe) continue;
        if (viewer->stream.pending_bytes() > kVideoBacklogLimit) {
            waiting = true;
            keyframe_wanted_[screen == ScreenId::Top ? 0 : 1] = true;
            continue;
        }
        if (payload.empty()) {
            payload.reserve(size + 7);
            payload.push_back(static_cast<uint8_t>(WebMessage::Video));
            payload.push_back(static_cast<uint8_t>(screen == ScreenId::Top ? 0 : 1));
            payload.push_back(keyframe ? 1 : 0);
            put_u16(payload, static_cast<uint16_t>(width));
            put_u16(payload, static_cast<uint16_t>(height));
            payload.insert(payload.end(), data, data + size);
        }
        waiting = false;
        send_frame(*viewer, 2, payload.data(), payload.size());
    }
}

void WebPlayServer::send_audio(ScreenId screen, const int16_t* samples, std::size_t frames, uint32_t rate,
                               uint8_t channels) {
    std::vector<uint8_t> payload;
    for (auto& viewer : viewers_) {
        if (!viewer->joined || viewer->finished || !viewer->hears(screen)) continue;
        if (viewer->stream.pending_bytes() > kAudioBacklogLimit) continue;
        if (payload.empty()) {
            const std::size_t count = frames * channels;
            payload.reserve(7 + count * 2);
            payload.push_back(static_cast<uint8_t>(WebMessage::Audio));
            payload.push_back(static_cast<uint8_t>(screen == ScreenId::Top ? 0 : 1));
            put_u32(payload, rate);
            payload.push_back(channels);
            for (std::size_t i = 0; i < count; ++i) put_u16(payload, static_cast<uint16_t>(samples[i]));
        }
        send_frame(*viewer, 2, payload.data(), payload.size());
    }
}

void WebPlayServer::send_text(uint32_t id, const std::string& json) {
    for (auto& viewer : viewers_) {
        if (!viewer->websocket || viewer->finished) continue;
        if (id != 0 && viewer->id != id) continue;
        send_frame(*viewer, 1, reinterpret_cast<const uint8_t*>(json.data()), json.size());
    }
}

void WebPlayServer::send_rumble(uint32_t pad_id, uint16_t low, uint16_t high, uint32_t duration_ms) {
    for (auto& viewer : viewers_) {
        for (const auto& [index, pad] : viewer->pads) {
            if (pad.id != pad_id) continue;
            const nlohmann::json json = {{"type", "rumble"},
                                         {"pad", index},
                                         {"low", low / 65535.0},
                                         {"high", high / 65535.0},
                                         {"ms", duration_ms}};
            send_text(viewer->id, json.dump());
            return;
        }
    }
}

void WebPlayServer::want_keyframes(Viewer& viewer) {
    for (const ScreenId each : {ScreenId::Top, ScreenId::Bottom}) {
        const int index = each == ScreenId::Top ? 0 : 1;
        viewer.waiting_keyframe[index] = true;
        if (viewer.watches(each)) keyframe_wanted_[index] = true;
    }
}

void WebPlayServer::send_menu(bool open, std::vector<uint8_t> image) {
    if (!open) image.clear();
    const bool was_open = menu_open_;
    menu_open_ = open;
    if (!image.empty()) menu_image_ = std::move(image);
    if (!open) menu_image_.clear();
    for (auto& viewer : viewers_) {
        if (!viewer->joined || viewer->finished) continue;
        if (!open) {
            viewer->menu_owed = false;
            if (was_open) send_menu_to(*viewer);
            continue;
        }
        viewer->menu_owed = true;
        send_menu_to(*viewer);
    }
}

void WebPlayServer::send_menu_to(Viewer& viewer) {
    if (!menu_open_) {
        static const std::string closed = R"({"type":"menu","open":false})";
        send_frame(viewer, 1, reinterpret_cast<const uint8_t*>(closed.data()), closed.size());
        return;
    }
    // A picture is big; one that would only queue behind a slow connection
    // waits for it to drain, and is then the latest one.
    if (menu_image_.empty() || viewer.stream.pending_bytes() > kVideoBacklogLimit) return;
    send_frame(viewer, 2, menu_image_.data(), menu_image_.size());
    viewer.menu_owed = false;
}

std::size_t WebPlayServer::pending_bytes(uint32_t id) const {
    for (const auto& viewer : viewers_) {
        if (viewer->id == id) return viewer->stream.pending_bytes();
    }
    return 0;
}

}  // namespace ds::net
