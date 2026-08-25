// Exercises the real sockets over loopback: a host announcing itself, a
// companion finding it, and a TCP stream carrying framed messages both ways.

#include "test_support.hpp"

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

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

// A peer that vanishes without closing (sleep, dead Wi-Fi, power loss) looks
// identical to one still there unless something actively probes for it. If
// nothing does, that connection sits "open" forever and, on the host side,
// permanently occupies the one slot a real reconnect needs. Keepalive is what
// notices, so both ends need it turned on with a short enough idle time to be
// useful.
void test_streams_enable_keepalive_with_a_short_idle_time() {
    TcpListener listener;
    std::string error;
    CHECK_MSG(listener.start(0, &error), error);
    const uint16_t port = listener.port();

    TcpStream client;
    CHECK_MSG(client.start_connect("127.0.0.1", port, &error), error);

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

    for (const TcpStream* stream : {&client, &server}) {
        const int descriptor = stream->descriptor_for_test();

        int keepalive = 0;
        socklen_t length = sizeof(keepalive);
        CHECK(::getsockopt(descriptor, SOL_SOCKET, SO_KEEPALIVE, &keepalive, &length) == 0);
        CHECK_MSG(keepalive != 0, "a stream did not enable SO_KEEPALIVE");

        int idle_seconds = 0;
        length = sizeof(idle_seconds);
        CHECK(::getsockopt(descriptor, IPPROTO_TCP, TCP_KEEPIDLE, &idle_seconds, &length) == 0);
        // The OS default is two hours -- ten seconds' worth of probing has to
        // have actually been configured, not left at that default.
        CHECK_MSG(idle_seconds > 0 && idle_seconds <= 10,
                 "TCP_KEEPIDLE was left near the OS default (" +
                     std::to_string(idle_seconds) + "s)");
    }

    client.close();
    server.close();
}

// How much can vanish into the kernel's send queue before the app can see it.
//
// The frame publisher drops a frame rather than queueing it when the link is
// behind, so a companion that cannot keep up sees dropped frames instead of
// mounting latency. It decides that from pending_bytes(), which counts only
// what is still in this process -- so everything the kernel has taken is
// latency the publisher cannot see and cannot drop. Left to autotune that
// queue reached 3.9 MB here, some ninety-five frames of a DS screen, and the
// drop logic never once fired.
void test_the_kernel_send_queue_is_small_enough_to_notice() {
    TcpListener listener;
    std::string error;
    CHECK_MSG(listener.start(0, &error), error);

    TcpStream client;
    CHECK_MSG(client.start_connect("127.0.0.1", listener.port(), &error), error);

    TcpStream server;
    std::vector<uint8_t> inbox;
    const uint64_t deadline = monotonic_millis() + 2000;
    while (!server.open() && monotonic_millis() < deadline) {
        listener.accept(server);
        client.pump(inbox, &error);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK_MSG(server.open(), "the listener never accepted the connection");
    if (!server.open()) return;

    // Both ends, because a bound that was only applied to one of them would
    // leave whichever path this test does not walk unbounded.
    for (const TcpStream* stream : {&client, &server}) {
        int send_buffer = 0;
        socklen_t length = sizeof(send_buffer);
        CHECK(::getsockopt(stream->descriptor_for_test(), SOL_SOCKET, SO_SNDBUF, &send_buffer,
                           &length) == 0);
        // Linux reports back roughly twice what was asked for, so this is a
        // check that a bound was set at all rather than one on its exact size.
        CHECK_MSG(send_buffer > 0 && send_buffer <= 512 * 1024,
                  "a stream's send queue was left to autotune (" + std::to_string(send_buffer) +
                      " bytes)");
    }

    // The measurement itself runs on the accepted socket, because that is the
    // one the host streams frames down: it accepts a companion rather than
    // connecting to one. A receiver that never reads a byte, with a small
    // window of its own, so what this measures is the sending side.
    const int receive_buffer = 32 * 1024;
    ::setsockopt(client.descriptor_for_test(), SOL_SOCKET, SO_RCVBUF, &receive_buffer,
                 sizeof(receive_buffer));

    // 40 KB a frame: what one dynamic DS screen actually costs after RGB565
    // and deflate.
    constexpr std::size_t kFrameBytes = 40 * 1024;
    const std::vector<uint8_t> frame(kFrameBytes, 0xa5);
    std::size_t accepted = 0;
    for (int i = 0; i < 200; ++i) {
        server.send(frame);
        if (!server.pump(inbox, &error)) break;
        if (server.pending_bytes() > 0) break;  // the backlog is finally visible
        accepted += frame.size();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    const double hidden_frames = static_cast<double>(accepted) / static_cast<double>(kFrameBytes);
    CHECK_MSG(hidden_frames < 16.0,
              "the kernel took " + std::to_string(hidden_frames) +
                  " frames before the backlog became visible");

    client.close();
    server.close();
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
    test_streams_enable_keepalive_with_a_short_idle_time();
    test_the_kernel_send_queue_is_small_enough_to_notice();
    test_connect_to_nothing_fails();
    return test::finish("net");
}
