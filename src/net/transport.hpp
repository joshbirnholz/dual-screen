#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ds::net {

// Thin non-blocking BSD-socket wrappers. Everything here is polled from the
// app's frame loop -- no threads, because a dropped frame is better than a
// race, and the loop already runs 60 times a second.

// Marks a socket's traffic as a live, interactive picture rather than bulk
// transfer, so wifi's own queueing treats it that way. See the definition:
// this is what Moonlight sets on its video socket and what Sunshine's QoS
// option does, and it is the difference between a stream that waits for the
// next beacon and one that does not.
void mark_as_interactive_media(int descriptor);

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

    // Why the last connect failed, as an errno, or 0 if it has not. Kept
    // across close(), so a caller told of the failure by pump() can still ask
    // what kind it was. See connect_is_undecided().
    int connect_error() const { return connect_error_; }

    // The raw descriptor, for a test to inspect socket options directly.
    int descriptor_for_test() const { return descriptor_; }

private:
    int descriptor_ = -1;
    bool connecting_ = false;
    int connect_error_ = 0;
    std::string peer_address_;
    std::vector<uint8_t> outbox_;
    std::size_t outbox_sent_ = 0;
    uint64_t queued_total_ = 0;
};

// The errnos a connect fails with when the machine it is on has a permission
// gate in front of the local network and has not opened it yet. Says nothing
// about whether this machine has one -- see connect_is_undecided() for that --
// so that what the codes mean can be checked anywhere rather than only on the
// platform that produces them.
//
// Deliberately narrow. A refused connection is a host that is really not
// listening and a timed-out one is a host that is really not there; asking
// either of those again only makes the player wait longer to be told.
bool connect_error_looks_like_a_permission_gate(int code);

// Whether a connect failure is one the operating system may answer differently
// a moment later, rather than a fact about the network.
//
// macOS decides whether an app may talk to the local network the first time it
// actually tries to, and that decision does not arrive in time for the attempt
// that asked for it: the first connect comes back "No route to host" even for
// an app the player allowed long ago. The next one, a moment later, goes
// through -- which is why joining a host used to take two tries there. On
// every other platform this errno means what it says and there is nothing to
// wait for.
bool connect_is_undecided(int code);

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

// UDP. Originally just the discovery beacon's broadcast/listen; now also the
// point-to-point media channel (video, audio, input, frame-acks) -- see
// send_to() below.
// One of the IPv4 networks this machine is on: the interface that reaches it,
// the address this machine has there, and the broadcast address for it.
struct LocalNetwork {
    std::string interface;
    std::string address;
    std::string broadcast;
    bool wireless = false;
};

class UdpSocket {
public:
    ~UdpSocket();

    // A socket that can broadcast. `listen_port` of 0 means send-only.
    bool start(uint16_t listen_port, bool allow_broadcast, std::string* error);
    void stop();
    bool open() const { return descriptor_ >= 0; }

    bool broadcast(uint16_t port, const uint8_t* data, std::size_t size);
    // One datagram to one address, unlike broadcast() above -- the media
    // channel's own send, once the far end's address and port are known
    // (learned from a received datagram; see receive() below).
    bool send_to(const std::string& address, uint16_t port, const uint8_t* data,
                std::size_t size);
    // Pins which of this machine's own addresses these datagrams leave from,
    // and with it which interface carries them.
    //
    // Needed because a machine on one network twice -- ethernet and wifi on
    // the same subnet, which is an ordinary desktop setup -- has two equally
    // valid routes to the far end, and an unbound socket takes whichever the
    // routing table currently prefers. That can be the wireless one even when
    // the session's own TCP connection is on the wire, and it can change
    // underneath a running session. Setting this to the address the far end
    // actually connected to keeps video and audio on the same interface as
    // the connection they belong to. An address this machine does not have,
    // or an empty one, goes back to letting the routing table decide.
    void set_source_address(const std::string& address);
    // Returns true once per waiting datagram.
    bool receive(std::vector<uint8_t>& out, std::string& from_address);
    // The same, but also reporting the sender's port -- what send_to() above
    // needs to answer a datagram whose sender is not listening on a fixed,
    // well-known port (an ephemeral one, like a companion's media socket).
    bool receive(std::vector<uint8_t>& out, std::string& from_address, uint16_t& from_port);
    // Waits up to `timeout_ms` for a datagram to be waiting, for a thread that
    // does nothing but read this socket. True when one is; false on the
    // timeout, and at once for a socket that is not open.
    bool wait_readable(int timeout_ms) const;

private:
    int descriptor_ = -1;
    // Empty until set_source_address() finds a real interface for it.
    LocalNetwork source_;
};

// Best-effort list of the networks this machine is on, as the kernel
// reports them.
std::vector<LocalNetwork> local_networks();

// The networks to announce on: one entry per network, so a machine that is on
// one network twice announces once, and the wired way onto it is the one it
// gives out. Never empty -- with nothing to go on it falls back to the
// all-subnets address and lets the routing table choose.
std::vector<LocalNetwork> announce_networks(const std::vector<LocalNetwork>& networks);
std::vector<LocalNetwork> announce_networks();

// Names the interfaces whenever this machine is on one network through more
// than one of them, and is empty otherwise. Worth saying out loud: companions
// are pointed at the wired one, but the machine still answers on both and the
// replies leave by whichever the routing table picks.
std::string shared_network_warning(const std::vector<LocalNetwork>& networks);

// Monotonic milliseconds, for timeouts that must not jump when the clock does.
uint64_t monotonic_millis();

}  // namespace ds::net
