#include "input/dsu_client.hpp"

#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

#include "util/log.hpp"

namespace ds {
namespace {

// The protocol's own numbers. See cemuhook's DSU spec, and kmicki's
// SteamDeckGyroDSU which is the server this was written against.
constexpr uint16_t kDsuVersion = 1001;
constexpr uint32_t kEventControllerData = 0x100002u;
// Every packet starts with the same twenty bytes: four of magic, the version,
// the length of everything after this field, the checksum, the sender's id,
// and the event type. Only the last of those is counted in the length, which
// is why it reads as 16 rather than 20 below.
constexpr std::size_t kHeaderBytes = 20;
constexpr std::size_t kUncountedHeaderBytes = 16;
// A controller-data packet is exactly this long, and the two readings sit at
// the end of it.
constexpr std::size_t kDataPacketBytes = 100;
constexpr std::size_t kTimestampOffset = 68;
constexpr std::size_t kAccelOffset = 76;
constexpr uint8_t kSlotStateConnected = 2;
constexpr uint8_t kDeviceModelFullGyro = 2;

constexpr uint16_t kDefaultPort = 26760;
constexpr char kDefaultAddress[] = "127.0.0.1";

// A server drops a subscriber it has not heard from in a few seconds, so this
// is a keepalive as much as a subscription -- there is no "unsubscribe" in
// the protocol, and going quiet is how a client leaves.
constexpr uint64_t kSubscribeIntervalMs = 1000;
// And the mirror of it: a server that has said nothing for this long is gone,
// whether it stopped or the machine went to sleep with it running.
constexpr uint64_t kServerSilentMs = 2000;
// And how long to keep asking a server that has never said anything at all
// before saying so. Longer than the silence above, because that one is a
// server that went away mid-session and this one is a server that may simply
// not be up yet -- a few seconds of quiet at startup is ordinary.
constexpr uint64_t kNeverAnsweredMs = 5000;

// DSU reports rotation in degrees a second and acceleration in g; everything
// downstream of DsMotionState is in SDL's radians a second and metres per
// second squared.
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

uint32_t read_u32(const uint8_t* data) {
    return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
           (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
}

uint64_t read_u64(const uint8_t* data) {
    return static_cast<uint64_t>(read_u32(data)) |
           (static_cast<uint64_t>(read_u32(data + 4)) << 32);
}

float read_f32(const uint8_t* data) {
    // Through memcpy rather than a cast: the two have the same bytes and only
    // one of them is defined.
    const uint32_t bits = read_u32(data);
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

}  // namespace

uint32_t dsu_crc32(const uint8_t* data, std::size_t size) {
    // Plain CRC-32, the same polynomial zip and PNG use. Written out rather
    // than taken from zlib because it is eight lines and this is the only
    // thing in the app that wants one.
    uint32_t crc = 0xffffffffu;
    for (std::size_t index = 0; index < size; ++index) {
        crc ^= data[index];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 1u) != 0u ? (crc >> 1) ^ 0xedb88320u : crc >> 1;
        }
    }
    return ~crc;
}

void build_dsu_subscribe(uint32_t client_id, uint8_t* out) {
    std::memset(out, 0, kDsuRequestBytes);
    out[0] = 'D';
    out[1] = 'S';
    out[2] = 'U';
    out[3] = 'C';  // client; a server answers with DSUS
    put_u16(out + 4, kDsuVersion);
    put_u16(out + 6, static_cast<uint16_t>(kDsuRequestBytes - kUncountedHeaderBytes));
    put_u32(out + 8, 0);  // checksum, filled in below over the whole packet
    put_u32(out + 12, client_id);
    put_u32(out + 16, kEventControllerData);
    // The body: which controllers to send. Zero means all of them, which is
    // what a client with one device to find wants -- the slot and MAC that
    // follow are only read when the mask says to, and stay zero.
    out[20] = 0;
    put_u32(out + 8, dsu_crc32(out, kDsuRequestBytes));
}

bool parse_dsu_motion(const uint8_t* data, std::size_t size, DsMotionState& out) {
    if (data == nullptr || size < kDataPacketBytes) {
        return false;
    }
    if (data[0] != 'D' || data[1] != 'S' || data[2] != 'U' || data[3] != 'S') {
        return false;
    }
    if (read_u32(data + 16) != kEventControllerData) {
        return false;
    }
    // The checksum covers the packet with its own field zeroed, so it has to
    // be taken out before checking and is not worth putting back.
    std::vector<uint8_t> copy(data, data + kDataPacketBytes);
    const uint32_t stated = read_u32(copy.data() + 8);
    put_u32(copy.data() + 8, 0);
    if (dsu_crc32(copy.data(), kDataPacketBytes) != stated) {
        return false;
    }

    const uint8_t slot_state = data[21];
    const uint8_t device_model = data[22];
    if (slot_state != kSlotStateConnected || device_model != kDeviceModelFullGyro) {
        // A slot with nothing in it, or a pad the server knows has no gyro.
        // Either way there is no reading here, and reporting zeroes as though
        // there were would read downstream as a device lying perfectly still.
        return false;
    }

    const uint8_t* motion = data + kAccelOffset;
    const float accel_x = read_f32(motion);
    const float accel_y = read_f32(motion + 4);
    const float accel_z = read_f32(motion + 8);
    const float pitch = read_f32(motion + 12);
    const float yaw = read_f32(motion + 16);
    const float roll = read_f32(motion + 20);

    // Into the convention DsMotionState carries, which is SDL's -- see the
    // class comment there. The signs are not a guess: Cemu consumes both of
    // these formats already, and the transform below is the difference
    // between its two readers (DSUControllerProvider, which negates the
    // second and third acceleration axes, and SDLControllerProvider, which
    // negates the first). Composing them the wrong way round is the sort of
    // mistake that shows as a game tilting the correct amount the wrong way,
    // so it is worth writing out: this turns a DSU reading into the SDL
    // reading that would have produced the same motion.
    out.gyro_x = pitch * kDegreesToRadians;
    out.gyro_y = -yaw * kDegreesToRadians;
    out.gyro_z = -roll * kDegreesToRadians;
    out.accel_x = -accel_x * kGravity;
    out.accel_y = -accel_y * kGravity;
    out.accel_z = -accel_z * kGravity;
    // The server's own microsecond clock, which is what DsMotionState wants:
    // the interval between two samples, measured where they were taken.
    out.sampled_us = static_cast<uint32_t>(read_u64(data + kTimestampOffset) & 0xffffffffull);
    out.present = true;
    return true;
}

std::string DsuClient::configured_server() {
    const char* from_environment = std::getenv("DUALSCREEN_DSU_SERVER");
    if (from_environment != nullptr && *from_environment != '\0') {
        return from_environment;
    }
    return std::string(kDefaultAddress) + ":" + std::to_string(kDefaultPort);
}

bool DsuClient::start(std::string* error) {
    stop();

    const std::string server = configured_server();
    if (server == "off" || server == "none") {
        if (error != nullptr) *error = "turned off by DUALSCREEN_DSU_SERVER";
        return false;
    }
    const std::size_t colon = server.rfind(':');
    if (colon == std::string::npos) {
        address_ = server;
        port_ = kDefaultPort;
    } else {
        address_ = server.substr(0, colon);
        port_ = static_cast<uint16_t>(std::atoi(server.c_str() + colon + 1));
        if (port_ == 0) port_ = kDefaultPort;
    }
    if (address_.empty()) {
        address_ = kDefaultAddress;
    }

    // Bound to nothing in particular: the reply comes back to whatever
    // ephemeral port the first send picks up.
    if (!socket_.start(0, false, error)) {
        return false;
    }

    std::random_device entropy;
    client_id_ = static_cast<uint32_t>(entropy());
    if (client_id_ == 0) client_id_ = 1;
    subscribed_at_ms_ = 0;
    heard_at_ms_ = 0;
    first_asked_at_ms_ = 0;
    said_never_answered_ = false;
    connected_ = false;
    said_connected_ = false;
    state_ = DsMotionState();
    return true;
}

void DsuClient::stop() {
    // No goodbye to send: a DSU server forgets a client that stops asking,
    // which is exactly what closing this does.
    socket_.stop();
    connected_ = false;
    said_connected_ = false;
    state_ = DsMotionState();
}

void DsuClient::send_subscribe() {
    uint8_t request[kDsuRequestBytes];
    build_dsu_subscribe(client_id_, request);
    socket_.send_to(address_, port_, request, sizeof(request));
    subscribed_at_ms_ = net::monotonic_millis();
    if (first_asked_at_ms_ == 0) {
        first_asked_at_ms_ = subscribed_at_ms_;
    }
}

void DsuClient::poll() {
    if (!socket_.open()) {
        return;
    }
    const uint64_t now = net::monotonic_millis();
    if (subscribed_at_ms_ == 0 || now - subscribed_at_ms_ >= kSubscribeIntervalMs) {
        send_subscribe();
    }

    // Everything waiting, keeping the last one that parsed. A server sends at
    // the rate its device samples -- a Steam Deck's is far quicker than a
    // frame -- and the older ones in the queue are only of interest to
    // something integrating every sample, which this deliberately is not.
    std::vector<uint8_t> packet;
    std::string from;
    bool got = false;
    while (socket_.receive(packet, from)) {
        DsMotionState reading;
        if (parse_dsu_motion(packet.data(), packet.size(), reading)) {
            state_ = reading;
            got = true;
        }
    }
    if (got) {
        heard_at_ms_ = now;
        connected_ = true;
        if (!said_connected_) {
            said_connected_ = true;
            log_info("motion: reading it from a DSU server at " + address_ + ":" +
                     std::to_string(port_));
        }
        return;
    }
    if (connected_ && now - heard_at_ms_ >= kServerSilentMs) {
        connected_ = false;
        said_connected_ = false;
        state_ = DsMotionState();
        log_info("motion: the DSU server stopped answering");
    }
    // A server that has never answered, as opposed to one that stopped. The
    // check above cannot see this case -- it only fires for a server that was
    // connected once -- so asking into the void produced no line at all, and a
    // log showed the request going out and then simply nothing. On a Steam
    // Deck that is the whole of the motion path, so the difference between
    // "the server is not running" and "something in this app is wrong" was
    // invisible in the one place it needed to be plain.
    if (!connected_ && heard_at_ms_ == 0 && !said_never_answered_ &&
        first_asked_at_ms_ != 0 && now - first_asked_at_ms_ >= kNeverAnsweredMs) {
        said_never_answered_ = true;
        log_warn("motion: nothing has answered at " + address_ + ":" + std::to_string(port_) +
                 " in " + std::to_string(kNeverAnsweredMs / 1000) +
                 " seconds, so this device reports no motion. Is the DSU server running? On a "
                 "Steam Deck that is SteamDeckGyroDSU.");
    }
}

}  // namespace ds
