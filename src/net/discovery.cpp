#include "net/discovery.hpp"

#include <algorithm>

#include "util/log.hpp"

namespace ds::net {

// --- HostAnnouncer ----------------------------------------------------------

bool HostAnnouncer::start(std::string* error) {
    // Send-only: the announcer never needs to receive anything, so it does not
    // bind the discovery port and cannot collide with a browser on the same
    // machine.
    if (!socket_.start(0, true, error)) {
        return false;
    }
    last_sent_ms_ = 0;
    dirty_ = true;

    std::string where;
    for (const std::string& address : broadcast_addresses()) {
        if (!where.empty()) where += ", ";
        where += address;
    }
    log_info("announcing this host on " + where);

    const std::string shared = shared_network_warning(local_networks());
    if (!shared.empty()) {
        log_warn(shared);
    }
    return true;
}

void HostAnnouncer::stop() { socket_.stop(); }

void HostAnnouncer::set_beacon(const BeaconMessage& beacon) {
    beacon_ = beacon;
    dirty_ = true;
}

void HostAnnouncer::poll(uint64_t now_ms) {
    if (!socket_.open()) {
        return;
    }
    if (last_sent_ms_ != 0 && now_ms - last_sent_ms_ < kBeaconIntervalMs) {
        return;
    }
    if (dirty_ || encoded_.empty()) {
        encode_beacon(beacon_, encoded_);
        dirty_ = false;
    }
    socket_.broadcast(kDiscoveryPort, encoded_.data(), encoded_.size());
    last_sent_ms_ = now_ms;
}

// --- HostBrowser ------------------------------------------------------------

bool HostBrowser::start(std::string* error) {
    hosts_.clear();
    return socket_.start(kDiscoveryPort, true, error);
}

void HostBrowser::stop() {
    socket_.stop();
    hosts_.clear();
}

void HostBrowser::ingest(const BeaconMessage& beacon, const std::string& address, uint64_t now_ms) {
    // A version we do not speak is not shown at all: connecting would only fail
    // later, with a worse message.
    if (beacon.protocol_version != kProtocolVersion) {
        return;
    }

    // Identity is address plus port, so two instances on one machine -- which is
    // exactly what the tests do -- stay distinct.
    for (DiscoveredHost& host : hosts_) {
        if (host.address == address && host.session_port == beacon.session_port) {
            host.device_name = beacon.device_name;
            host.game_title = beacon.game_title;
            host.rom_sha1 = beacon.rom_sha1;
            host.top_available = beacon.top_available;
            host.bottom_available = beacon.bottom_available;
            host.last_seen_ms = now_ms;
            return;
        }
    }

    DiscoveredHost host;
    host.address = address;
    host.session_port = beacon.session_port;
    host.device_name = beacon.device_name;
    host.game_title = beacon.game_title;
    host.rom_sha1 = beacon.rom_sha1;
    host.top_available = beacon.top_available;
    host.bottom_available = beacon.bottom_available;
    host.last_seen_ms = now_ms;
    hosts_.push_back(host);
    log_info("found host " + host.device_name + " at " + host.address + " running " +
             host.game_title);
}

void HostBrowser::expire(uint64_t now_ms) {
    hosts_.erase(std::remove_if(hosts_.begin(), hosts_.end(),
                                [now_ms](const DiscoveredHost& host) {
                                    return now_ms > host.last_seen_ms &&
                                           now_ms - host.last_seen_ms > kHostExpiryMs;
                                }),
                 hosts_.end());
}

void HostBrowser::poll(uint64_t now_ms) {
    if (!socket_.open()) {
        return;
    }
    std::vector<uint8_t> datagram;
    std::string from_address;
    while (socket_.receive(datagram, from_address)) {
        BeaconMessage beacon;
        if (decode_beacon(datagram.data(), datagram.size(), beacon)) {
            ingest(beacon, from_address, now_ms);
        }
    }
    expire(now_ms);
}

}  // namespace ds::net
