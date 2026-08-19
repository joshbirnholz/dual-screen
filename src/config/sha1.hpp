#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace ds {

// Streaming SHA-1. Games are keyed by the hash of their ROM contents rather
// than by path, so moving a ROM never loses its saved layout — and so two
// devices in a future networked session can agree on "same game" without
// sharing a filesystem.
class Sha1 {
public:
    Sha1();

    void update(const void* data, std::size_t length);
    // Returns the lowercase hex digest and resets the hasher.
    std::string finalize();

    static std::string hash_bytes(const void* data, std::size_t length);
    // Hashes a file's contents. Returns an empty string if the file cannot be read.
    static std::string hash_file(const std::string& path);

private:
    void reset();
    void process_block(const uint8_t* block);

    uint32_t state_[5];
    uint64_t bit_count_;
    uint8_t buffer_[64];
    std::size_t buffer_used_;
};

}  // namespace ds
