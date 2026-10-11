#include "update/zip_extract.hpp"

#include <zlib.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "config/paths.hpp"

namespace ds::update {
namespace {

// Bigger than any core, smaller than anything that would hurt to hold.
constexpr uint64_t kMaxEntry = 512ull * 1024 * 1024;

uint16_t le16(const uint8_t* at) { return static_cast<uint16_t>(at[0] | (at[1] << 8)); }
uint32_t le32(const uint8_t* at) {
    return static_cast<uint32_t>(at[0]) | (static_cast<uint32_t>(at[1]) << 8) |
           (static_cast<uint32_t>(at[2]) << 16) | (static_cast<uint32_t>(at[3]) << 24);
}

struct ZipEntry {
    std::string name;
    uint16_t method = 0;
    uint32_t crc = 0;
    uint32_t compressed = 0;
    uint32_t size = 0;
    uint32_t local_offset = 0;
    uint16_t flags = 0;
};

bool read_directory(const std::vector<uint8_t>& data, std::vector<ZipEntry>& entries, std::string* error) {
    const auto fail = [&](const char* why) {
        if (error != nullptr) *error = why;
        return false;
    };
    if (data.size() < 22) return fail("it is not a zip");
    // The end-of-directory record, searched for backwards past any comment.
    std::size_t end = std::string::npos;
    const std::size_t lowest = data.size() > 22 + 65535 ? data.size() - 22 - 65535 : 0;
    for (std::size_t at = data.size() - 22 + 1; at-- > lowest;) {
        if (le32(&data[at]) == 0x06054b50) {
            end = at;
            break;
        }
    }
    if (end == std::string::npos) return fail("it is not a zip");
    const uint16_t count = le16(&data[end + 10]);
    const uint32_t directory_size = le32(&data[end + 12]);
    const uint32_t directory_offset = le32(&data[end + 16]);
    if (directory_offset == 0xFFFFFFFFu || count == 0xFFFFu) return fail("it is a zip64 archive");
    if (static_cast<uint64_t>(directory_offset) + directory_size > data.size()) return fail("its directory is cut off");
    std::size_t at = directory_offset;
    for (uint16_t index = 0; index < count; ++index) {
        if (at + 46 > data.size() || le32(&data[at]) != 0x02014b50) return fail("its directory is damaged");
        ZipEntry entry;
        entry.flags = le16(&data[at + 8]);
        entry.method = le16(&data[at + 10]);
        entry.crc = le32(&data[at + 16]);
        entry.compressed = le32(&data[at + 20]);
        entry.size = le32(&data[at + 24]);
        const uint16_t name_length = le16(&data[at + 28]);
        const uint16_t extra_length = le16(&data[at + 30]);
        const uint16_t comment_length = le16(&data[at + 32]);
        entry.local_offset = le32(&data[at + 42]);
        if (at + 46 + name_length > data.size()) return fail("its directory is damaged");
        entry.name.assign(reinterpret_cast<const char*>(&data[at + 46]), name_length);
        entries.push_back(entry);
        at += 46u + name_length + extra_length + comment_length;
    }
    return true;
}

}  // namespace

bool list_zip(const std::string& zip, std::vector<std::string>& names, std::string* error) {
    std::vector<uint8_t> data;
    if (!read_file(zip, data, error)) return false;
    std::vector<ZipEntry> entries;
    if (!read_directory(data, entries, error)) return false;
    names.clear();
    for (const ZipEntry& entry : entries) names.push_back(entry.name);
    return true;
}

bool extract_zip_entry(const std::string& zip, const std::string& name, const std::string& out_path,
                       std::string* error) {
    const auto fail = [&](const std::string& why) {
        if (error != nullptr) *error = why;
        return false;
    };
    std::vector<uint8_t> data;
    if (!read_file(zip, data, error)) return false;
    std::vector<ZipEntry> entries;
    if (!read_directory(data, entries, error)) return false;
    const ZipEntry* found = nullptr;
    for (const ZipEntry& entry : entries) {
        if (entry.name == name) found = &entry;
    }
    if (found == nullptr) return fail("the zip has no " + name);
    if (found->flags & 0x1) return fail("the zip is encrypted");
    if (found->size > kMaxEntry) return fail(name + " is too big");
    const std::size_t local = found->local_offset;
    if (local + 30 > data.size() || le32(&data[local]) != 0x04034b50) return fail("the zip is damaged");
    const std::size_t start = local + 30 + le16(&data[local + 26]) + le16(&data[local + 28]);
    if (start + found->compressed > data.size()) return fail("the zip is cut off");

    std::vector<uint8_t> out(found->size);
    if (found->method == 0) {
        if (found->compressed != found->size) return fail("the zip is damaged");
        if (found->size > 0) std::memcpy(out.data(), &data[start], found->size);
    } else if (found->method == 8) {
        z_stream stream{};
        if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) return fail("zlib would not start");
        stream.next_in = &data[start];
        stream.avail_in = found->compressed;
        stream.next_out = out.data();
        stream.avail_out = static_cast<uInt>(out.size());
        const int result = inflate(&stream, Z_FINISH);
        const bool whole = result == Z_STREAM_END && stream.total_out == found->size;
        inflateEnd(&stream);
        if (!whole) return fail(name + " would not decompress");
    } else {
        return fail(name + " is compressed in a way this cannot read");
    }
    const uLong crc = crc32(crc32(0L, Z_NULL, 0), out.data(), static_cast<uInt>(out.size()));
    if (static_cast<uint32_t>(crc) != found->crc) return fail(name + " is damaged");
    return write_file_atomically(out_path, out.data(), out.size(), error);
}

}  // namespace ds::update
