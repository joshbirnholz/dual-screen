// Exercises the real sockets over loopback: a host announcing itself, a
// companion finding it, and a TCP stream carrying framed messages both ways.

#include "test_support.hpp"

#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "net/discovery.hpp"
#include "net/protocol.hpp"
#include "net/transport.hpp"

using namespace ds;
using namespace ds::net;

namespace {

void pump_briefly(TcpStream& a, std::vector<uint8_t>& a_inbox, TcpStream& b,
                  std::vector<uint8_t>& b_inbox, int milliseconds) {
    const uint64_t deadline = monotonic_millis() + static_cast<uint64_t>(milliseconds);
    while (monotonic_millis() < deadline) {
        std::string error;
        a.pump(a_inbox, &error);
        b.pump(b_inbox, &error);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

// The browser's merge and expiry rules, without touching the network.
void test_browser_merges_and_expires() {
    HostBrowser browser;

    BeaconMessage beacon;
    beacon.device_name = "Gaming PC";
    beacon.game_title = "Pokemon Platinum";
    beacon.rom_sha1 = "abc";
    beacon.session_port = 47821;
    beacon.top_available = true;
    beacon.bottom_available = true;

    browser.ingest(beacon, "192.168.1.10", 1000);
    CHECK_EQ(browser.hosts().size(), static_cast<std::size_t>(1));

    // The same host announcing again updates rather than duplicating.
    beacon.bottom_available = false;
    browser.ingest(beacon, "192.168.1.10", 2000);
    CHECK_EQ(browser.hosts().size(), static_cast<std::size_t>(1));
    CHECK_EQ(browser.hosts()[0].bottom_available, false);
    CHECK(browser.hosts()[0].offers(ScreenId::Top));
    CHECK(!browser.hosts()[0].offers(ScreenId::Bottom));

    // A different port on the same address is a different host -- which is what
    // two instances on one machine look like.
    beacon.session_port = 47822;
    browser.ingest(beacon, "192.168.1.10", 2000);
    CHECK_EQ(browser.hosts().size(), static_cast<std::size_t>(2));

    // A host that goes quiet drops off, but not before the grace period.
    browser.expire(2000 + kHostExpiryMs);
    CHECK_EQ(browser.hosts().size(), static_cast<std::size_t>(2));
    browser.expire(2000 + kHostExpiryMs + 1);
    CHECK_EQ(browser.hosts().size(), static_cast<std::size_t>(0));

    // A beacon from a version we do not speak is never shown: connecting would
    // only fail later with a worse message.
    BeaconMessage future = beacon;
    future.protocol_version = kProtocolVersion + 1;
    browser.ingest(future, "192.168.1.11", 3000);
    CHECK_EQ(browser.hosts().size(), static_cast<std::size_t>(0));
}

// The real thing: a UDP broadcast that a browser on the same machine picks up.
void test_discovery_over_loopback() {
    HostBrowser browser;
    std::string error;
    if (!browser.start(&error)) {
        std::fprintf(stderr, "skipping: cannot bind the discovery port (%s)\n", error.c_str());
        return;
    }

    HostAnnouncer announcer;
    CHECK_MSG(announcer.start(&error), error);

    BeaconMessage beacon;
    beacon.device_name = "Test Host";
    beacon.game_title = "Test Game";
    beacon.rom_sha1 = "d0858904048973636d489701cafe3a15bee6ee41";
    beacon.session_port = 47899;
    announcer.set_beacon(beacon);

    // Give it a couple of seconds; broadcast delivery is not instantaneous.
    const uint64_t deadline = monotonic_millis() + 3000;
    bool found = false;
    while (!found && monotonic_millis() < deadline) {
        const uint64_t now = monotonic_millis();
        announcer.poll(now);
        browser.poll(now);
        for (const DiscoveredHost& host : browser.hosts()) {
            if (host.device_name == "Test Host" && host.session_port == 47899) {
                found = true;
                CHECK_STR_EQ(host.game_title, "Test Game");
                CHECK_STR_EQ(host.rom_sha1, beacon.rom_sha1);
                CHECK(!host.address.empty());
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK_MSG(found, "the announcer's beacon never reached the browser");

    announcer.stop();
    browser.stop();
}

// A TCP session carrying real protocol messages in both directions.
void test_tcp_carries_framed_messages() {
    TcpListener listener;
    std::string error;
    CHECK_MSG(listener.start(0, &error), error);  // 0 = any free port
    const uint16_t port = listener.port();
    CHECK(port != 0);

    TcpStream client;
    CHECK_MSG(client.start_connect("127.0.0.1", port, &error), error);

    // Accept, with a little patience for the handshake.
    TcpStream server;
    const uint64_t deadline = monotonic_millis() + 2000;
    while (!server.open() && monotonic_millis() < deadline) {
        listener.accept(server);
        std::vector<uint8_t> ignored;
        client.pump(ignored, &error);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK_MSG(server.open(), "the listener never accepted the connection");
    if (!server.open()) return;

    // Companion -> host: a hello, then input.
    HelloMessage hello;
    hello.device_name = "Companion";
    hello.wanted_screen = ScreenId::Bottom;
    std::vector<uint8_t> outgoing;
    encode_hello(hello, outgoing);
    InputMessage input;
    input.buttons = 0b1010;
    input.touching = true;
    input.touch_x = 42;
    input.touch_y = 24;
    encode_input(input, outgoing);
    client.send(outgoing);

    // Host -> companion: a welcome and a frame.
    WelcomeMessage welcome;
    welcome.game_title = "Test Game";
    welcome.granted_screen = ScreenId::Bottom;
    std::vector<uint8_t> reply;
    encode_welcome(welcome, reply);

    std::vector<uint32_t> screen(256 * 192, 0x00336699u);
    FrameMessage frame;
    frame.screen = ScreenId::Bottom;
    frame.serial = 7;
    frame.width = 256;
    frame.height = 192;
    CHECK(encode_screen_pixels(screen.data(), 256, 192, 256, frame.payload));
    encode_frame(frame, reply);
    server.send(reply);

    std::vector<uint8_t> client_inbox;
    std::vector<uint8_t> server_inbox;
    pump_briefly(client, client_inbox, server, server_inbox, 500);

    // What the host received.
    MessageType type = MessageType::Ping;
    std::vector<uint8_t> payload;
    bool fatal = false;
    CHECK(take_message(server_inbox, type, payload, fatal));
    CHECK(type == MessageType::Hello);
    HelloMessage received_hello;
    CHECK(decode_hello(payload.data(), payload.size(), received_hello));
    CHECK_STR_EQ(received_hello.device_name, "Companion");

    CHECK(take_message(server_inbox, type, payload, fatal));
    CHECK(type == MessageType::Input);
    InputMessage received_input;
    CHECK(decode_input(payload.data(), payload.size(), received_input));
    CHECK_EQ(received_input.buttons, 0b1010u);
    CHECK_EQ(received_input.touch_x, 42);

    // What the companion received.
    CHECK(take_message(client_inbox, type, payload, fatal));
    CHECK(type == MessageType::Welcome);
    CHECK(take_message(client_inbox, type, payload, fatal));
    CHECK(type == MessageType::Frame);
    FrameMessage received_frame;
    CHECK(decode_frame(payload.data(), payload.size(), received_frame));
    CHECK_EQ(received_frame.serial, static_cast<uint64_t>(7));
    CHECK(received_frame.screen == ScreenId::Bottom);

    std::vector<uint32_t> decoded;
    CHECK(decode_screen_pixels(received_frame.payload, received_frame.width,
                               received_frame.height, decoded));
    CHECK_EQ(decoded.size(), screen.size());
    // RGB565 is lossy by design -- that is what makes streaming cheap enough to
    // skip a video encoder -- so the guarantee is closeness, not equality.
    const uint32_t original = 0x00336699u;
    const uint32_t got = decoded[0] & 0x00ffffffu;
    for (int shift : {16, 8, 0}) {
        const int a = static_cast<int>((original >> shift) & 0xffu);
        const int b = static_cast<int>((got >> shift) & 0xffu);
        CHECK_MSG(std::abs(a - b) <= 8, "channel drifted from " + std::to_string(a) + " to " +
                                            std::to_string(b));
    }

    // Closing one end must be reported as a disconnect, not hang.
    server.close();
    bool client_saw_disconnect = false;
    const uint64_t close_deadline = monotonic_millis() + 1000;
    while (monotonic_millis() < close_deadline) {
        std::string reason;
        if (!client.pump(client_inbox, &reason)) {
            client_saw_disconnect = true;
            CHECK(!reason.empty());
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK_MSG(client_saw_disconnect, "a closed peer was not reported as a disconnect");
}

// A refused connection has to surface as a failure rather than hanging.
void test_connect_to_nothing_fails() {
    TcpStream stream;
    std::string error;
    // Port 1 is not going to be listening.
    if (stream.start_connect("127.0.0.1", 1, &error)) {
        bool failed = false;
        const uint64_t deadline = monotonic_millis() + 2000;
        std::vector<uint8_t> inbox;
        while (monotonic_millis() < deadline) {
            std::string reason;
            if (!stream.pump(inbox, &reason)) {
                failed = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        CHECK_MSG(failed, "connecting to a closed port never reported an error");
    }
}

}  // namespace

int main() {
    test_browser_merges_and_expires();
    test_discovery_over_loopback();
    test_tcp_carries_framed_messages();
    test_connect_to_nothing_fails();
    return test::finish("net");
}
