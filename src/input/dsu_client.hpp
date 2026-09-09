#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "input/ds_input.hpp"
#include "net/transport.hpp"

namespace ds {

// Motion from a DSU (cemuhook) server, for the devices that have sensors but
// will not let this app read them.
//
// A Steam Deck is the case this exists for. It has a gyroscope and an
// accelerometer, but Steam Input re-presents the hardware as a virtual pad
// with the sensors stripped off, so SDL sees a controller with no motion at
// all -- which is why MotionReader's first two sources both come back empty
// there however the pad is configured. The usual answer on that machine is
// SteamDeckGyroDSU, which reads the real device and re-publishes it over DSU:
// a small UDP protocol that emulators already speak (Cemu, Citra, Dolphin,
// yuzu) precisely because so many devices need something like this.
//
// So rather than reaching for the hardware behind Steam's back -- which needs
// hidraw permissions, a udev rule and a device-specific report parser, all of
// it Deck-only -- this speaks the protocol those tools already publish on.
// Anything else that serves DSU works too, including a phone standing in as a
// motion source, which is the reason the address is configurable at all.
//
// Nothing here integrates anything, the same as MotionReader: what comes out
// is a raw reading in SDL's units and axes. See DsMotionState.
class DsuClient {
public:
    // Where to look. Defaults to the loopback address and the port the
    // protocol conventionally uses, which is what SteamDeckGyroDSU listens on
    // out of the box. DUALSCREEN_DSU_SERVER overrides it -- "host",
    // "host:port", or "off" to not look at all.
    static std::string configured_server();

    // Opens the socket and starts asking. Cheap and quiet: if nothing is
    // listening this sends a small datagram once a second to a port on this
    // machine that nobody has open, and reports no motion forever.
    bool start(std::string* error);
    void stop();
    bool started() const { return socket_.open(); }

    // Whether a server has actually answered recently with a controller that
    // has motion to report. False until the first packet arrives, and false
    // again if the server goes away mid-session.
    bool connected() const { return connected_; }

    // Drains whatever has arrived and re-subscribes when due. Cheap enough to
    // call once a frame, which is how MotionReader calls it.
    void poll();

    // The newest reading, or absent if none has arrived.
    const DsMotionState& state() const { return state_; }

private:
    void send_subscribe();

    net::UdpSocket socket_;
    std::string address_;
    uint16_t port_ = 0;
    // Constant for the life of this client, which is what the protocol asks
    // for -- a server keys its subscriber list on it.
    uint32_t client_id_ = 0;
    uint64_t subscribed_at_ms_ = 0;
    uint64_t heard_at_ms_ = 0;
    // When this first asked anything of the server, so a server that never
    // answers can be told from one that answered and stopped. Separate from
    // subscribed_at_ms_, which moves every time the subscription is renewed.
    uint64_t first_asked_at_ms_ = 0;
    bool said_never_answered_ = false;
    bool connected_ = false;
    bool said_connected_ = false;
    DsMotionState state_;
};

// Reads one DSU controller-data packet into a motion reading, converting as
// it goes. Separate from the client above because it is the part worth
// testing on its own: no socket, no timing, just the bytes a server sends and
// the reading they mean.
//
// False for anything that is not a well-formed data packet from a connected
// controller that actually has motion -- a version or info answer, a slot
// reporting no gyro, a truncated datagram, a bad checksum.
bool parse_dsu_motion(const uint8_t* data, std::size_t size, DsMotionState& out);

// The protocol's own checksum: CRC-32 over the whole packet with the checksum
// field itself zeroed. Exposed so a test can build a packet a server would
// accept rather than only one this client happens to.
uint32_t dsu_crc32(const uint8_t* data, std::size_t size);

// Builds the "send me controller data" request, the only thing this client
// ever says. `out` must have room for kDsuRequestBytes.
inline constexpr std::size_t kDsuRequestBytes = 28;
void build_dsu_subscribe(uint32_t client_id, uint8_t* out);

}  // namespace ds
