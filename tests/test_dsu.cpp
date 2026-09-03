// The DSU (cemuhook) motion client, against packets built the way a server
// builds them.
//
// Worth testing at this level because the part that can be wrong is not the
// socket -- it is the twenty-four bytes at the end of a hundred-byte packet
// and what they mean. A sign flipped here is a game that tilts the right
// amount the wrong way, which looks like a bug in the emulator rather than in
// this, so the transform is pinned down here rather than found on hardware.

#include "test_support.hpp"

#include <SDL.h>

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "input/dsu_client.hpp"
#include "net/transport.hpp"

using namespace ds;

namespace {

constexpr float kDegreesToRadians = 0.01745329252f;
constexpr float kGravity = 9.80665f;

void put_u16(uint8_t* out, uint16_t value) {
    out[0] = static_cast<uint8_t>(value & 0xff);
    out[1] = static_cast<uint8_t>((value >> 8) & 0xff);
}

void put_u32(uint8_t* out, uint32_t value) {
    out[0] = static_cast<uint8_t>(value & 0xff);
    out[1] = static_cast<uint8_t>((value >> 8) & 0xff);
    out[2] = static_cast<uint8_t>((value >> 16) & 0xff);
    out[3] = static_cast<uint8_t>((value >> 24) & 0xff);
}

void put_f32(uint8_t* out, float value) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    put_u32(out, bits);
}

// A controller-data packet, laid out exactly as SteamDeckGyroDSU sends one.
std::vector<uint8_t> data_packet(float accel_x, float accel_y, float accel_z, float pitch,
                                 float yaw, float roll, uint64_t timestamp_us = 1234567,
                                 uint8_t slot_state = 2, uint8_t device_model = 2) {
    std::vector<uint8_t> packet(100, 0);
    packet[0] = 'D';
    packet[1] = 'S';
    packet[2] = 'U';
    packet[3] = 'S';
    put_u16(packet.data() + 4, 1001);
    put_u16(packet.data() + 6, static_cast<uint16_t>(packet.size() - 16));
    put_u32(packet.data() + 12, 0x1234abcd);   // the server's id
    put_u32(packet.data() + 16, 0x100002u);    // controller data
    packet[20] = 0;                            // slot
    packet[21] = slot_state;
    packet[22] = device_model;
    packet[23] = 1;   // connected over USB
    packet[31] = 1;   // the pad is connected
    put_u32(packet.data() + 32, 7);  // packet number
    put_u32(packet.data() + 68, static_cast<uint32_t>(timestamp_us & 0xffffffffull));
    put_u32(packet.data() + 72, static_cast<uint32_t>(timestamp_us >> 32));
    put_f32(packet.data() + 76, accel_x);
    put_f32(packet.data() + 80, accel_y);
    put_f32(packet.data() + 84, accel_z);
    put_f32(packet.data() + 88, pitch);
    put_f32(packet.data() + 92, yaw);
    put_f32(packet.data() + 96, roll);
    put_u32(packet.data() + 8, dsu_crc32(packet.data(), packet.size()));
    return packet;
}

void test_a_reading_comes_through_in_sdl_units_and_axes() {
    // Degrees a second and g on the wire; radians a second and metres per
    // second squared out, which is what DsMotionState carries.
    const auto packet = data_packet(0.1f, 0.2f, 0.9f, 30.0f, 60.0f, 90.0f);
    DsMotionState motion;
    CHECK(parse_dsu_motion(packet.data(), packet.size(), motion));
    CHECK(motion.present);

    CHECK(test::nearly_equal(motion.gyro_x, 30.0f * kDegreesToRadians));
    CHECK(test::nearly_equal(motion.gyro_y, -60.0f * kDegreesToRadians));
    CHECK(test::nearly_equal(motion.gyro_z, -90.0f * kDegreesToRadians));
    CHECK(test::nearly_equal(motion.accel_x, -0.1f * kGravity));
    CHECK(test::nearly_equal(motion.accel_y, -0.2f * kGravity));
    CHECK(test::nearly_equal(motion.accel_z, -0.9f * kGravity));
    CHECK(motion.sampled_us == 1234567u);
}

void test_the_conversion_agrees_with_what_cemu_would_have_done_itself() {
    // The property that actually matters, and the only one that can be
    // checked without hardware.
    //
    // Cemu already reads both of these formats, through two providers that
    // each hand the same fusion the same six numbers. Its DSU reader converts
    // degrees to radians and negates the second and third acceleration axes;
    // its SDL reader negates the second and third rotation axes and the first
    // acceleration axis, and divides acceleration by gravity. This app sends
    // SDL-convention readings and the Cemu side applies the second of those,
    // so a DSU reading has to be turned into the SDL reading that lands in
    // the same place -- and "the same place" is exactly what is asserted
    // below, rather than a set of signs copied out and hoped over.
    const float accel[3] = {0.13f, -0.42f, 0.88f};
    const float gyro[3] = {12.5f, -3.25f, 44.0f};  // degrees a second

    // What Cemu's own DSU reader would pass to the fusion.
    const float want_gyro[3] = {gyro[0] * kDegreesToRadians, gyro[1] * kDegreesToRadians,
                                gyro[2] * kDegreesToRadians};
    const float want_accel[3] = {accel[0], -accel[1], -accel[2]};

    const auto packet =
        data_packet(accel[0], accel[1], accel[2], gyro[0], gyro[1], gyro[2]);
    DsMotionState motion;
    CHECK(parse_dsu_motion(packet.data(), packet.size(), motion));

    // And what Cemu's SDL reader makes of what this app now sends -- the
    // transform mirrored in VPADController for the dualscreen link.
    const float got_gyro[3] = {motion.gyro_x, -motion.gyro_y, -motion.gyro_z};
    const float got_accel[3] = {-motion.accel_x / kGravity, motion.accel_y / kGravity,
                                motion.accel_z / kGravity};

    for (int axis = 0; axis < 3; ++axis) {
        CHECK(test::nearly_equal(got_gyro[axis], want_gyro[axis]));
        CHECK(test::nearly_equal(got_accel[axis], want_accel[axis]));
    }
}

void test_packets_that_are_not_a_reading_are_refused() {
    DsMotionState motion;

    // Truncated.
    const auto whole = data_packet(0.0f, 0.0f, 1.0f, 1.0f, 2.0f, 3.0f);
    CHECK(!parse_dsu_motion(whole.data(), whole.size() - 1, motion));

    // A version or info answer rather than controller data.
    auto wrong_event = whole;
    put_u32(wrong_event.data() + 16, 0x100000u);
    put_u32(wrong_event.data() + 8, 0);
    put_u32(wrong_event.data() + 8, dsu_crc32(wrong_event.data(), wrong_event.size()));
    CHECK(!parse_dsu_motion(wrong_event.data(), wrong_event.size(), motion));

    // Corrupted in flight.
    auto corrupted = whole;
    corrupted[80] ^= 0xff;
    CHECK(!parse_dsu_motion(corrupted.data(), corrupted.size(), motion));

    // A slot with nothing in it, and a pad the server says has no gyro.
    // Both must read as "no motion" rather than as a device held perfectly
    // still, which is what zeroes would mean downstream.
    const auto empty_slot = data_packet(0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1, 0, 2);
    CHECK(!parse_dsu_motion(empty_slot.data(), empty_slot.size(), motion));
    const auto no_gyro = data_packet(0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1, 2, 1);
    CHECK(!parse_dsu_motion(no_gyro.data(), no_gyro.size(), motion));
}

void test_the_subscription_is_something_a_server_would_accept() {
    uint8_t request[kDsuRequestBytes];
    build_dsu_subscribe(0xdeadbeef, request);

    CHECK(request[0] == 'D' && request[1] == 'S' && request[2] == 'U' && request[3] == 'C');
    CHECK(request[4] == 0xe9 && request[5] == 0x03);  // version 1001, little-endian
    // The length counts everything after itself except the checksum and id --
    // see the protocol: the event type is part of the body it describes.
    CHECK(request[6] == kDsuRequestBytes - 16 && request[7] == 0);
    CHECK(request[16] == 0x02 && request[17] == 0x00 && request[18] == 0x10);

    // And the checksum: taken over the whole packet with its own field zero,
    // which is the one detail a server will reject this over.
    std::vector<uint8_t> copy(request, request + kDsuRequestBytes);
    const uint32_t stated = static_cast<uint32_t>(copy[8]) | (static_cast<uint32_t>(copy[9]) << 8) |
                            (static_cast<uint32_t>(copy[10]) << 16) |
                            (static_cast<uint32_t>(copy[11]) << 24);
    put_u32(copy.data() + 8, 0);
    CHECK(dsu_crc32(copy.data(), copy.size()) == stated);
}

// A stand-in DSU server: takes one subscription and answers it, which is all
// the client needs to see to report a reading.
void test_the_client_subscribes_and_reads_from_a_server() {
    net::UdpSocket server;
    std::string error;
    // Any free port -- the client is pointed at it rather than at the
    // protocol's usual one, so this never collides with a real server that
    // happens to be running on the machine doing the testing.
    uint16_t port = 0;
    for (uint16_t candidate = 47830; candidate < 47850; ++candidate) {
        if (server.start(candidate, false, &error)) {
            port = candidate;
            break;
        }
    }
    if (port == 0) {
        test::skip("no free UDP port for a stand-in DSU server");
        return;
    }

    const std::string server_address = "127.0.0.1:" + std::to_string(port);
    ::setenv("DUALSCREEN_DSU_SERVER", server_address.c_str(), 1);
    CHECK(DsuClient::configured_server() == server_address);

    DsuClient client;
    CHECK(client.start(&error));
    CHECK(!client.connected());

    // The client asks on its first poll.
    client.poll();

    std::vector<uint8_t> request;
    std::string from_address;
    uint16_t from_port = 0;
    bool subscribed = false;
    for (int attempt = 0; attempt < 100 && !subscribed; ++attempt) {
        while (server.receive(request, from_address, from_port)) {
            if (request.size() == kDsuRequestBytes && request[3] == 'C') {
                subscribed = true;
            }
        }
        if (!subscribed) SDL_Delay(10);
    }
    CHECK(subscribed);
    if (!subscribed) return;

    // Answer it the way a real one would, and the reading should arrive.
    const auto packet = data_packet(0.0f, 0.0f, -1.0f, 10.0f, 20.0f, 30.0f);
    CHECK(server.send_to(from_address, from_port, packet.data(), packet.size()));

    bool connected = false;
    for (int attempt = 0; attempt < 100 && !connected; ++attempt) {
        client.poll();
        connected = client.connected();
        if (!connected) SDL_Delay(10);
    }
    CHECK(connected);
    if (!connected) return;

    CHECK(client.state().present);
    CHECK(test::nearly_equal(client.state().gyro_x, 10.0f * kDegreesToRadians));
    CHECK(test::nearly_equal(client.state().accel_z, 1.0f * kGravity));

    ::unsetenv("DUALSCREEN_DSU_SERVER");
}

void test_it_can_be_turned_off() {
    ::setenv("DUALSCREEN_DSU_SERVER", "off", 1);
    DsuClient client;
    std::string error;
    CHECK(!client.start(&error));
    CHECK(!client.started());
    CHECK(!error.empty());
    ::unsetenv("DUALSCREEN_DSU_SERVER");
}

}  // namespace

int main() {
    test_a_reading_comes_through_in_sdl_units_and_axes();
    test_the_conversion_agrees_with_what_cemu_would_have_done_itself();
    test_packets_that_are_not_a_reading_are_refused();
    test_the_subscription_is_something_a_server_would_accept();
    test_the_client_subscribes_and_reads_from_a_server();
    test_it_can_be_turned_off();
    return test::finish("dsu");
}
