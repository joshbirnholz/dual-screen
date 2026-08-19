#include "config/sha1.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace ds {
namespace {

inline uint32_t rotl(uint32_t value, int bits) {
    return (value << bits) | (value >> (32 - bits));
}

}  // namespace

Sha1::Sha1() { reset(); }

void Sha1::reset() {
    state_[0] = 0x67452301u;
    state_[1] = 0xefcdab89u;
    state_[2] = 0x98badcfeu;
    state_[3] = 0x10325476u;
    state_[4] = 0xc3d2e1f0u;
    bit_count_ = 0;
    buffer_used_ = 0;
    std::memset(buffer_, 0, sizeof(buffer_));
}

void Sha1::process_block(const uint8_t* block) {
    uint32_t w[80];
    for (int i = 0; i < 16; ++i) {
        w[i] = (static_cast<uint32_t>(block[i * 4 + 0]) << 24) |
               (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
               (static_cast<uint32_t>(block[i * 4 + 2]) << 8) |
               (static_cast<uint32_t>(block[i * 4 + 3]));
    }
    for (int i = 16; i < 80; ++i) {
        w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }

    uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3], e = state_[4];
    for (int i = 0; i < 80; ++i) {
        uint32_t f, k;
        if (i < 20) {
            f = (b & c) | (~b & d);
            k = 0x5a827999u;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ed9eba1u;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8f1bbcdcu;
        } else {
            f = b ^ c ^ d;
            k = 0xca62c1d6u;
        }
        const uint32_t temp = rotl(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rotl(b, 30);
        b = a;
        a = temp;
    }

    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
}

void Sha1::update(const void* data, std::size_t length) {
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    bit_count_ += static_cast<uint64_t>(length) * 8u;

    while (length > 0) {
        const std::size_t take = std::min(length, sizeof(buffer_) - buffer_used_);
        std::memcpy(buffer_ + buffer_used_, bytes, take);
        buffer_used_ += take;
        bytes += take;
        length -= take;
        if (buffer_used_ == sizeof(buffer_)) {
            process_block(buffer_);
            buffer_used_ = 0;
        }
    }
}

std::string Sha1::finalize() {
    const uint64_t total_bits = bit_count_;

    // Append 0x80, pad with zeroes until 56 bytes into a block, then the
    // 64-bit big-endian message length.
    buffer_[buffer_used_++] = 0x80;
    if (buffer_used_ > 56) {
        std::memset(buffer_ + buffer_used_, 0, sizeof(buffer_) - buffer_used_);
        process_block(buffer_);
        buffer_used_ = 0;
    }
    std::memset(buffer_ + buffer_used_, 0, 56 - buffer_used_);
    for (int i = 0; i < 8; ++i) {
        buffer_[56 + i] = static_cast<uint8_t>((total_bits >> (56 - i * 8)) & 0xffu);
    }
    process_block(buffer_);

    char hex[41];
    for (int i = 0; i < 5; ++i) {
        std::snprintf(hex + i * 8, 9, "%08x", state_[i]);
    }
    std::string digest(hex, 40);

    reset();
    return digest;
}

std::string Sha1::hash_bytes(const void* data, std::size_t length) {
    Sha1 hasher;
    hasher.update(data, length);
    return hasher.finalize();
}

std::string Sha1::hash_file(const std::string& path) {
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        return std::string();
    }
    Sha1 hasher;
    std::vector<uint8_t> chunk(64 * 1024);
    while (true) {
        const std::size_t read = std::fread(chunk.data(), 1, chunk.size(), file);
        if (read > 0) {
            hasher.update(chunk.data(), read);
        }
        if (read < chunk.size()) {
            break;
        }
    }
    const bool failed = std::ferror(file) != 0;
    std::fclose(file);
    if (failed) {
        return std::string();
    }
    return hasher.finalize();
}

}  // namespace ds
