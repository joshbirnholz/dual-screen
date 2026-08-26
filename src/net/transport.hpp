#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ds::net {

// Thin non-blocking BSD-socket wrappers. Everything here is polled from the
// app's frame loop -- no threads, because a dropped frame is better than a
// race, and the loop already runs 60 times a second.

class TcpStream {
public:
    TcpStream() = default;
    ~TcpStream();
    TcpStream(TcpStream&& other) noexcept;
    TcpStream& operator=(TcpStream&& other) noexcept;
    TcpStream(const TcpStream&) = delete;
    TcpStream& operator=(const TcpStream&) = delete;

    // Starts a non-blocking connect. `connected()` stays false until pump()
    // reports the handshake finished.
    bool start_connect(const std::string& address, uint16_t port, std::string* error);

    bool open() const { return descriptor_ >= 0; }
    bool connected() const { return descriptor_ >= 0 && !connecting_; }
    void close();

    // Queues bytes for sending. They go out during pump().
    void send(const uint8_t* data, std::size_t size);
    void send(const std::vector<uint8_t>& data) { send(data.data(), data.size()); }
    // What is still waiting in this process.
    std::size_t pending_bytes() const { return outbox_.size() - outbox_sent_; }

    // Everything ever handed to send(), and how much of it has actually left
    // the machine. The difference is the whole backlog -- this process's queue
    // *and* the kernel's, which is otherwise invisible and, on a link that
    // cannot keep up, is where most of the delay accumulates.
    uint64_t queued_total() const { return queued_total_; }
    uint64_t departed_total() const { return queued_total_ - backlog_bytes(); }
    std::size_t backlog_bytes() const;

    // Moves queued writes out and available reads into `inbox`. Returns false
    // once the connection is finished, with the reason in `error`.
    bool pump(std::vector<uint8_t>& inbox, std::string* error);

    const std::string& peer_address() const { return peer_address_; }
    // Which of this machine's addresses the routing table actually chose for
    // this connection. On a machine with more than one way onto the same
    // network -- a desktop with its ethernet and its wifi both up on one
    // subnet -- that is not something anyone can tell by looking, and it
    // decides whether the session is on the wire or on the air.
    std::string local_address() const;

    // Takes ownership of an accepted descriptor.
    static TcpStream adopt(int descriptor, std::string peer_address);

    // The raw descriptor, for a test to inspect socket options directly.
    int descriptor_for_test() const { return descriptor_; }

private:
    int descriptor_ = -1;
    bool connecting_ = false;
    std::string peer_address_;
    std::vector<uint8_t> outbox_;
    std::size_t outbox_sent_ = 0;
    uint64_t queued_total_ = 0;
};

class TcpListener {
public:
    ~TcpListener();

    bool start(uint16_t port, std::string* error);
    void stop();
    bool listening() const { return descriptor_ >= 0; }
    uint16_t port() const { return port_; }

    // Returns true and fills `out` when someone connected; false otherwise.
    bool accept(TcpStream& out);

private:
    int descriptor_ = -1;
    uint16_t port_ = 0;
};

// UDP, used only for the discovery beacon.
class UdpSocket {
public:
    ~UdpSocket();

    // A socket that can broadcast. `listen_port` of 0 means send-only.
    bool start(uint16_t listen_port, bool allow_broadcast, std::string* error);
    void stop();
    bool open() const { return descriptor_ >= 0; }

    bool broadcast(uint16_t port, const uint8_t* data, std::size_t size);
    // Returns true once per waiting datagram.
    bool receive(std::vector<uint8_t>& out, std::string& from_address);

private:
    int descriptor_ = -1;
};

// One of the IPv4 networks this machine is on: the interface that reaches it,
// the address this machine has there, and the broadcast address for it.
struct LocalNetwork {
    std::string interface;
    std::string address;
    std::string broadcast;
};

// Best-effort list of the networks this machine is on.
std::vector<LocalNetwork> local_networks();

// Best-effort list of this machine's broadcast addresses, so a beacon reaches
// every subnet the device is on rather than only the default route. One entry
// per network: a machine that is on one network twice still announces once.
std::vector<std::string> broadcast_addresses(const std::vector<LocalNetwork>& networks);
std::vector<std::string> broadcast_addresses();

// Names the interfaces whenever this machine is on one network through more
// than one of them, and is empty otherwise. Worth saying out loud: a companion
// reaches such a machine by whichever interface answered first, the replies can
// leave by the other one, and neither choice is anyone's.
std::string shared_network_warning(const std::vector<LocalNetwork>& networks);

// Monotonic milliseconds, for timeouts that must not jump when the clock does.
uint64_t monotonic_millis();

}  // namespace ds::net
