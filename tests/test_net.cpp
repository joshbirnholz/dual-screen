// Exercises the real sockets over loopback: a host announcing itself, a
// companion finding it, and a TCP stream carrying framed messages both ways.

#include "test_support.hpp"

#include <cerrno>

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

// A connect that has not finished must not claim it has. SO_ERROR only means
// anything once the socket reports itself writable; asked before that it reads
// zero, which is indistinguishable from success -- and taking that for a
// finished handshake makes an address this device cannot route to look exactly
// like a host that is connected and simply quiet.
//
// 203.0.113.0/24 is TEST-NET-3, reserved for documentation and routed nowhere,
// so a connect to it hangs in SYN retries rather than being refused.
void test_a_connect_in_progress_does_not_claim_to_be_connected() {
    TcpStream stream;
    std::string error;
    if (!stream.start_connect("203.0.113.7", kSessionPort, &error)) {
        return;  // this machine refuses the route outright, which is not the case under test
    }
    CHECK(!stream.connected());
    std::vector<uint8_t> inbox;
    const uint64_t deadline = monotonic_millis() + 500;
    while (monotonic_millis() < deadline) {
        std::string reason;
        if (!stream.pump(inbox, &reason)) {
            return;  // an outright failure is a legitimate answer; claiming success is not
        }
        CHECK_MSG(!stream.connected(),
                  "a connect that cannot possibly have completed reported itself connected");
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

std::vector<LocalNetwork> a_machine_on_one_network_twice() {
    // As the interfaces arrive from the kernel: the wire is not first, so
    // preferring it has to be a decision rather than an accident of order.
    return {
        {"lo", "127.0.0.1", "127.0.0.1", false},
        {"wlan0", "192.168.0.98", "192.168.0.255", true},
        {"enp42s0", "192.168.0.103", "192.168.0.255", false},
    };
}

void test_one_network_reached_twice_is_called_out() {
    std::vector<LocalNetwork> alone {
        {"lo", "127.0.0.1", "127.0.0.1", false},
        {"enp42s0", "192.168.0.103", "192.168.0.255", false},
    };
    CHECK_MSG(shared_network_warning(alone).empty(),
              "a machine with one way onto each network was warned about anyway");

    const std::string warning = shared_network_warning(a_machine_on_one_network_twice());
    CHECK_MSG(warning.find("enp42s0 (192.168.0.103)") != std::string::npos &&
                  warning.find("wlan0 (192.168.0.98)") != std::string::npos,
              "a machine on one network through two interfaces did not name them both");
    CHECK_MSG(warning.find("pointed at enp42s0") != std::string::npos,
              "the warning did not say companions are pointed at the wired interface: " + warning);
}

void test_a_network_reached_twice_is_announced_once_over_the_wire() {
    const std::vector<LocalNetwork> announce =
        announce_networks(a_machine_on_one_network_twice());
    CHECK_MSG(announce.size() == 2,
              "one network reached twice was announced to " + std::to_string(announce.size()) +
                  " addresses rather than 2");
    for (const LocalNetwork& network : announce) {
        if (network.broadcast != "192.168.0.255") continue;
        CHECK_MSG(network.interface == "enp42s0",
                  "a network reachable by wire and by wifi was announced from " +
                      network.interface + " rather than the wire");
    }
}

void test_a_beacon_pinned_to_an_interface_still_arrives() {
    // Which interface gets chosen cannot be checked on a machine that is not
    // on one network twice. What can be checked is that asking for a specific
    // one still delivers a datagram carrying an address to dial back.
    UdpSocket listener;
    std::string error;
    CHECK(listener.start(kDiscoveryPort, false, &error));

    UdpSocket sender;
    CHECK(sender.start(0, true, &error));
    const std::vector<uint8_t> payload {1, 2, 3, 4};
    CHECK(sender.broadcast(kDiscoveryPort, payload.data(), payload.size()));

    std::vector<uint8_t> received;
    std::string from;
    const uint64_t deadline = monotonic_millis() + 1000;
    while (received.empty() && monotonic_millis() < deadline) {
        if (!listener.receive(received, from)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    CHECK_MSG(!received.empty(), "a beacon sent out of a named interface never arrived");
    CHECK_MSG(!from.empty() && from != "0.0.0.0",
              "a beacon arrived without a source address for a companion to dial back");
}

// The media channel's own send: one datagram to one address, learned from a
// received one rather than self-reported -- see UdpSocket::send_to() and
// HostSession's session-token handshake.
void test_send_to_reaches_an_address_learned_from_a_receive() {
    UdpSocket host_socket;
    std::string error;
    // Bound to the real media port, the way HostSession actually would --
    // its address is what the companion's own send_to() needs to know in
    // advance (unlike the reply below, which is learned from a receive).
    CHECK(host_socket.start(kMediaPort, false, &error));

    UdpSocket companion_socket;
    CHECK(companion_socket.start(0, false, &error));

    // The companion announces itself, the same way it would send its
    // session-token datagram to the host's kMediaPort in the real handshake.
    const std::vector<uint8_t> hello_payload {9};
    CHECK(companion_socket.send_to("127.0.0.1", kMediaPort, hello_payload.data(),
                                   hello_payload.size()));

    std::vector<uint8_t> received;
    std::string companion_address;
    uint16_t companion_port = 0;
    const uint64_t deadline = monotonic_millis() + 1000;
    while (received.empty() && monotonic_millis() < deadline) {
        if (!host_socket.receive(received, companion_address, companion_port)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    CHECK_MSG(!received.empty(), "the companion's announcing datagram never reached the host");
    CHECK(companion_port != 0);

    // Now the host answers back to the address and port it just learned,
    // without either side having agreed on it in advance.
    const std::vector<uint8_t> reply_payload {1, 2, 3};
    CHECK(host_socket.send_to(companion_address, companion_port, reply_payload.data(),
                              reply_payload.size()));

    std::vector<uint8_t> reply;
    std::string ignored_address;
    const uint64_t reply_deadline = monotonic_millis() + 1000;
    while (reply.empty() && monotonic_millis() < reply_deadline) {
        if (!companion_socket.receive(reply, ignored_address)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    CHECK(reply == reply_payload);
}

}  // namespace

// Which connect failures are worth asking again about.
//
// macOS decides whether an app may talk to the local network the first time it
// tries to, and the attempt that asks the question is the one that gets
// refused: joining a host used to fail on the first try and work on the
// second, for an app the player had already allowed. Trying again inside the
// join deadline is what turns that back into one connection that took a beat.
//
// The list has to stay short. A refused connection is a host that really is
// not listening and a timed-out one is a host that really is not there;
// retrying either only makes the player wait longer to be told so.
void test_only_a_permission_gate_is_worth_asking_again() {
    CHECK(connect_error_looks_like_a_permission_gate(EHOSTUNREACH));
    CHECK(connect_error_looks_like_a_permission_gate(ENETUNREACH));
    CHECK(connect_error_looks_like_a_permission_gate(EPERM));
    CHECK(connect_error_looks_like_a_permission_gate(EACCES));

    CHECK(!connect_error_looks_like_a_permission_gate(ECONNREFUSED));
    CHECK(!connect_error_looks_like_a_permission_gate(ETIMEDOUT));
    CHECK(!connect_error_looks_like_a_permission_gate(ECONNRESET));
    CHECK(!connect_error_looks_like_a_permission_gate(0));

    // And only where there is a gate to be waiting on. Everywhere else those
    // errnos mean what they say, so a session must take them at their word
    // rather than spending its whole join deadline asking again.
#if defined(__APPLE__)
    CHECK(connect_is_undecided(EHOSTUNREACH));
#else
    CHECK(!connect_is_undecided(EHOSTUNREACH));
#endif
    CHECK(!connect_is_undecided(ECONNREFUSED));
}

// The errno behind a failed connect has to outlive the socket, because the
// caller only hears about the failure after the stream has closed itself --
// that is where the retry decision above gets made.
void test_a_failed_connect_remembers_why() {
    TcpStream stream;
    std::string error;
    CHECK_EQ(stream.connect_error(), 0);
    // Not an address at all, so this fails before any errno is involved: the
    // one that follows must not be left over from it either.
    CHECK(!stream.start_connect("not-an-address", 1, &error));
    CHECK_EQ(stream.connect_error(), 0);

    // A connect that reaches the network and comes straight back -- on
    // loopback, nothing is listening on port 1 -- may report either now or on
    // the first pump. Whichever it is, the reason is there to be asked for.
    if (!stream.start_connect("127.0.0.1", 1, &error)) {
        CHECK(stream.connect_error() != 0);
        CHECK(!connect_is_undecided(stream.connect_error()));
    }
}

int main() {
    test_browser_merges_and_expires();
    test_discovery_over_loopback();
    test_tcp_carries_framed_messages();
    test_streams_enable_keepalive_with_a_short_idle_time();
    test_the_kernel_send_queue_is_small_enough_to_notice();
    test_connect_to_nothing_fails();
    test_only_a_permission_gate_is_worth_asking_again();
    test_a_failed_connect_remembers_why();
    test_a_connect_in_progress_does_not_claim_to_be_connected();
    test_one_network_reached_twice_is_called_out();
    test_a_network_reached_twice_is_announced_once_over_the_wire();
    test_a_beacon_pinned_to_an_interface_still_arrives();
    test_send_to_reaches_an_address_learned_from_a_receive();
    return test::finish("net");
}
