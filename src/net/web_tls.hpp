#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ds::net {

// HTTPS for Web Play's page, over mbedTLS.
//
// A browser only hands a page its gamepads, and only decodes video for it,
// when the page came over HTTPS (or from localhost) -- so a friend's browser
// opening this computer by its address has to be spoken to in TLS, whatever
// network it came over. Nothing in here knows or cares how they got here: a
// LAN, a forwarded port, or a VPN with its own HTTPS in front of this one are
// all just a TCP connection arriving.

// A certificate and the private key that goes with it, both PEM.
struct TlsIdentity {
    std::string certificate_pem;
    std::string key_pem;
    bool empty() const { return certificate_pem.empty() || key_pem.empty(); }
};

// A new certificate signed by its own key, for when the player has given none:
// a browser warns once before trusting it, and then remembers. P-256, good for
// twenty years, named `common_name`.
bool make_self_signed_identity(const std::string& common_name, TlsIdentity& out, std::string* error);

// Whether a certificate and key read and belong together -- what Settings
// checks a player's own files with before using them.
bool tls_identity_usable(const TlsIdentity& identity, std::string* error);

// The SHA-256 of the certificate, as "AB:CD:..." -- what a browser's
// certificate viewer shows, so a player can tell it is this computer's.
std::string tls_fingerprint(const TlsIdentity& identity);

// What a set of connections shares: the certificate, and the randomness.
class TlsContext {
public:
    TlsContext();
    ~TlsContext();
    TlsContext(const TlsContext&) = delete;
    TlsContext& operator=(const TlsContext&) = delete;

    // A server answering with `identity`.
    bool start_server(const TlsIdentity& identity, std::string* error);
    // A client that trusts anything -- for tests only.
    bool start_client(std::string* error);

    struct Impl;
    Impl* impl() const { return impl_.get(); }

private:
    std::unique_ptr<Impl> impl_;
};

// One connection, over a non-blocking socket it owns, speaking TLS -- or, if
// made with a null context, nothing but the bytes themselves (what the tests
// of the layers above it use, and what a browser that knocked with plain HTTP
// is answered with; see looks_like_plain_http()).
//
// Polled, like everything else on the network: pump() moves what it can and
// never waits.
class SecureStream {
public:
    SecureStream();
    ~SecureStream();
    SecureStream(SecureStream&& other) noexcept;
    SecureStream& operator=(SecureStream&& other) noexcept;
    SecureStream(const SecureStream&) = delete;
    SecureStream& operator=(const SecureStream&) = delete;

    // Takes `descriptor`, already non-blocking. `context` may be null for no
    // TLS at all.
    bool start(TlsContext* context, int descriptor, std::string* error);
    bool open() const { return descriptor_ >= 0; }
    void close();

    // Whether the handshake is over (always, without TLS).
    bool secure_ready() const;
    // A server's: the first bytes in were not a TLS handshake but the start
    // of an HTTP request -- someone typed http:// -- and the stream went over
    // to plain bytes to answer it with where to go instead.
    bool looks_like_plain_http() const { return plain_http_; }

    // Queues bytes for sending, sent during pump() once the handshake is done.
    void send(const uint8_t* data, std::size_t size);
    void send(const std::vector<uint8_t>& data) { send(data.data(), data.size()); }
    void send(const std::string& text) { send(reinterpret_cast<const uint8_t*>(text.data()), text.size()); }
    // What is still waiting in this process to go out.
    std::size_t pending_bytes() const { return outbox_.size() - outbox_sent_; }

    // Moves what it can each way: reads land on the end of `inbox`. False once
    // the connection is over, with why in `error`.
    bool pump(std::vector<uint8_t>& inbox, std::string* error);

    // Closes once everything queued has gone, rather than at once.
    void close_after_sending() { closing_ = true; }
    bool closing() const { return closing_; }

    int descriptor() const { return descriptor_; }
    // For the TLS layer's callbacks.
    struct Tls;

private:
    bool pump_plain(std::vector<uint8_t>& inbox, std::string* error);
    bool pump_tls(std::vector<uint8_t>& inbox, std::string* error);
    void compact_outbox();

    int descriptor_ = -1;
    std::unique_ptr<Tls> tls_;
    bool server_ = false;
    bool sniffed_ = false;
    bool plain_http_ = false;
    bool closing_ = false;
    std::vector<uint8_t> outbox_;
    std::size_t outbox_sent_ = 0;
    // What the last write that could not finish was asked to write, which
    // mbedTLS needs to be asked again exactly.
    std::size_t tls_write_pending_ = 0;
};

}  // namespace ds::net
