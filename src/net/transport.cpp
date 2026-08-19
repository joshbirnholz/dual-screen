#include "net/transport.hpp"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <cstring>
#include <utility>

#include "util/log.hpp"

namespace ds::net {
namespace {

bool set_non_blocking(int descriptor) {
    const int flags = ::fcntl(descriptor, F_GETFL, 0);
    return flags >= 0 && ::fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) == 0;
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
}

bool TcpStream::pump(std::vector<uint8_t>& inbox, std::string* error) {
    if (descriptor_ < 0) {
        if (error != nullptr) *error = "not connected";
        return false;
    }

    if (connecting_) {
        int result = 0;
        socklen_t length = sizeof(result);
        if (::getsockopt(descriptor_, SOL_SOCKET, SO_ERROR, &result, &length) != 0) {
            if (error != nullptr) *error = std::string("getsockopt: ") + std::strerror(errno);
            return false;
        }
        if (result == EINPROGRESS) {
            return true;  // still handshaking
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
    for (const std::string& target : broadcast_addresses()) {
        sockaddr_in address {};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        if (::inet_pton(AF_INET, target.c_str(), &address.sin_addr) != 1) {
            continue;
        }
        const ssize_t sent = ::sendto(descriptor_, data, size, 0,
                                      reinterpret_cast<sockaddr*>(&address), sizeof(address));
        any = any || (sent == static_cast<ssize_t>(size));
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

std::vector<std::string> broadcast_addresses() {
    std::vector<std::string> addresses;

    ifaddrs* list = nullptr;
    if (::getifaddrs(&list) == 0) {
        for (ifaddrs* entry = list; entry != nullptr; entry = entry->ifa_next) {
            if (entry->ifa_addr == nullptr || entry->ifa_addr->sa_family != AF_INET) {
                continue;
            }
            if ((entry->ifa_flags & IFF_UP) == 0) {
                continue;
            }
            // Loopback is included on purpose: it is how two instances on one
            // machine find each other, which is exactly what the tests do.
            if ((entry->ifa_flags & IFF_BROADCAST) != 0 && entry->ifa_broadaddr != nullptr) {
                addresses.push_back(
                    describe(*reinterpret_cast<sockaddr_in*>(entry->ifa_broadaddr)));
            } else if ((entry->ifa_flags & IFF_LOOPBACK) != 0) {
                addresses.push_back(describe(*reinterpret_cast<sockaddr_in*>(entry->ifa_addr)));
            }
        }
        ::freeifaddrs(list);
    }

    if (addresses.empty()) {
        addresses.push_back("255.255.255.255");
    }
    return addresses;
}

}  // namespace ds::net
