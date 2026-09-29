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
    // Hosts heard speaking another protocol version, said once each: they are
    // not listed, and "the other device never shows up" is otherwise all the
    // player sees of two builds that no longer match.
    std::vector<std::string> other_versions_;
};

// How often a host re-announces itself, and how long a companion keeps showing
// a host it has stopped hearing from. The gap between them means a single
// dropped datagram never makes a host flicker out of the list.
constexpr uint64_t kBeaconIntervalMs = 1000;
constexpr uint64_t kHostExpiryMs = 5000;

// How long joining a host is given before it is called off. Generous against a
// LAN handshake, which is a few round trips, and short enough that picking a
// host that cannot answer costs a moment rather than the two minutes the
// operating system would otherwise spend retrying.
constexpr uint64_t kJoinTimeoutMs = 6000;

// How long to wait before trying a connection again, when the last attempt
// failed with something the operating system may answer differently in a
// moment -- see connect_is_undecided(). Long enough not to spin, short enough
// that a good few attempts fit inside the join deadline above, so the player
// sees one connection that took a beat rather than one that had to be asked
// for twice.
constexpr uint64_t kConnectRetryMs = 250;

// And how long a join is given when the attempts are being refused by this
// machine's own local-network permission rather than by anything on the
// network. Far longer than kJoinTimeoutMs, because what it may be waiting for
// is a person noticing a system prompt and answering it -- six seconds is not
// enough for that, and giving up early is what makes the first try look
// broken and the second look fine.
constexpr uint64_t kLocalNetworkJoinTimeoutMs = 30000;

}  // namespace ds::net
