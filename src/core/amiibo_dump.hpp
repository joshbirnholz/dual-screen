#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <vector>

namespace ds {

// Whether a file's bytes are an amiibo dump Cemu will take: an NTAG215's
// pages, 532 bytes or more, with the fixed bytes every amiibo has where Cemu
// looks for them -- see nnNfp_touchNfcTagFromFile() in Cemu. Checked before
// the menu closes, so a file that is not one is said to be so there rather
// than after the game has gone on without it.
inline bool is_amiibo_dump(const std::vector<uint8_t>& bytes) {
    constexpr std::size_t kNtag215Size = 532;
    if (bytes.size() < kNtag215Size) return false;
    const auto at = [&](std::size_t offset, std::initializer_list<uint8_t> expected) {
        std::size_t index = offset;
        for (const uint8_t value : expected) {
            if (bytes[index++] != value) return false;
        }
        return true;
    };
    // Static lock, capability container, and the two configuration pages.
    if (!at(0x00A, {0x0F, 0xE0})) return false;
    if (!at(0x00C, {0xF1, 0x10, 0xFF, 0xEE})) return false;
    if (!at(0x20C, {0x00, 0x00, 0x00, 0x04})) return false;
    if (!at(0x210, {0x5F, 0x00, 0x00, 0x00})) return false;
    // The serial number's two check bytes.
    const uint8_t bcc0 = static_cast<uint8_t>(bytes[0] ^ bytes[1] ^ bytes[2] ^ 0x88);
    const uint8_t bcc1 = static_cast<uint8_t>(bytes[4] ^ bytes[5] ^ bytes[6] ^ bytes[7]);
    return bytes[3] == bcc0 && bytes[8] == bcc1;
}

}  // namespace ds
