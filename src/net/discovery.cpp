#include "net/discovery.hpp"

#include <algorithm>

#include "util/log.hpp"

namespace ds::net {

// --- HostAnnouncer ----------------------------------------------------------

bool HostAnnouncer::start(std::string* error) {
    // Send-only: the announcer never needs to receive anything, so it does not
    // bind the discovery port and cannot collide with a browser on the same
    // machine.
    stop();
    if (!socket_.start(0, true, error)) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        dirty_ = true;
        stopping_ = false;
    }

    std::string where;
    for (const LocalNetwork& network : announce_networks()) {
        if (!where.empty()) where += ", ";
        where += network.broadcast;
        if (!network.interface.empty()) where += " from " + network.interface;
    }
    log_info("announcing this host on " + where);

    const std::string shared = shared_network_warning(local_networks());
    if (!shared.empty()) {
        log_warn(shared);
    }
    worker_ = std::thread([this] { run(); });
    return true;
}

HostAnnouncer::~HostAnnouncer() { stop(); }

void HostAnnouncer::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    if (worker_.joinable()) worker_.join();
    socket_.stop();
}

void HostAnnouncer::set_beacon(const BeaconMessage& beacon) {
    std::lock_guard<std::mutex> lock(mutex_);
    beacon_ = beacon;
    dirty_ = true;
}

void HostAnnouncer::set_icon(std::vector<std::vector<uint8_t>> pieces) {
    std::lock_guard<std::mutex> lock(mutex_);
    icon_pieces_ = std::move(pieces);
}

void HostAnnouncer::run() {
    std::unique_lock<std::mutex> lock(mutex_);
    int round = 0;
    while (!stopping_) {
        if (dirty_ || encoded_.empty()) {
            encode_beacon(beacon_, encoded_);
            dirty_ = false;
        }
        const std::vector<uint8_t> beacon = encoded_;
        std::vector<std::vector<uint8_t>> icon;
        if (round++ % kBeaconIconEvery == 0) icon = icon_pieces_;
        // Sent without the lock: however long the network takes, the game's
        // loop handing over a new beacon never waits for it.
        lock.unlock();
        socket_.broadcast(kDiscoveryPort, beacon.data(), beacon.size());
        for (const std::vector<uint8_t>& piece : icon) {
            socket_.broadcast(kDiscoveryPort, piece.data(), piece.size());
        }
        lock.lock();
        wake_.wait_for(lock, std::chrono::milliseconds(kBeaconIntervalMs),
                       [this] { return stopping_; });
    }
}

// --- HostBrowser ------------------------------------------------------------

bool HostBrowser::start(std::string* error) {
    hosts_.clear();
    icons_.clear();
    return socket_.start(kDiscoveryPort, true, error);
}

void HostBrowser::stop() {
    socket_.stop();
    hosts_.clear();
    icons_.clear();
}

void HostBrowser::give_icon(DiscoveredHost& host, const IconAssembly& icon) {
    if (!icon.complete || host.icon_id == icon.icon_id) return;
    host.icon_id = icon.icon_id;
    host.icon_width = icon.width;
    host.icon_height = icon.height;
    host.icon_rgba = icon.rgba;
}

void HostBrowser::ingest_icon(const BeaconIconPiece& piece,
                              const std::string& address,
                              uint64_t now_ms) {
    if (piece.protocol_version != kProtocolVersion) return;
    IconAssembly* icon = nullptr;
    for (IconAssembly& candidate : icons_) {
        if (candidate.address == address && candidate.session_port == piece.session_port) {
            icon = &candidate;
            break;
        }
    }
    if (icon == nullptr) {
        icons_.emplace_back();
        icon = &icons_.back();
        icon->address = address;
        icon->session_port = piece.session_port;
    }
    icon->last_seen_ms = now_ms;
    if (icon->icon_id != piece.icon_id || icon->width != piece.width ||
        icon->height != piece.height || icon->total_bytes != piece.total_bytes ||
        icon->pieces.size() != piece.count) {
        // A different icon from the one being put together: start over.
        icon->icon_id = piece.icon_id;
        icon->width = piece.width;
        icon->height = piece.height;
        icon->total_bytes = piece.total_bytes;
        icon->pieces.assign(piece.count, {});
        icon->received.assign(piece.count, false);
        icon->complete = false;
        icon->rgba.clear();
    }
    if (icon->complete || icon->received[piece.index]) return;
    icon->pieces[piece.index] = piece.data;
    icon->received[piece.index] = true;
    if (std::find(icon->received.begin(), icon->received.end(), false) != icon->received.end()) {
        return;
    }
    std::vector<uint8_t> deflated;
    for (const std::vector<uint8_t>& part : icon->pieces) {
        deflated.insert(deflated.end(), part.begin(), part.end());
    }
    icon->pieces.assign(icon->pieces.size(), {});
    if (deflated.size() != icon->total_bytes ||
        !inflate_beacon_icon(deflated, icon->width, icon->height, icon->rgba)) {
        // Wait for the next round to go through it all again.
        icon->received.assign(icon->received.size(), false);
        return;
    }
    icon->complete = true;
    for (DiscoveredHost& host : hosts_) {
        if (host.address == address && host.session_port == piece.session_port) {
            give_icon(host, *icon);
        }
    }
}

void HostBrowser::ingest(const BeaconMessage& beacon, const std::string& address, uint64_t now_ms) {
    // A version we do not speak is listed all the same, as needing an update:
    // "the other device never shows up" is otherwise all the player sees of two
    // builds that no longer match. offers() keeps it from being joined.
    if (beacon.protocol_version != kProtocolVersion &&
        std::find(other_versions_.begin(), other_versions_.end(), address) == other_versions_.end()) {
        other_versions_.push_back(address);
        log_warn("heard " + beacon.device_name + " at " + address + " hosting " + beacon.game_title +
                 ", but it speaks version " + std::to_string(beacon.protocol_version) +
                 " of the protocol and this speaks " + std::to_string(kProtocolVersion) +
                 " -- listed as needing an update; update both to the same build");
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
            host.protocol_version = beacon.protocol_version;
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
    host.protocol_version = beacon.protocol_version;
    host.last_seen_ms = now_ms;
    for (const IconAssembly& icon : icons_) {
        if (icon.address == address && icon.session_port == beacon.session_port) {
            give_icon(host, icon);
        }
    }
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
    icons_.erase(std::remove_if(icons_.begin(), icons_.end(),
                                [now_ms](const IconAssembly& icon) {
                                    return now_ms > icon.last_seen_ms &&
                                           now_ms - icon.last_seen_ms > kHostExpiryMs;
                                }),
                 icons_.end());
}

void HostBrowser::poll(uint64_t now_ms) {
    if (!socket_.open()) {
        return;
    }
    std::vector<uint8_t> datagram;
    std::string from_address;
    while (socket_.receive(datagram, from_address)) {
        BeaconMessage beacon;
        BeaconIconPiece piece;
        if (decode_beacon(datagram.data(), datagram.size(), beacon)) {
            ingest(beacon, from_address, now_ms);
        } else if (decode_beacon_icon_piece(datagram.data(), datagram.size(), piece)) {
            ingest_icon(piece, from_address, now_ms);
        }
    }
    expire(now_ms);
}

}  // namespace ds::net
