#pragma once

// The BSD socket calls, the same on every platform this builds for.
//
// Winsock is BSD sockets with its own spellings: closesocket() rather than
// close(), ioctlsocket() rather than fcntl(), WSAPoll() rather than poll(),
// and an error code from WSAGetLastError() rather than errno. Everything in
// this codebase that touches a socket goes through the few names below, so
// the rest reads the same everywhere. A socket is kept in an int, as it
// always was: a Winsock SOCKET is a small handle value in practice, and
// INVALID_SOCKET becomes -1 like a failed socket() does elsewhere.

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <string>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <sys/types.h>
// <windows.h> defines these as macros for old COM and 16-bit code. They are
// ordinary words in this codebase (LocalNetwork::interface, near/far).
#undef interface
#undef near
#undef far
// Windows 10 (1709 on) has per-socket keepalive timing under the same names
// as Linux; MinGW's headers predate them. The values are the Windows SDK's.
#ifndef TCP_KEEPIDLE
#define TCP_KEEPIDLE 3
#endif
#ifndef TCP_KEEPCNT
#define TCP_KEEPCNT 16
#endif
#ifndef TCP_KEEPINTVL
#define TCP_KEEPINTVL 17
#endif
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace ds::net {

#if defined(_WIN32)
using socklen_type = int;
#else
using socklen_type = socklen_t;
#endif

// Starts Winsock the first time any socket is wanted. Harmless and free
// elsewhere, and safe to call from several threads at once (a function-local
// static is initialised exactly once). Never cleaned up: the process ending
// does that.
inline void ensure_sockets() {
#if defined(_WIN32)
    static const bool started = [] {
        WSADATA data {};
        return ::WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    (void)started;
#endif
}

// A new socket, with Winsock started first where it needs starting.
inline int open_socket(int family, int type, int protocol) {
    ensure_sockets();
    return static_cast<int>(::socket(family, type, protocol));
}

inline void close_socket(int descriptor) {
    if (descriptor < 0) return;
#if defined(_WIN32)
    ::closesocket(static_cast<SOCKET>(descriptor));
#else
    ::close(descriptor);
#endif
}

inline bool set_socket_non_blocking(int descriptor) {
#if defined(_WIN32)
    u_long on = 1;
    return ::ioctlsocket(static_cast<SOCKET>(descriptor), FIONBIO, &on) == 0;
#else
    const int flags = ::fcntl(descriptor, F_GETFL, 0);
    return flags >= 0 && ::fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

// The last socket call's error, as the POSIX errno value it means (EAGAIN,
// EINPROGRESS, ECONNRESET...), so one comparison serves every platform.
inline int socket_error() {
#if defined(_WIN32)
    switch (::WSAGetLastError()) {
        case WSAEWOULDBLOCK: return EWOULDBLOCK;
        case WSAEINPROGRESS: return EINPROGRESS;
        case WSAEALREADY: return EALREADY;
        case WSAEINTR: return EINTR;
        case WSAECONNRESET: return ECONNRESET;
        case WSAECONNABORTED: return ECONNABORTED;
        case WSAECONNREFUSED: return ECONNREFUSED;
        case WSAENOTCONN: return ENOTCONN;
        case WSAESHUTDOWN: return EPIPE;
        case WSAETIMEDOUT: return ETIMEDOUT;
        case WSAEHOSTUNREACH: return EHOSTUNREACH;
        case WSAENETUNREACH: return ENETUNREACH;
        case WSAENETDOWN: return ENETDOWN;
        case WSAEADDRINUSE: return EADDRINUSE;
        case WSAEADDRNOTAVAIL: return EADDRNOTAVAIL;
        case WSAEACCES: return EACCES;
        case WSAEMSGSIZE: return EMSGSIZE;
        case WSAENOBUFS: return ENOBUFS;
        case WSAEINVAL: return EINVAL;
        case WSAENOTSOCK: return ENOTSOCK;
        default: return EIO;
    }
#else
    return errno;
#endif
}

// What that error means, in words. std::strerror() knows the POSIX values
// socket_error() maps Winsock's onto only partly on Windows, so the common
// ones are spelled out here.
inline std::string socket_error_text(int code) {
    switch (code) {
        case EWOULDBLOCK: return "Resource temporarily unavailable";
        case EINPROGRESS: return "Operation now in progress";
        case ECONNRESET: return "Connection reset by peer";
        case ECONNABORTED: return "Connection aborted";
        case ECONNREFUSED: return "Connection refused";
        case ENOTCONN: return "Not connected";
        case ETIMEDOUT: return "Connection timed out";
        case EHOSTUNREACH: return "No route to host";
        case ENETUNREACH: return "Network is unreachable";
        case ENETDOWN: return "Network is down";
        case EADDRINUSE: return "Address already in use";
        case EADDRNOTAVAIL: return "Cannot assign requested address";
        default: return std::strerror(code);
    }
}

inline bool socket_would_block(int code) { return code == EAGAIN || code == EWOULDBLOCK; }

// A non-blocking connect that has started and not finished: EINPROGRESS on
// POSIX, WSAEWOULDBLOCK on Windows -- both arrive here as one or the other.
inline bool socket_connect_pending(int code) {
#if defined(_WIN32)
    return code == EINPROGRESS || socket_would_block(code);
#else
    return code == EINPROGRESS;
#endif
}

inline int poll_sockets(pollfd* fds, std::size_t count, int timeout_ms) {
#if defined(_WIN32)
    return ::WSAPoll(fds, static_cast<ULONG>(count), timeout_ms);
#else
    return ::poll(fds, static_cast<nfds_t>(count), timeout_ms);
#endif
}

// The set/getsockopt value pointer, which Winsock takes as a char*.
#if defined(_WIN32)
#define DS_SOCKOPT(pointer) reinterpret_cast<const char*>(pointer)
#define DS_SOCKOPT_OUT(pointer) reinterpret_cast<char*>(pointer)
#else
#define DS_SOCKOPT(pointer) (pointer)
#define DS_SOCKOPT_OUT(pointer) (pointer)
#endif

// send()/recv() buffers, likewise.
#if defined(_WIN32)
#define DS_SOCKBUF(pointer) reinterpret_cast<const char*>(pointer)
#define DS_SOCKBUF_OUT(pointer) reinterpret_cast<char*>(pointer)
#else
#define DS_SOCKBUF(pointer) (pointer)
#define DS_SOCKBUF_OUT(pointer) (pointer)
#endif

}  // namespace ds::net
