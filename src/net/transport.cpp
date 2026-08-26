#include "net/transport.hpp"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

// Not every platform has getifaddrs(). The Switch's BSD sockets are a subset
// of the usual ones and libnx has none; Android's Bionic gained it only in API
// 24, and the companion there is built to run further back than that.
//
// Only a host enumerating its own interfaces to broadcast on needs it, and
// both of those builds are companions -- they connect out and listen for
// broadcasts -- so the one function that wants it falls back to the
// all-subnets address rather than this file being forked in two.
//
// Written as what the platform can do rather than which platform it is, so
// raising Android's floor to 24 picks the real thing up on its own.
#if defined(__SWITCH__)
#define DS_GETIFADDRS_AVAILABLE 0
#elif defined(__ANDROID__) && defined(__ANDROID_API__) && __ANDROID_API__ < 24
#define DS_GETIFADDRS_AVAILABLE 0
#else
#define DS_GETIFADDRS_AVAILABLE 1
#endif

#if DS_GETIFADDRS_AVAILABLE
#define DS_HAVE_GETIFADDRS 1
#include <ifaddrs.h>
#include <net/if.h>
#include <sys/stat.h>
#endif

// Saying which interface a beacon leaves by needs both the interface list and
// per-datagram source control. Without either, the routing table chooses.
#if DS_GETIFADDRS_AVAILABLE && defined(IP_PKTINFO)
#define DS_HAVE_SOURCE_CHOICE 1
#else
#define DS_HAVE_SOURCE_CHOICE 0
#endif

#include <algorithm>
#include <cstring>
#include <utility>

#include "util/log.hpp"

namespace ds::net {
namespace {

bool set_non_blocking(int descriptor) {
    const int flags = ::fcntl(descriptor, F_GETFL, 0);
    return flags >= 0 && ::fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) == 0;
}

// Neither side ever sends anything the other doesn't need to reply to, so an
// idle stream is normal, not a sign of trouble -- but a peer that vanished
// without closing (a device that slept, lost Wi-Fi, or was powered off) looks
// exactly the same from here as one still there, since nothing short of a
// probe tells the two apart. Without this, that stream sits "open" forever:
// the host's one companion slot is pinned on a connection that is never
// coming back, so every real reconnect attempt after it completes its own TCP
// handshake and then just waits, since the host never calls accept() while
// the phantom still holds the slot. The OS default idle time before the first
// probe is two hours, which is not useful here, so it is shortened to notice
// within about ten seconds instead.
void enable_keepalive(int descriptor) {
    const int one = 1;
    ::setsockopt(descriptor, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
    // How soon the probes start and how often they repeat is a per-socket
    // option only where the TCP stack exposes one. Where it does not -- the
    // Switch's does not -- keepalive is still on, just on that stack's own
    // schedule, which is the difference between noticing a vanished peer late
    // and never noticing at all.
#if defined(TCP_KEEPIDLE) && defined(TCP_KEEPINTVL) && defined(TCP_KEEPCNT)
    const int idle_seconds = 3;
    const int interval_seconds = 2;
    const int probe_count = 3;
    ::setsockopt(descriptor, IPPROTO_TCP, TCP_KEEPIDLE, &idle_seconds, sizeof(idle_seconds));
    ::setsockopt(descriptor, IPPROTO_TCP, TCP_KEEPINTVL, &interval_seconds, sizeof(interval_seconds));
    ::setsockopt(descriptor, IPPROTO_TCP, TCP_KEEPCNT, &probe_count, sizeof(probe_count));
#endif
}

// The kernel's own send queue, which nothing above this file can see.
//
// That invisibility is the whole point of bounding it. The frame publisher
// drops a frame rather than queueing it when the link is behind, so that a
// companion that cannot keep up sees dropped frames instead of mounting
// latency -- but it decides that from the bytes still sitting in *this*
// process, and bytes handed to the kernel have left. Left to autotune, that
// queue grows into the megabytes: measured on loopback, 3.9 MB -- some
// ninety-five frames of a DS screen -- was accepted before one byte backed up
// where the publisher could see it. Every one of those frames is a frame the
// player is behind, and none of them can be dropped.
//
// Small enough that what hides in it is a frame or two rather than a second
// and a half, and still several times the bandwidth-delay product of a LAN at
// the bitrates this streams at, so a healthy link stays saturated.
constexpr int kSendQueueBytes = 64 * 1024;

void bound_send_queue(int descriptor) {
    ::setsockopt(descriptor, SOL_SOCKET, SO_SNDBUF, &kSendQueueBytes, sizeof(kSendQueueBytes));
}

bool interface_is_wireless(const char* name) {
#if defined(DS_HAVE_GETIFADDRS) && defined(__linux__)
    // The kernel gives every wireless interface this directory and gives no
    // other interface one, which beats guessing from the interface's name.
    struct stat entry {};
    return ::stat((std::string("/sys/class/net/") + name + "/wireless").c_str(), &entry) == 0;
#else
    (void)name;
    return false;
#endif
}

// Sends one datagram out of a named interface with its source address set to
// that interface's, so a machine on one network twice tells companions which
// of its two addresses to come back on. Falls back to letting the routing
// table decide, which is all a platform without the controls can do.
bool send_from(int descriptor, const LocalNetwork& network, const sockaddr_in& to,
               const uint8_t* data, std::size_t size) {
#if DS_HAVE_SOURCE_CHOICE
    in_addr source {};
    const unsigned int index =
        network.interface.empty() ? 0u : ::if_nametoindex(network.interface.c_str());
    if (index != 0 && ::inet_pton(AF_INET, network.address.c_str(), &source) == 1) {
        iovec payload {const_cast<uint8_t*>(data), size};
        alignas(cmsghdr) uint8_t control[CMSG_SPACE(sizeof(in_pktinfo))] = {0};
        msghdr message {};
        message.msg_name = const_cast<sockaddr_in*>(&to);
        message.msg_namelen = sizeof(to);
        message.msg_iov = &payload;
        message.msg_iovlen = 1;
        message.msg_control = control;
        message.msg_controllen = sizeof(control);

        cmsghdr* header = CMSG_FIRSTHDR(&message);
        header->cmsg_level = IPPROTO_IP;
        header->cmsg_type = IP_PKTINFO;
        header->cmsg_len = CMSG_LEN(sizeof(in_pktinfo));
        in_pktinfo info {};
        info.ipi_ifindex = static_cast<int>(index);
        info.ipi_spec_dst = source;
        std::memcpy(CMSG_DATA(header), &info, sizeof(info));

        return ::sendmsg(descriptor, &message, 0) == static_cast<ssize_t>(size);
    }
#endif
    return ::sendto(descriptor, data, size, 0, reinterpret_cast<const sockaddr*>(&to), sizeof(to)) ==
           static_cast<ssize_t>(size);
}

// A wire beats a radio: it is the faster and steadier of the two, and unlike
// the radio it is not sharing its air with the companion at the other end.
std::vector<LocalNetwork> wired_first(std::vector<LocalNetwork> networks) {
    std::stable_sort(networks.begin(), networks.end(),
                     [](const LocalNetwork& first, const LocalNetwork& second) {
                         return !first.wireless && second.wireless;
                     });
    return networks;
}

std::string describe(const sockaddr_in& address) {
    char text[INET_ADDRSTRLEN] = {0};
    ::inet_ntop(AF_INET, &address.sin_addr, text, sizeof(text));
    return std::string(text);
}

}  // namespace

uint64_t monotonic_millis() {
    timespec now {};
    ::clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<uint64_t>(now.tv_sec) * 1000u + static_cast<uint64_t>(now.tv_nsec) / 1000000u;
}

// --- TcpStream --------------------------------------------------------------

TcpStream::~TcpStream() { close(); }

TcpStream::TcpStream(TcpStream&& other) noexcept
    : descriptor_(other.descriptor_),
      connecting_(other.connecting_),
      peer_address_(std::move(other.peer_address_)),
      outbox_(std::move(other.outbox_)),
      outbox_sent_(other.outbox_sent_) {
    other.descriptor_ = -1;
    other.outbox_sent_ = 0;
}

TcpStream& TcpStream::operator=(TcpStream&& other) noexcept {
    if (this != &other) {
        close();
        descriptor_ = other.descriptor_;
        connecting_ = other.connecting_;
        peer_address_ = std::move(other.peer_address_);
        outbox_ = std::move(other.outbox_);
        outbox_sent_ = other.outbox_sent_;
        other.descriptor_ = -1;
        other.outbox_sent_ = 0;
    }
    return *this;
}

TcpStream TcpStream::adopt(int descriptor, std::string peer_address) {
    TcpStream stream;
    stream.descriptor_ = descriptor;
    stream.peer_address_ = std::move(peer_address);
    set_non_blocking(descriptor);
    const int one = 1;
    // Frames are latency-sensitive and small; waiting to coalesce them adds
    // exactly the delay this is meant to avoid.
    ::setsockopt(descriptor, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    bound_send_queue(descriptor);
    enable_keepalive(descriptor);
    return stream;
}

bool TcpStream::start_connect(const std::string& address, uint16_t port, std::string* error) {
    close();

    descriptor_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (descriptor_ < 0) {
        if (error != nullptr) *error = std::string("socket: ") + std::strerror(errno);
        return false;
    }
    set_non_blocking(descriptor_);
    const int one = 1;
    ::setsockopt(descriptor_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    bound_send_queue(descriptor_);
    enable_keepalive(descriptor_);

    sockaddr_in target {};
    target.sin_family = AF_INET;
    target.sin_port = htons(port);
    if (::inet_pton(AF_INET, address.c_str(), &target.sin_addr) != 1) {
        if (error != nullptr) *error = "not an IPv4 address: " + address;
        close();
        return false;
    }

    peer_address_ = address;
    if (::connect(descriptor_, reinterpret_cast<sockaddr*>(&target), sizeof(target)) == 0) {
        connecting_ = false;
        return true;
    }
    if (errno == EINPROGRESS) {
        connecting_ = true;
        return true;
    }
    if (error != nullptr) *error = std::string("connect: ") + std::strerror(errno);
    close();
    return false;
}

std::string TcpStream::local_address() const {
    if (descriptor_ < 0) {
        return std::string();
    }
    sockaddr_in local {};
    socklen_t length = sizeof(local);
    if (::getsockname(descriptor_, reinterpret_cast<sockaddr*>(&local), &length) != 0) {
        return std::string();
    }
    return describe(local);
}

void TcpStream::close() {
    if (descriptor_ >= 0) {
        ::close(descriptor_);
        descriptor_ = -1;
    }
    connecting_ = false;
    outbox_.clear();
    outbox_sent_ = 0;
}

void TcpStream::send(const uint8_t* data, std::size_t size) {
    if (descriptor_ < 0 || data == nullptr || size == 0) {
        return;
    }
    outbox_.insert(outbox_.end(), data, data + size);
    queued_total_ += size;
}

// The backlog nobody can see from outside: what this process is still holding,
// plus what the kernel has accepted but not yet put on the wire.
//
// That second part matters as much as the first. A sender that decides how far
// behind it is from its own queue alone believes it is keeping up right until
// the socket buffer is full, and everything sitting in that buffer is picture
// the player has not been shown yet. SIOCOUTQ is how Linux answers it; where a
// stack does not, the bound on SO_SNDBUF is what keeps the invisible part small
// enough not to matter.
std::size_t TcpStream::backlog_bytes() const {
    std::size_t total = pending_bytes();
#ifdef SIOCOUTQ
    if (descriptor_ >= 0) {
        int unsent = 0;
        if (::ioctl(descriptor_, SIOCOUTQ, &unsent) == 0 && unsent > 0) {
            total += static_cast<std::size_t>(unsent);
        }
    }
#endif
    return total;
}

bool TcpStream::pump(std::vector<uint8_t>& inbox, std::string* error) {
    if (descriptor_ < 0) {
        if (error != nullptr) *error = "not connected";
        return false;
    }

    if (connecting_) {
        // SO_ERROR only means anything once the socket reports itself
        // writable, which is how a non-blocking connect says it has finished
        // -- either way. Asked before that it reads 0, which is
        // indistinguishable from success: the old code took that for a
        // completed handshake on the very first poll, milliseconds after the
        // connect started, and the hello then went into a socket still in
        // SYN_SENT. Against a host at an address this device cannot route to
        // that looks exactly like being connected, right up until the join
        // times out.
        pollfd waiting {};
        waiting.fd = descriptor_;
        waiting.events = POLLOUT;
        const int ready = ::poll(&waiting, 1, 0);
        if (ready < 0 && errno != EINTR) {
            if (error != nullptr) *error = std::string("poll: ") + std::strerror(errno);
            return false;
        }
        if (ready <= 0 || (waiting.revents & (POLLOUT | POLLERR | POLLHUP)) == 0) {
            return true;  // still handshaking
        }
        int result = 0;
        socklen_t length = sizeof(result);
        if (::getsockopt(descriptor_, SOL_SOCKET, SO_ERROR, &result, &length) != 0) {
            if (error != nullptr) *error = std::string("getsockopt: ") + std::strerror(errno);
            return false;
        }
        if (result != 0) {
            if (error != nullptr) *error = std::string("connect: ") + std::strerror(result);
            return false;
        }
        connecting_ = false;
    }

    // Writes first, so a frame queued this tick goes out this tick.
    while (outbox_sent_ < outbox_.size()) {
        const ssize_t written = ::send(descriptor_, outbox_.data() + outbox_sent_,
                                       outbox_.size() - outbox_sent_, MSG_NOSIGNAL);
        if (written > 0) {
            outbox_sent_ += static_cast<std::size_t>(written);
            continue;
        }
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;  // the kernel buffer is full; the rest goes next tick
        }
        if (error != nullptr) *error = std::string("send: ") + std::strerror(errno);
        return false;
    }
    if (outbox_sent_ > 0) {
        outbox_.erase(outbox_.begin(), outbox_.begin() + static_cast<long>(outbox_sent_));
        outbox_sent_ = 0;
    }

    uint8_t chunk[64 * 1024];
    while (true) {
        const ssize_t read = ::recv(descriptor_, chunk, sizeof(chunk), 0);
        if (read > 0) {
            inbox.insert(inbox.end(), chunk, chunk + read);
            continue;
        }
        if (read == 0) {
            if (error != nullptr) *error = "the other device disconnected";
            return false;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        if (error != nullptr) *error = std::string("recv: ") + std::strerror(errno);
        return false;
    }
    return true;
}

// --- TcpListener ------------------------------------------------------------

TcpListener::~TcpListener() { stop(); }

bool TcpListener::start(uint16_t port, std::string* error) {
    stop();

    descriptor_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (descriptor_ < 0) {
        if (error != nullptr) *error = std::string("socket: ") + std::strerror(errno);
        return false;
    }
    const int one = 1;
    ::setsockopt(descriptor_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    set_non_blocking(descriptor_);

    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);
    if (::bind(descriptor_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        if (error != nullptr) {
            *error = "cannot listen on port " + std::to_string(port) + ": " + std::strerror(errno);
        }
        stop();
        return false;
    }
    if (::listen(descriptor_, 4) != 0) {
        if (error != nullptr) *error = std::string("listen: ") + std::strerror(errno);
        stop();
        return false;
    }

    // Port 0 means "any free port"; read back what we actually got.
    socklen_t length = sizeof(address);
    if (::getsockname(descriptor_, reinterpret_cast<sockaddr*>(&address), &length) == 0) {
        port_ = ntohs(address.sin_port);
    } else {
        port_ = port;
    }
    return true;
}

void TcpListener::stop() {
    if (descriptor_ >= 0) {
        ::close(descriptor_);
        descriptor_ = -1;
    }
    port_ = 0;
}

bool TcpListener::accept(TcpStream& out) {
    if (descriptor_ < 0) {
        return false;
    }
    sockaddr_in peer {};
    socklen_t length = sizeof(peer);
    const int accepted = ::accept(descriptor_, reinterpret_cast<sockaddr*>(&peer), &length);
    if (accepted < 0) {
        return false;
    }
    out = TcpStream::adopt(accepted, describe(peer));
    return true;
}

// --- UdpSocket --------------------------------------------------------------

UdpSocket::~UdpSocket() { stop(); }

bool UdpSocket::start(uint16_t listen_port, bool allow_broadcast, std::string* error) {
    stop();

    descriptor_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (descriptor_ < 0) {
        if (error != nullptr) *error = std::string("socket: ") + std::strerror(errno);
        return false;
    }
    set_non_blocking(descriptor_);
    const int one = 1;
    ::setsockopt(descriptor_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (allow_broadcast) {
        ::setsockopt(descriptor_, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
    }

    if (listen_port != 0) {
        sockaddr_in address {};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_ANY);
        address.sin_port = htons(listen_port);
        if (::bind(descriptor_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
            if (error != nullptr) {
                *error = "cannot bind UDP port " + std::to_string(listen_port) + ": " +
                         std::strerror(errno);
            }
            stop();
            return false;
        }
    }
    return true;
}

void UdpSocket::stop() {
    if (descriptor_ >= 0) {
        ::close(descriptor_);
        descriptor_ = -1;
    }
}

bool UdpSocket::broadcast(uint16_t port, const uint8_t* data, std::size_t size) {
    if (descriptor_ < 0 || data == nullptr || size == 0) {
        return false;
    }
    bool any = false;
    for (const LocalNetwork& network : announce_networks()) {
        sockaddr_in address {};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        if (::inet_pton(AF_INET, network.broadcast.c_str(), &address.sin_addr) != 1) {
            continue;
        }
        any = send_from(descriptor_, network, address, data, size) || any;
    }
    return any;
}

bool UdpSocket::receive(std::vector<uint8_t>& out, std::string& from_address) {
    if (descriptor_ < 0) {
        return false;
    }
    uint8_t chunk[2048];
    sockaddr_in peer {};
    socklen_t length = sizeof(peer);
    const ssize_t read = ::recvfrom(descriptor_, chunk, sizeof(chunk), 0,
                                    reinterpret_cast<sockaddr*>(&peer), &length);
    if (read <= 0) {
        return false;
    }
    out.assign(chunk, chunk + read);
    from_address = describe(peer);
    return true;
}

std::vector<LocalNetwork> local_networks() {
    std::vector<LocalNetwork> networks;

#if defined(DS_HAVE_GETIFADDRS)
    ifaddrs* list = nullptr;
    if (::getifaddrs(&list) == 0) {
        for (ifaddrs* entry = list; entry != nullptr; entry = entry->ifa_next) {
            if (entry->ifa_addr == nullptr || entry->ifa_addr->sa_family != AF_INET) {
                continue;
            }
            if ((entry->ifa_flags & IFF_UP) == 0) {
                continue;
            }
            LocalNetwork network;
            network.interface = entry->ifa_name != nullptr ? entry->ifa_name : "";
            network.address = describe(*reinterpret_cast<sockaddr_in*>(entry->ifa_addr));
            network.wireless = interface_is_wireless(network.interface.c_str());
            // Loopback is included on purpose: it is how two instances on one
            // machine find each other, which is exactly what the tests do.
            if ((entry->ifa_flags & IFF_BROADCAST) != 0 && entry->ifa_broadaddr != nullptr) {
                network.broadcast = describe(*reinterpret_cast<sockaddr_in*>(entry->ifa_broadaddr));
            } else if ((entry->ifa_flags & IFF_LOOPBACK) != 0) {
                network.broadcast = network.address;
            } else {
                continue;
            }
            networks.push_back(network);
        }
        ::freeifaddrs(list);
    }
#endif

    return networks;
}

std::vector<LocalNetwork> announce_networks() { return announce_networks(local_networks()); }

std::vector<LocalNetwork> announce_networks(const std::vector<LocalNetwork>& networks) {
    std::vector<LocalNetwork> announce;
    for (const LocalNetwork& network : wired_first(networks)) {
        const bool seen = std::any_of(announce.begin(), announce.end(),
                                      [&network](const LocalNetwork& already) {
                                          return already.broadcast == network.broadcast;
                                      });
        if (!seen) {
            announce.push_back(network);
        }
    }
    if (announce.empty()) {
        LocalNetwork everywhere;
        everywhere.broadcast = "255.255.255.255";
        announce.push_back(everywhere);
    }
    return announce;
}

std::string shared_network_warning(const std::vector<LocalNetwork>& given) {
    const std::vector<LocalNetwork> networks = wired_first(given);
    for (std::size_t first = 0; first < networks.size(); ++first) {
        std::vector<std::string> sharing;
        for (const LocalNetwork& network : networks) {
            if (network.broadcast == networks[first].broadcast) {
                sharing.push_back(network.interface + " (" + network.address + ")");
            }
        }
        if (sharing.size() < 2) {
            continue;
        }
        // Only the first such network is reported: a machine on one network
        // twice is already the whole of what the reader has to go and fix.
        std::string named;
        for (std::size_t index = 0; index < sharing.size(); ++index) {
            if (index > 0) named += index + 1 == sharing.size() ? " and " : ", ";
            named += sharing[index];
        }
        return named + " are on the same network; companions are pointed at " +
               networks[first].interface +
               ", but this machine still answers on the others and streaming can be uneven "
               "until they are off";
    }
    return std::string();
}

}  // namespace ds::net
