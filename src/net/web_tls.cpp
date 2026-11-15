#include "net/web_tls.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/ecp.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/x509_csr.h>
#include <psa/crypto.h>

#include "net/socket_api.hpp"

// mbedTLS's own net layer is not used; these are its two codes, here.
#ifndef MBEDTLS_ERR_NET_SEND_FAILED
#define MBEDTLS_ERR_NET_SEND_FAILED -0x004E
#endif
#ifndef MBEDTLS_ERR_NET_RECV_FAILED
#define MBEDTLS_ERR_NET_RECV_FAILED -0x004C
#endif

namespace ds::net {

namespace {

std::string mbed_error(const char* what, int code) {
    char text[160] = {};
    mbedtls_strerror(code, text, sizeof(text));
    return std::string(what) + ": " + text;
}

// TLS 1.3 in mbedTLS 3.6 goes through PSA, which wants starting once before
// anything uses it.
bool ensure_psa(std::string* error) {
    static std::once_flag once;
    static psa_status_t status = PSA_ERROR_GENERIC_ERROR;
    std::call_once(once, [] { status = psa_crypto_init(); });
    if (status != PSA_SUCCESS) {
        if (error) *error = "TLS: could not start the crypto library";
        return false;
    }
    return true;
}

struct Random {
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    Random() {
        mbedtls_entropy_init(&entropy);
        mbedtls_ctr_drbg_init(&drbg);
    }
    ~Random() {
        mbedtls_ctr_drbg_free(&drbg);
        mbedtls_entropy_free(&entropy);
    }
    bool seed(std::string* error) {
        static const char kPersonal[] = "dualscreen-web-play";
        const int result = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
                                                 reinterpret_cast<const unsigned char*>(kPersonal),
                                                 sizeof(kPersonal) - 1);
        if (result != 0) {
            if (error) *error = mbed_error("TLS: no randomness", result);
            return false;
        }
        return true;
    }
};

// PEM in, parsed. mbedTLS wants the terminating NUL counted.
int parse_certificate(mbedtls_x509_crt& crt, const std::string& pem) {
    return mbedtls_x509_crt_parse(&crt, reinterpret_cast<const unsigned char*>(pem.c_str()), pem.size() + 1);
}

int parse_key(mbedtls_pk_context& key, const std::string& pem, Random& random) {
    return mbedtls_pk_parse_key(&key, reinterpret_cast<const unsigned char*>(pem.c_str()), pem.size() + 1,
                                nullptr, 0, mbedtls_ctr_drbg_random, &random.drbg);
}

}  // namespace

bool make_self_signed_identity(const std::string& common_name, TlsIdentity& out, std::string* error) {
    if (!ensure_psa(error)) return false;
    Random random;
    if (!random.seed(error)) return false;

    mbedtls_pk_context key;
    mbedtls_pk_init(&key);
    mbedtls_x509write_cert writer;
    mbedtls_x509write_crt_init(&writer);
    bool ok = false;
    do {
        int result = mbedtls_pk_setup(&key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
        if (result != 0) {
            if (error) *error = mbed_error("TLS: key", result);
            break;
        }
        result = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(key), mbedtls_ctr_drbg_random,
                                     &random.drbg);
        if (result != 0) {
            if (error) *error = mbed_error("TLS: key", result);
            break;
        }
        // Commas and equals signs would be read as more of the name.
        std::string name;
        for (char c : common_name) {
            if (c != ',' && c != '=' && c != '\\' && c != '+') name += c;
        }
        if (name.empty()) name = "Dualscreen";
        const std::string subject = "CN=" + name + ",O=Dualscreen";
        mbedtls_x509write_crt_set_version(&writer, MBEDTLS_X509_CRT_VERSION_3);
        mbedtls_x509write_crt_set_md_alg(&writer, MBEDTLS_MD_SHA256);
        mbedtls_x509write_crt_set_subject_key(&writer, &key);
        mbedtls_x509write_crt_set_issuer_key(&writer, &key);
        if ((result = mbedtls_x509write_crt_set_subject_name(&writer, subject.c_str())) != 0 ||
            (result = mbedtls_x509write_crt_set_issuer_name(&writer, subject.c_str())) != 0) {
            if (error) *error = mbed_error("TLS: name", result);
            break;
        }
        unsigned char serial[16];
        mbedtls_ctr_drbg_random(&random.drbg, serial, sizeof(serial));
        serial[0] &= 0x7F;  // positive
        serial[0] |= 0x01;  // and no leading zero byte
        mbedtls_x509write_crt_set_serial_raw(&writer, serial, sizeof(serial));
        mbedtls_x509write_crt_set_validity(&writer, "20250101000000", "20451231235959");
        mbedtls_x509write_crt_set_basic_constraints(&writer, 0, -1);
        mbedtls_x509write_crt_set_key_usage(&writer, MBEDTLS_X509_KU_DIGITAL_SIGNATURE);

        unsigned char buffer[4096];
        if ((result = mbedtls_x509write_crt_pem(&writer, buffer, sizeof(buffer), mbedtls_ctr_drbg_random,
                                                &random.drbg)) != 0) {
            if (error) *error = mbed_error("TLS: certificate", result);
            break;
        }
        out.certificate_pem = reinterpret_cast<const char*>(buffer);
        if ((result = mbedtls_pk_write_key_pem(&key, buffer, sizeof(buffer))) != 0) {
            if (error) *error = mbed_error("TLS: key", result);
            break;
        }
        out.key_pem = reinterpret_cast<const char*>(buffer);
        ok = true;
    } while (false);
    mbedtls_x509write_crt_free(&writer);
    mbedtls_pk_free(&key);
    return ok;
}

bool tls_identity_usable(const TlsIdentity& identity, std::string* error) {
    if (identity.empty()) {
        if (error) *error = "no certificate";
        return false;
    }
    if (!ensure_psa(error)) return false;
    Random random;
    if (!random.seed(error)) return false;
    mbedtls_x509_crt crt;
    mbedtls_x509_crt_init(&crt);
    mbedtls_pk_context key;
    mbedtls_pk_init(&key);
    bool ok = false;
    int result = parse_certificate(crt, identity.certificate_pem);
    if (result != 0) {
        if (error) *error = mbed_error("certificate", result);
    } else if ((result = parse_key(key, identity.key_pem, random)) != 0) {
        if (error) *error = mbed_error("key", result);
    } else if ((result = mbedtls_pk_check_pair(&crt.pk, &key, mbedtls_ctr_drbg_random, &random.drbg)) != 0) {
        if (error) *error = "the key is not this certificate's";
    } else {
        ok = true;
    }
    mbedtls_pk_free(&key);
    mbedtls_x509_crt_free(&crt);
    return ok;
}

std::string tls_fingerprint(const TlsIdentity& identity) {
    mbedtls_x509_crt crt;
    mbedtls_x509_crt_init(&crt);
    std::string text;
    if (parse_certificate(crt, identity.certificate_pem) == 0) {
        unsigned char digest[32];
        mbedtls_sha256(crt.raw.p, crt.raw.len, digest, 0);
        char hex[4];
        for (unsigned char byte : digest) {
            std::snprintf(hex, sizeof(hex), "%02X", byte);
            if (!text.empty()) text += ':';
            text += hex;
        }
    }
    mbedtls_x509_crt_free(&crt);
    return text;
}

struct TlsContext::Impl {
    Random random;
    mbedtls_ssl_config config;
    mbedtls_x509_crt certificate;
    mbedtls_pk_context key;
    bool server = true;
    Impl() {
        mbedtls_ssl_config_init(&config);
        mbedtls_x509_crt_init(&certificate);
        mbedtls_pk_init(&key);
    }
    ~Impl() {
        mbedtls_pk_free(&key);
        mbedtls_x509_crt_free(&certificate);
        mbedtls_ssl_config_free(&config);
    }
};

TlsContext::TlsContext() = default;
TlsContext::~TlsContext() = default;

bool TlsContext::start_server(const TlsIdentity& identity, std::string* error) {
    if (!ensure_psa(error)) return false;
    impl_.reset(new Impl());
    Impl& impl = *impl_;
    if (!impl.random.seed(error)) return false;
    int result = parse_certificate(impl.certificate, identity.certificate_pem);
    if (result != 0) {
        if (error) *error = mbed_error("TLS certificate", result);
        return false;
    }
    if ((result = parse_key(impl.key, identity.key_pem, impl.random)) != 0) {
        if (error) *error = mbed_error("TLS key", result);
        return false;
    }
    if ((result = mbedtls_ssl_config_defaults(&impl.config, MBEDTLS_SSL_IS_SERVER, MBEDTLS_SSL_TRANSPORT_STREAM,
                                              MBEDTLS_SSL_PRESET_DEFAULT)) != 0) {
        if (error) *error = mbed_error("TLS", result);
        return false;
    }
    mbedtls_ssl_conf_rng(&impl.config, mbedtls_ctr_drbg_random, &impl.random.drbg);
    if ((result = mbedtls_ssl_conf_own_cert(&impl.config, &impl.certificate, &impl.key)) != 0) {
        if (error) *error = mbed_error("TLS", result);
        return false;
    }
    impl.server = true;
    return true;
}

bool TlsContext::start_client(std::string* error) {
    if (!ensure_psa(error)) return false;
    impl_.reset(new Impl());
    Impl& impl = *impl_;
    if (!impl.random.seed(error)) return false;
    const int result = mbedtls_ssl_config_defaults(&impl.config, MBEDTLS_SSL_IS_CLIENT,
                                                   MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    if (result != 0) {
        if (error) *error = mbed_error("TLS", result);
        return false;
    }
    mbedtls_ssl_conf_rng(&impl.config, mbedtls_ctr_drbg_random, &impl.random.drbg);
    mbedtls_ssl_conf_authmode(&impl.config, MBEDTLS_SSL_VERIFY_NONE);
    impl.server = false;
    return true;
}

struct SecureStream::Tls {
    mbedtls_ssl_context ssl;
    bool handshake_done = false;
    int descriptor = -1;
    Tls() { mbedtls_ssl_init(&ssl); }
    ~Tls() { mbedtls_ssl_free(&ssl); }

    static int send(void* self, const unsigned char* data, size_t size) {
        Tls& tls = *static_cast<Tls*>(self);
#if defined(_WIN32)
        const int sent = ::send(static_cast<SOCKET>(tls.descriptor), reinterpret_cast<const char*>(data),
                                static_cast<int>(size), 0);
#elif defined(MSG_NOSIGNAL)
        const ssize_t sent = ::send(tls.descriptor, data, size, MSG_NOSIGNAL);
#else
        const ssize_t sent = ::send(tls.descriptor, data, size, 0);
#endif
        if (sent >= 0) return static_cast<int>(sent);
        const int code = socket_error();
        if (code == EWOULDBLOCK || code == EAGAIN || code == EINTR) return MBEDTLS_ERR_SSL_WANT_WRITE;
        return MBEDTLS_ERR_NET_SEND_FAILED;
    }
    static int receive(void* self, unsigned char* data, size_t size) {
        Tls& tls = *static_cast<Tls*>(self);
#if defined(_WIN32)
        const int got = ::recv(static_cast<SOCKET>(tls.descriptor), reinterpret_cast<char*>(data),
                               static_cast<int>(size), 0);
#else
        const ssize_t got = ::recv(tls.descriptor, data, size, 0);
#endif
        if (got >= 0) return static_cast<int>(got);
        const int code = socket_error();
        if (code == EWOULDBLOCK || code == EAGAIN || code == EINTR) return MBEDTLS_ERR_SSL_WANT_READ;
        return MBEDTLS_ERR_NET_RECV_FAILED;
    }
};

SecureStream::SecureStream() = default;
SecureStream::~SecureStream() { close(); }

SecureStream::SecureStream(SecureStream&& other) noexcept { *this = std::move(other); }

SecureStream& SecureStream::operator=(SecureStream&& other) noexcept {
    if (this != &other) {
        close();
        descriptor_ = other.descriptor_;
        tls_ = std::move(other.tls_);
        server_ = other.server_;
        sniffed_ = other.sniffed_;
        plain_http_ = other.plain_http_;
        closing_ = other.closing_;
        outbox_ = std::move(other.outbox_);
        outbox_sent_ = other.outbox_sent_;
        tls_write_pending_ = other.tls_write_pending_;
        other.descriptor_ = -1;
        other.outbox_sent_ = 0;
        other.tls_write_pending_ = 0;
    }
    return *this;
}

bool SecureStream::start(TlsContext* context, int descriptor, std::string* error) {
    close();
    descriptor_ = descriptor;
    outbox_.clear();
    outbox_sent_ = 0;
    tls_write_pending_ = 0;
    closing_ = false;
    plain_http_ = false;
    sniffed_ = true;
    server_ = false;
    if (context == nullptr || context->impl() == nullptr) return true;
    server_ = context->impl()->server;
    sniffed_ = !server_;
    tls_.reset(new Tls());
    tls_->descriptor = descriptor;
    const int result = mbedtls_ssl_setup(&tls_->ssl, &context->impl()->config);
    if (result != 0) {
        if (error) *error = mbed_error("TLS", result);
        close();
        return false;
    }
    mbedtls_ssl_set_bio(&tls_->ssl, tls_.get(), &Tls::send, &Tls::receive, nullptr);
    return true;
}

void SecureStream::close() {
    if (tls_ && tls_->handshake_done) mbedtls_ssl_close_notify(&tls_->ssl);
    tls_.reset();
    if (descriptor_ >= 0) close_socket(descriptor_);
    descriptor_ = -1;
}

bool SecureStream::secure_ready() const { return !tls_ || tls_->handshake_done; }

void SecureStream::send(const uint8_t* data, std::size_t size) {
    if (descriptor_ < 0 || size == 0) return;
    outbox_.insert(outbox_.end(), data, data + size);
}

void SecureStream::compact_outbox() {
    if (outbox_sent_ == outbox_.size()) {
        outbox_.clear();
        outbox_sent_ = 0;
    } else if (outbox_sent_ > (1u << 20) && outbox_sent_ > outbox_.size() / 2) {
        outbox_.erase(outbox_.begin(), outbox_.begin() + static_cast<std::ptrdiff_t>(outbox_sent_));
        outbox_sent_ = 0;
    }
}

bool SecureStream::pump(std::vector<uint8_t>& inbox, std::string* error) {
    if (descriptor_ < 0) {
        if (error) *error = "closed";
        return false;
    }
    if (!sniffed_) {
        // A TLS client's first byte is a handshake record, 0x16. Anything else
        // is someone who typed http://, and is answered in plain text.
        uint8_t first = 0;
#if defined(_WIN32)
        const int got = ::recv(static_cast<SOCKET>(descriptor_), reinterpret_cast<char*>(&first), 1, MSG_PEEK);
#else
        const ssize_t got = ::recv(descriptor_, &first, 1, MSG_PEEK);
#endif
        if (got == 0) {
            if (error) *error = "closed";
            return false;
        }
        if (got < 0) {
            const int code = socket_error();
            if (code == EWOULDBLOCK || code == EAGAIN || code == EINTR) return true;
            if (error) *error = socket_error_text(code);
            return false;
        }
        sniffed_ = true;
        if (first != 0x16) {
            plain_http_ = true;
            tls_.reset();
        }
    }
    return tls_ ? pump_tls(inbox, error) : pump_plain(inbox, error);
}

bool SecureStream::pump_plain(std::vector<uint8_t>& inbox, std::string* error) {
    while (outbox_sent_ < outbox_.size()) {
        const std::size_t size = outbox_.size() - outbox_sent_;
#if defined(_WIN32)
        const int sent = ::send(static_cast<SOCKET>(descriptor_), reinterpret_cast<const char*>(&outbox_[outbox_sent_]),
                                static_cast<int>(size), 0);
#elif defined(MSG_NOSIGNAL)
        const ssize_t sent = ::send(descriptor_, &outbox_[outbox_sent_], size, MSG_NOSIGNAL);
#else
        const ssize_t sent = ::send(descriptor_, &outbox_[outbox_sent_], size, 0);
#endif
        if (sent < 0) {
            const int code = socket_error();
            if (code == EWOULDBLOCK || code == EAGAIN || code == EINTR) break;
            if (error) *error = socket_error_text(code);
            return false;
        }
        outbox_sent_ += static_cast<std::size_t>(sent);
    }
    compact_outbox();
    if (closing_ && pending_bytes() == 0) {
        if (error) *error = "finished";
        return false;
    }
    uint8_t buffer[16384];
    for (;;) {
#if defined(_WIN32)
        const int got = ::recv(static_cast<SOCKET>(descriptor_), reinterpret_cast<char*>(buffer), sizeof(buffer), 0);
#else
        const ssize_t got = ::recv(descriptor_, buffer, sizeof(buffer), 0);
#endif
        if (got == 0) {
            if (error) *error = "closed";
            return false;
        }
        if (got < 0) {
            const int code = socket_error();
            if (code == EWOULDBLOCK || code == EAGAIN || code == EINTR) break;
            if (error) *error = socket_error_text(code);
            return false;
        }
        inbox.insert(inbox.end(), buffer, buffer + got);
    }
    return true;
}

bool SecureStream::pump_tls(std::vector<uint8_t>& inbox, std::string* error) {
    Tls& tls = *tls_;
    if (!tls.handshake_done) {
        const int result = mbedtls_ssl_handshake(&tls.ssl);
        if (result == MBEDTLS_ERR_SSL_WANT_READ || result == MBEDTLS_ERR_SSL_WANT_WRITE) return true;
        if (result != 0) {
            if (error) *error = mbed_error("TLS handshake", result);
            return false;
        }
        tls.handshake_done = true;
    }
    while (outbox_sent_ < outbox_.size()) {
        std::size_t size = tls_write_pending_;
        if (size == 0) size = std::min<std::size_t>(outbox_.size() - outbox_sent_, 16384);
        const int result = mbedtls_ssl_write(&tls.ssl, &outbox_[outbox_sent_], size);
        if (result == MBEDTLS_ERR_SSL_WANT_WRITE || result == MBEDTLS_ERR_SSL_WANT_READ) {
            tls_write_pending_ = size;
            break;
        }
        if (result < 0) {
            if (error) *error = mbed_error("TLS", result);
            return false;
        }
        tls_write_pending_ = 0;
        outbox_sent_ += static_cast<std::size_t>(result);
    }
    compact_outbox();
    if (closing_ && pending_bytes() == 0) {
        if (error) *error = "finished";
        return false;
    }
    unsigned char buffer[16384];
    for (;;) {
        const int result = mbedtls_ssl_read(&tls.ssl, buffer, sizeof(buffer));
        if (result > 0) {
            inbox.insert(inbox.end(), buffer, buffer + result);
            continue;
        }
        if (result == MBEDTLS_ERR_SSL_WANT_READ || result == MBEDTLS_ERR_SSL_WANT_WRITE) break;
#if defined(MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET)
        if (result == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) continue;
#endif
        if (result == 0 || result == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
            if (error) *error = "closed";
            return false;
        }
        if (error) *error = mbed_error("TLS", result);
        return false;
    }
    return true;
}

}  // namespace ds::net
