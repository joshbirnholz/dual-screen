#pragma once

#include <thread>

#include <mutex>

#include <condition_variable>

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
    // The game's icon, RGBA8, once all of its pieces have come in -- see
    // BeaconIconPiece. Empty from a host from before it was sent, or until
    // then. `icon_id` changes whenever the icon does.
    uint32_t icon_id = 0;
    int icon_width = 0;
    int icon_height = 0;
    std::vector<uint8_t> icon_rgba;
    // The protocol it speaks. Another than this build's is a host that is
    // listed, so that the player sees it, but that cannot be joined until
    // one device or the other is updated -- see needs_update().
    uint16_t protocol_version = kProtocolVersion;

    // Whether the two builds no longer match, and which is behind.
    bool needs_update() const { return protocol_version != kProtocolVersion; }
    bool is_older() const { return protocol_version < kProtocolVersion; }

    // A host that needs an update offers nothing: connecting would only fail.
    bool offers(ScreenId screen) const {
        if (needs_update()) return false;
        return screen == ScreenId::Top ? top_available : bottom_available;
    }
};

// Broadcast while a game is running so companions can find this device without
// anyone typing an address.
//
// On a thread of its own, every kBeaconIntervalMs: finding which networks to
// announce on asks the system, and on a Switch waking from sleep, with its
// Wi-Fi still coming back, that and the send could each take a second or
// more -- on the game's own loop, once a beacon, which played the game at a
// fifth of its speed until the network settled.
class HostAnnouncer {
public:
    ~HostAnnouncer();
    bool start(std::string* error);
    void stop();
    bool running() const { return socket_.open(); }

    // Cheap to call every frame: it only hands the beacon over.
    void set_beacon(const BeaconMessage& beacon);
    // The game's icon, as encode_beacon_icon() cut it up; sent beside the
    // beacon, every kBeaconIconEvery beacons.
    void set_icon(std::vector<std::vector<uint8_t>> pieces);
    // Nothing to do any more -- the thread sends -- and kept so callers need
    // not change.
    void poll(uint64_t now_ms) { (void)now_ms; }

private:
    void run();

    UdpSocket socket_;
    std::mutex mutex_;
    std::condition_variable wake_;
    bool stopping_ = false;
    BeaconMessage beacon_;
    std::vector<uint8_t> encoded_;
    bool dirty_ = true;
    std::vector<std::vector<uint8_t>> icon_pieces_;
    std::thread worker_;
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
    void ingest_icon(const BeaconIconPiece& piece, const std::string& address, uint64_t now_ms);
    void expire(uint64_t now_ms);

private:
    // One host's icon as its pieces come in, and once they all have.
    struct IconAssembly {
        std::string address;
        uint16_t session_port = 0;
        uint32_t icon_id = 0;
        int width = 0;
        int height = 0;
        uint32_t total_bytes = 0;
        std::vector<std::vector<uint8_t>> pieces;
        std::vector<bool> received;
        bool complete = false;
        std::vector<uint8_t> rgba;
        uint64_t last_seen_ms = 0;
    };
    static void give_icon(DiscoveredHost& host, const IconAssembly& icon);

    UdpSocket socket_;
    std::vector<DiscoveredHost> hosts_;
    std::vector<IconAssembly> icons_;
    // Hosts heard speaking another protocol version, said once each in the
    // log beside their row's "Needs Update".
    std::vector<std::string> other_versions_;
};

// How often a host re-announces itself, and how long a companion keeps showing
// a host it has stopped hearing from. The gap between them means a single
// dropped datagram never makes a host flicker out of the list.
constexpr uint64_t kBeaconIntervalMs = 1000;
constexpr uint64_t kHostExpiryMs = 5000;
// The icon goes with the first beacon and every this many after it: a picker
// opened mid-game has it in a moment or two, and the network is not carrying
// the same few kilobytes every second for the whole of a session.
constexpr int kBeaconIconEvery = 2;

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
