#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "net/protocol.hpp"
#include "net/transport.hpp"

namespace ds::net {

// A host seen on the network, as the companion's picker shows it.
struct DiscoveredHost {
    std::string address;
    uint16_t session_port = kSessionPort;
    std::string device_name;
    std::string game_title;
    std::string rom_sha1;
    bool top_available = true;
    bool bottom_available = true;
    uint64_t last_seen_ms = 0;

    bool offers(ScreenId screen) const {
        return screen == ScreenId::Top ? top_available : bottom_available;
    }
};

// Broadcast while a game is running so companions can find this device without
// anyone typing an address.
class HostAnnouncer {
public:
    bool start(std::string* error);
    void stop();
    bool running() const { return socket_.open(); }

    // Cheap to call every frame; the beacon only goes out on its own interval.
    void set_beacon(const BeaconMessage& beacon);
    void poll(uint64_t now_ms);

private:
    UdpSocket socket_;
    BeaconMessage beacon_;
    std::vector<uint8_t> encoded_;
    uint64_t last_sent_ms_ = 0;
    bool dirty_ = true;
};

// Listens for beacons and keeps a list of hosts, dropping ones that go quiet.
class HostBrowser {
public:
    bool start(std::string* error);
    void stop();
    bool running() const { return socket_.open(); }

    void poll(uint64_t now_ms);
    const std::vector<DiscoveredHost>& hosts() const { return hosts_; }

    // Exposed so the merge and expiry rules can be tested without sockets.
    void ingest(const BeaconMessage& beacon, const std::string& address, uint64_t now_ms);
    void expire(uint64_t now_ms);

private:
    UdpSocket socket_;
    std::vector<DiscoveredHost> hosts_;
};

// How often a host re-announces itself, and how long a companion keeps showing
// a host it has stopped hearing from. The gap between them means a single
// dropped datagram never makes a host flicker out of the list.
constexpr uint64_t kBeaconIntervalMs = 1000;
constexpr uint64_t kHostExpiryMs = 5000;

}  // namespace ds::net
