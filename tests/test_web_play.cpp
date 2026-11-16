// Web Play's server over loopback: the page, the WebSocket handshake, joining,
// a browser's pads arriving as pads, video only from a keyframe on, and the
// whole thing again over TLS with a certificate made on the spot.

#include "test_support.hpp"

#include <chrono>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "net/socket_api.hpp"
#include "net/transport.hpp"
#include "net/web_play_host.hpp"
#include "net/web_play_server.hpp"
#include "net/web_tls.hpp"

using namespace ds;
using namespace ds::net;

namespace {

uint64_t now_ms() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}

// A browser, as little of one as the server needs.
struct Client {
    SecureStream stream;
    std::vector<uint8_t> inbox;
    bool connect(uint16_t port, TlsContext* tls) {
        TcpStream tcp;
        std::string error;
        if (!tcp.start_connect("127.0.0.1", port, &error)) return false;
        std::vector<uint8_t> ignored;
        const uint64_t deadline = now_ms() + 2000;
        while (!tcp.connected() && now_ms() < deadline) {
            tcp.pump(ignored, &error);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (!tcp.connected()) return false;
        return stream.start(tls, tcp.release(), &error);
    }
    void send_masked(uint8_t opcode, const std::vector<uint8_t>& data) {
        std::vector<uint8_t> frame;
        frame.push_back(static_cast<uint8_t>(0x80 | opcode));
        if (data.size() < 126) {
            frame.push_back(static_cast<uint8_t>(0x80 | data.size()));
        } else {
            frame.push_back(0x80 | 126);
            frame.push_back(static_cast<uint8_t>(data.size() >> 8));
            frame.push_back(static_cast<uint8_t>(data.size() & 0xFF));
        }
        const uint8_t mask[4] = {0x12, 0x34, 0x56, 0x78};
        frame.insert(frame.end(), mask, mask + 4);
        for (std::size_t i = 0; i < data.size(); ++i) frame.push_back(data[i] ^ mask[i & 3]);
        stream.send(frame);
    }
    void send_text(const std::string& text) { send_masked(1, std::vector<uint8_t>(text.begin(), text.end())); }
};

void pump_until(WebPlayServer& server, Client& client, const std::function<bool()>& done, int ms = 2000) {
    const uint64_t deadline = now_ms() + static_cast<uint64_t>(ms);
    while (now_ms() < deadline) {
        server.pump();
        std::string error;
        client.stream.pump(client.inbox, &error);
        if (done()) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

std::string as_text(const std::vector<uint8_t>& bytes) { return std::string(bytes.begin(), bytes.end()); }

// The server's frames out of what a client received, after the HTTP head.
std::vector<WebSocketReader::Message> server_frames(const std::vector<uint8_t>& bytes) {
    std::vector<WebSocketReader::Message> out;
    const std::string text = as_text(bytes);
    const std::size_t head = text.find("\r\n\r\n");
    if (head == std::string::npos) return out;
    WebSocketReader reader;
    reader.feed(bytes.data() + head + 4, bytes.size() - head - 4, out);
    return out;
}

void test_accept_key() {
    // RFC 6455's own example.
    test::check_equal_strings(websocket_accept_key("dGhlIHNhbXBsZSBub25jZQ=="), "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=",
                        "accept key", __FILE__, __LINE__);
}

void test_reader() {
    WebSocketReader reader;
    std::vector<WebSocketReader::Message> out;
    // "Hello", masked, as RFC 6455 has it -- fed a byte at a time.
    const uint8_t hello[] = {0x81, 0x85, 0x37, 0xfa, 0x21, 0x3d, 0x7f, 0x9f, 0x4d, 0x51, 0x58};
    for (uint8_t byte : hello) CHECK(reader.feed(&byte, 1, out));
    CHECK_EQ(out.size(), 1u);
    if (!out.empty()) {
        CHECK_EQ(static_cast<int>(out[0].opcode), 1);
        test::check_equal_strings(as_text(out[0].data), "Hello", "unmasked", __FILE__, __LINE__);
    }
    // Fragmented, unmasked, with a ping in the middle.
    out.clear();
    const uint8_t pieces[] = {0x01, 0x03, 'H', 'e', 'l', 0x89, 0x00, 0x80, 0x02, 'l', 'o'};
    CHECK(reader.feed(pieces, sizeof(pieces), out));
    CHECK_EQ(out.size(), 2u);
    if (out.size() == 2) {
        CHECK_EQ(static_cast<int>(out[0].opcode), 9);
        test::check_equal_strings(as_text(out[1].data), "Hello", "joined", __FILE__, __LINE__);
    }
    // A long one, through websocket_frame and back.
    out.clear();
    std::vector<uint8_t> big(70000);
    for (std::size_t i = 0; i < big.size(); ++i) big[i] = static_cast<uint8_t>(i * 7);
    const std::vector<uint8_t> frame = websocket_frame(2, big.data(), big.size());
    CHECK(reader.feed(frame.data(), frame.size(), out));
    CHECK(out.size() == 1 && out[0].data == big);
}

void run_session(bool tls) {
    WebPlayServer server;
    WebPlayServer::Config config;
    config.page = "<html>web play</html>";
    config.device_name = "Test Deck";
    config.game_title = "Test Game";
    TlsContext client_tls;
    std::string error;
    if (tls) {
        CHECK_MSG(make_self_signed_identity("Test Deck", config.identity, &error), error);
        CHECK_MSG(tls_identity_usable(config.identity, &error), error);
        CHECK(tls_fingerprint(config.identity).size() == 32 * 3 - 1);
        CHECK(client_tls.start_client(&error));
    }
    CHECK_MSG(server.start(config, &error), error);
    TlsContext* client_context = tls ? &client_tls : nullptr;

    // The page.
    {
        Client page;
        CHECK(page.connect(server.port(), client_context));
        page.stream.send(std::string("GET / HTTP/1.1\r\nHost: x\r\n\r\n"));
        pump_until(server, page, [&] { return as_text(page.inbox).find("web play") != std::string::npos; });
        CHECK_MSG(as_text(page.inbox).find("200 OK") != std::string::npos, as_text(page.inbox));
        CHECK(as_text(page.inbox).find("<html>web play</html>") != std::string::npos);
    }
    if (tls) {
        // Someone who typed http:// is sent to https://.
        Client typed;
        CHECK(typed.connect(server.port(), nullptr));
        typed.stream.send(std::string("GET /x HTTP/1.1\r\nHost: deck:8443\r\n\r\n"));
        pump_until(server, typed, [&] { return as_text(typed.inbox).find("\r\n\r\n") != std::string::npos; });
        CHECK_MSG(as_text(typed.inbox).find("Location: https://deck:8443/x") != std::string::npos, as_text(typed.inbox));
    }

    // The WebSocket.
    Client browser;
    CHECK(browser.connect(server.port(), client_context));
    browser.stream.send(std::string("GET /ws HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                                    "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n"));
    pump_until(server, browser, [&] { return server_frames(browser.inbox).size() >= 1; });
    CHECK(as_text(browser.inbox).find("101 Switching Protocols") != std::string::npos);
    CHECK(as_text(browser.inbox).find("s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") != std::string::npos);
    {
        const auto frames = server_frames(browser.inbox);
        CHECK(!frames.empty() && as_text(frames[0].data).find("\"device\":\"Test Deck\"") != std::string::npos);
    }
    CHECK(server.viewers().empty());
    CHECK(!server.screen_watched(ScreenId::Top));

    browser.send_text(R"({"type":"hello","name":"  Sam  "})");
    browser.send_text(R"({"type":"pads","pads":[{"index":0,"name":"Xbox Controller"},{"index":15,"name":"Touch Controls"}]})");
    // Pad 0: A and D-pad up held, left stick fully right.
    std::vector<uint8_t> pads = {static_cast<uint8_t>(WebMessage::Pads), 1, 0};
    const uint32_t bits = (1u << SDL_CONTROLLER_BUTTON_A) | (1u << SDL_CONTROLLER_BUTTON_DPAD_UP);
    for (int shift = 0; shift < 32; shift += 8) pads.push_back(static_cast<uint8_t>(bits >> shift));
    const int16_t axes[6] = {32767, 0, 0, -32768, 0, 32767};
    for (int16_t axis : axes) {
        pads.push_back(static_cast<uint8_t>(static_cast<uint16_t>(axis) & 0xFF));
        pads.push_back(static_cast<uint8_t>(static_cast<uint16_t>(axis) >> 8));
    }
    browser.send_masked(2, pads);
    pump_until(server, browser, [&] { return server.pads().size() == 2 && server.pads()[0].raw.buttons[0]; });

    const auto viewers = server.viewers();
    CHECK_EQ(viewers.size(), 1u);
    if (!viewers.empty()) test::check_equal_strings(viewers[0].name, "Sam", "name trimmed", __FILE__, __LINE__);
    const auto known = server.pads();
    CHECK_EQ(known.size(), 2u);
    if (known.size() == 2) {
        test::check_equal_strings(known[0].name, "Xbox Controller", "pad name", __FILE__, __LINE__);
        CHECK(known[0].raw.buttons[SDL_CONTROLLER_BUTTON_A]);
        CHECK(known[0].raw.buttons[SDL_CONTROLLER_BUTTON_DPAD_UP]);
        CHECK(!known[0].raw.buttons[SDL_CONTROLLER_BUTTON_B]);
        CHECK_EQ(static_cast<int>(known[0].raw.axes[SDL_CONTROLLER_AXIS_LEFTX]), 32767);
        CHECK_EQ(static_cast<int>(known[0].raw.axes[SDL_CONTROLLER_AXIS_RIGHTY]), -32768);
        CHECK(is_web_pad_id(known[0].id));
        CHECK(known[0].id != known[1].id);
        CHECK_EQ(static_cast<int>(known[1].index), 15);
    }
    CHECK(server.screen_watched(ScreenId::Top));
    CHECK(!server.screen_watched(ScreenId::Bottom));
    CHECK(server.take_keyframe_request(ScreenId::Top));
    CHECK(!server.take_keyframe_request(ScreenId::Top));

    // Video: a frame that isn't a keyframe is held back from a browser that
    // has none yet; the keyframe and what follows it are sent.
    const uint8_t delta[] = {0, 0, 0, 1, 0x41, 0xAA};
    server.send_video(ScreenId::Top, delta, sizeof(delta), false, 1280, 720);
    pump_until(server, browser, [] { return false; }, 50);
    const auto count_video = [&] {
        int count = 0;
        for (const auto& frame : server_frames(browser.inbox)) {
            if (frame.opcode == 2 && !frame.data.empty() && frame.data[0] == static_cast<uint8_t>(WebMessage::Video)) {
                ++count;
            }
        }
        return count;
    };
    CHECK_EQ(count_video(), 0);
    const uint8_t key[] = {0, 0, 0, 1, 0x67, 0x42, 0, 0x1f, 0, 0, 0, 1, 0x65, 0xBB};
    server.send_video(ScreenId::Top, key, sizeof(key), true, 1280, 720);
    server.send_video(ScreenId::Top, delta, sizeof(delta), false, 1280, 720);
    pump_until(server, browser, [&] { return count_video() == 2; });
    CHECK_EQ(count_video(), 2);
    for (const auto& frame : server_frames(browser.inbox)) {
        if (frame.opcode != 2 || frame.data.size() < 7 || frame.data[0] != 1) continue;
        CHECK_EQ(static_cast<int>(frame.data[1]), 0);
        CHECK_EQ(static_cast<int>(frame.data[3] | (frame.data[4] << 8)), 1280);
        CHECK_EQ(static_cast<int>(frame.data[5] | (frame.data[6] << 8)), 720);
        break;
    }

    // Moved to the GamePad: the TV is no longer sent to it, and a keyframe of
    // the GamePad is asked for.
    server.set_viewer_screen(viewers.empty() ? 0 : viewers[0].id, ScreenId::Bottom);
    CHECK(!server.screen_watched(ScreenId::Top));
    CHECK(server.screen_watched(ScreenId::Bottom));
    CHECK(server.take_keyframe_request(ScreenId::Bottom));
    const uint32_t sam = viewers.empty() ? 0 : viewers[0].id;
    const auto settle = [&] { pump_until(server, browser, [] { return false; }, 50); };

    // A finger on the GamePad's picture is the GamePad's touch screen.
    browser.send_masked(2, {static_cast<uint8_t>(WebMessage::Touch), 1, 0x00, 0x80, 0xFF, 0xFF});
    pump_until(server, browser, [&] { return !server.viewers().empty() && server.viewers()[0].touching; });
    {
        const auto now = server.viewers();
        CHECK(!now.empty() && now[0].touching);
        if (!now.empty()) {
            CHECK_EQ(static_cast<int>(now[0].touch_x), 0x8000);
            CHECK_EQ(static_cast<int>(now[0].touch_y), 0xFFFF);
        }
    }

    // Asking to see both: both are sent to it, and it still touches the
    // GamePad's.
    browser.send_text(R"({"type":"view","view":"both"})");
    pump_until(server, browser, [&] { return server.screen_watched(ScreenId::Top); });
    CHECK(server.screen_watched(ScreenId::Top));
    CHECK(server.screen_watched(ScreenId::Bottom));
    CHECK(server.take_keyframe_request(ScreenId::Top));
    browser.send_masked(2, {static_cast<uint8_t>(WebMessage::Touch), 1, 0x10, 0x00, 0x10, 0x00});
    pump_until(server, browser, [&] { return !server.viewers().empty() && server.viewers()[0].touching; });
    CHECK(!server.viewers().empty() && server.viewers()[0].touching);
    // Only the TV: the GamePad is not sent, and there is nothing to touch.
    browser.send_text(R"({"type":"view","view":"tv"})");
    pump_until(server, browser, [&] { return !server.screen_watched(ScreenId::Bottom); });
    CHECK(server.screen_watched(ScreenId::Top));
    CHECK(!server.screen_watched(ScreenId::Bottom));
    CHECK(!server.viewers().empty() && !server.viewers()[0].touching);
    // Given up the GamePad, it sees the TV whatever it chose.
    server.set_viewer_screen(sam, ScreenId::Top);
    browser.send_text(R"({"type":"view","view":"gamepad"})");
    settle();
    CHECK(server.screen_watched(ScreenId::Top));
    CHECK(!server.screen_watched(ScreenId::Bottom));

    // The quick menu: its picture while it is open, and that it closed.
    const auto count_menu = [&] {
        int count = 0;
        for (const auto& frame : server_frames(browser.inbox)) {
            if (frame.opcode == 2 && !frame.data.empty() && frame.data[0] == static_cast<uint8_t>(WebMessage::Menu)) {
                ++count;
            }
        }
        return count;
    };
    std::vector<uint8_t> picture = {static_cast<uint8_t>(WebMessage::Menu), 0, 4, 0, 2, 0, 0xFF, 0xD8};
    server.send_menu(true, picture);
    CHECK(server.menu_open());
    pump_until(server, browser, [&] { return count_menu() == 1; });
    CHECK_EQ(count_menu(), 1);
    server.send_menu(false, {});
    pump_until(server, browser, [&] { return as_text(browser.inbox).find(R"("type":"menu","open":false)") != std::string::npos; });
    CHECK(as_text(browser.inbox).find(R"("type":"menu","open":false)") != std::string::npos);
    CHECK(!server.menu_open());

    // Leaving: gone from the list, and its pads with it.
    browser.send_masked(8, {});
    pump_until(server, browser, [&] { return server.viewers().empty(); });
    CHECK(server.viewers().empty());
    CHECK(server.pads().empty());
    server.stop();
}

}  // namespace

// The menu's picture: a JPEG when nothing shows through it, a PNG when
// something does, no bigger than asked.
void test_menu_picture() {
    std::vector<uint8_t> rgba(64 * 32 * 4, 200);
    std::vector<uint8_t> message = web_menu_message(rgba.data(), 64, 32, 32, 32);
    CHECK(message.size() > 8);
    if (message.size() > 8) {
        CHECK_EQ(static_cast<int>(message[0]), static_cast<int>(WebMessage::Menu));
        CHECK_EQ(static_cast<int>(message[1]), 1);  // alpha 200 shows through
        CHECK_EQ(static_cast<int>(message[2] | (message[3] << 8)), 32);
        CHECK_EQ(static_cast<int>(message[4] | (message[5] << 8)), 16);
        CHECK(message[6] == 0x89 && message[7] == 'P');
    }
    for (std::size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
    message = web_menu_message(rgba.data(), 64, 32, 1280, 720);
    CHECK(message.size() > 8);
    if (message.size() > 8) {
        CHECK_EQ(static_cast<int>(message[1]), 0);
        CHECK_EQ(static_cast<int>(message[2] | (message[3] << 8)), 64);
        CHECK(message[6] == 0xFF && message[7] == 0xD8);
    }
}

int main() {
    test_accept_key();
    test_menu_picture();
    test_reader();
    run_session(false);
    run_session(true);
    return test::finish("web_play");
}
