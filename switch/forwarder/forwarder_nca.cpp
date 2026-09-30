// SPDX-License-Identifier: GPL-3.0-or-later
//
// Adapted from Sphaira's owo.cpp (https://github.com/ITotalJustice/sphaira,
// GPL-3.0), which builds its romfs after hacbrewpack. See forwarder_nca.hpp.

#include "forwarder_nca.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>

namespace ds::forwarder {

// --- SHA-256 (FIPS 180-4) ----------------------------------------------------

namespace {

constexpr uint32_t kSha256K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

void sha256_block(uint32_t state[8], const uint8_t* block) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = (uint32_t(block[i * 4]) << 24) | (uint32_t(block[i * 4 + 1]) << 16) |
               (uint32_t(block[i * 4 + 2]) << 8) | uint32_t(block[i * 4 + 3]);
    }
    for (int i = 16; i < 64; ++i) {
        const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
    for (int i = 0; i < 64; ++i) {
        const uint32_t t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) +
                            kSha256K[i] + w[i];
        const uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

}  // namespace

Sha256Digest sha256(const void* data, std::size_t size) {
    uint32_t state[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                         0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    const auto* bytes = static_cast<const uint8_t*>(data);
    std::size_t whole = size - size % 64;
    for (std::size_t i = 0; i < whole; i += 64) sha256_block(state, bytes + i);

    uint8_t tail[128] = {};
    const std::size_t left = size - whole;
    if (left > 0) std::memcpy(tail, bytes + whole, left);
    tail[left] = 0x80;
    const std::size_t tail_size = left + 1 + 8 <= 64 ? 64 : 128;
    const uint64_t bits = uint64_t(size) * 8;
    for (int i = 0; i < 8; ++i) tail[tail_size - 1 - i] = uint8_t(bits >> (i * 8));
    for (std::size_t i = 0; i < tail_size; i += 64) sha256_block(state, tail + i);

    Sha256Digest digest{};
    for (int i = 0; i < 8; ++i) {
        digest[i * 4] = uint8_t(state[i] >> 24);
        digest[i * 4 + 1] = uint8_t(state[i] >> 16);
        digest[i * 4 + 2] = uint8_t(state[i] >> 8);
        digest[i * 4 + 3] = uint8_t(state[i]);
    }
    return digest;
}

namespace {

constexpr uint32_t kIvfcLevels = 6;
constexpr uint32_t kIvfcBlockSize = 0x4000;
constexpr uint32_t kExefsHashBlockSize = 0x10000;
constexpr uint32_t kMetaHashBlockSize = 0x1000;
constexpr uint32_t kMediaUnit = 0x200;
constexpr uint32_t kRomfsEmpty = 0xFFFFFFFF;
constexpr uint32_t kRomfsFileDataOffset = 0x200;
constexpr uint32_t kNca3Magic = 0x3341434E;  // "NCA3"

// The NCA header, as nca.hpp in Sphaira (yati) lays it out.
#pragma pack(push, 1)
struct LayerRegion {
    uint64_t offset;
    uint64_t size;
};
struct HierarchicalSha256Data {
    uint8_t master_hash[0x20];
    uint32_t block_size;
    uint32_t layer_count;
    LayerRegion hash_layer;
    LayerRegion pfs0_layer;
    LayerRegion unused_layers[3];
    uint8_t reserved[0x80];
};
struct IvfcLevel {
    uint64_t logical_offset;
    uint64_t hash_data_size;
    uint32_t block_size;  // log2
    uint32_t reserved;
};
struct IntegrityMetaInfo {
    uint32_t magic;  // "IVFC"
    uint32_t version;
    uint32_t master_hash_size;
    uint32_t max_layers;
    IvfcLevel levels[kIvfcLevels];
    uint8_t signature_salt[0x20];
    uint8_t master_hash[0x20];
    uint8_t reserved[0x18];
};
struct FsHeader {
    uint16_t version;
    uint8_t fs_type;
    uint8_t hash_type;
    uint8_t encryption_type;
    uint8_t metadata_hash_type;
    uint8_t reserved0[2];
    union {
        HierarchicalSha256Data sha256;
        IntegrityMetaInfo integrity;
    } hash_data;
    uint8_t rest[0x100];  // patch, counter, sparse and compression info: all unused
};
struct SectionTableEntry {
    uint32_t media_start_offset;
    uint32_t media_end_offset;
    uint8_t reserved[8];
};
struct NcaHeader {
    uint8_t rsa_fixed_key[0x100];
    uint8_t rsa_npdm[0x100];
    uint32_t magic;
    uint8_t distribution_type;
    uint8_t content_type;
    uint8_t old_key_gen;
    uint8_t kaek_index;
    uint64_t size;
    uint64_t program_id;
    uint32_t context_id;
    uint32_t sdk_version;
    uint8_t key_gen;
    uint8_t sig_key_gen;
    uint8_t reserved0[0xE];
    uint8_t rights_id[0x10];
    SectionTableEntry fs_table[4];
    uint8_t fs_header_hash[4][0x20];
    uint8_t key_area[4][0x10];
    uint8_t reserved1[0xC0];
    FsHeader fs_header[4];
};
struct RomfsHeader {
    uint64_t header_size;
    uint64_t dir_hash_table_offset;
    uint64_t dir_hash_table_size;
    uint64_t dir_table_offset;
    uint64_t dir_table_size;
    uint64_t file_hash_table_offset;
    uint64_t file_hash_table_size;
    uint64_t file_table_offset;
    uint64_t file_table_size;
    uint64_t file_data_offset;
};
struct RomfsDirEntry {
    uint32_t parent;
    uint32_t sibling;
    uint32_t child_dir;
    uint32_t child_file;
    uint32_t next_hash;
    uint32_t name_length;
};
struct RomfsFileEntry {
    uint32_t parent;
    uint32_t sibling;
    uint64_t data_offset;
    uint64_t data_size;
    uint32_t next_hash;
    uint32_t name_length;
};
struct Pfs0Header {
    uint32_t magic;  // "PFS0"
    uint32_t file_count;
    uint32_t string_table_size;
    uint32_t reserved;
};
struct Pfs0FileEntry {
    uint64_t data_offset;
    uint64_t data_size;
    uint32_t name_offset;
    uint32_t reserved;
};
struct CnmtHeader {
    uint64_t title_id;
    uint32_t title_version;
    uint8_t meta_type;
    uint8_t reserved0;
    ContentMetaHeader meta_header;
    uint8_t install_type;
    uint8_t reserved1;
    uint32_t required_system_version;
    uint8_t reserved2[4];
};
// The start of an NPDM and its two access control blocks, as npdm.hpp in
// Sphaira lays them out.
struct NpdmMeta {
    uint32_t magic;  // "META"
    uint32_t signature_key_generation;
    uint32_t reserved0;
    uint8_t flags;
    uint8_t reserved1;
    uint8_t main_thread_priority;
    uint8_t main_thread_core;
    uint32_t reserved2;
    uint32_t system_resource_size;
    uint32_t version;
    uint32_t main_thread_stack_size;
    char title_name[0x10];
    char product_code[0x10];
    uint8_t reserved3[0x30];
    uint32_t aci0_offset;
    uint32_t aci0_size;
    uint32_t acid_offset;
    uint32_t acid_size;
};
struct NpdmAcid {
    uint8_t rsa_signature[0x100];
    uint8_t rsa_public_key[0x100];
    uint32_t magic;  // "ACID"
    uint32_t size;
    uint8_t version;
    uint8_t reserved0[3];
    uint32_t flags;
    uint64_t program_id_min;
    uint64_t program_id_max;
    uint32_t fac_offset;
    uint32_t fac_size;
    uint32_t sac_offset;
    uint32_t sac_size;
    uint32_t kac_offset;
    uint32_t kac_size;
    uint8_t reserved1[8];
};
struct NpdmAci0 {
    uint32_t magic;  // "ACI0"
    uint8_t reserved0[0xC];
    uint64_t program_id;
    uint8_t reserved1[8];
    uint32_t fac_offset;
    uint32_t fac_size;
    uint32_t sac_offset;
    uint32_t sac_size;
    uint32_t kac_offset;
    uint32_t kac_size;
    uint8_t reserved2[8];
};
#pragma pack(pop)
static_assert(sizeof(HierarchicalSha256Data) == 0xF8);
static_assert(sizeof(IntegrityMetaInfo) == 0xF8);
static_assert(sizeof(FsHeader) == 0x200);
static_assert(sizeof(NcaHeader) == kNcaHeaderSize);
static_assert(offsetof(NcaHeader, fs_table) == 0x240);
static_assert(offsetof(NcaHeader, fs_header) == 0x400);
static_assert(sizeof(RomfsHeader) == 0x50);
static_assert(sizeof(RomfsDirEntry) == 0x18);
static_assert(sizeof(RomfsFileEntry) == 0x20);
static_assert(sizeof(CnmtHeader) == 0x20);
static_assert(sizeof(NpdmMeta) == 0x80);
static_assert(sizeof(NpdmAcid) == 0x240);
static_assert(sizeof(NpdmAci0) == 0x40);

enum : uint8_t {
    kNcaContentProgram = 0,
    kNcaContentMeta = 1,
    kNcaContentControl = 2,
    kFsTypeRomfs = 0,
    kFsTypePfs0 = 1,
    kHashHierarchicalSha256 = 2,
    kHashHierarchicalIntegrity = 3,
    kEncryptionNone = 1,
    kContentMetaTypeApplication = 0x80,
    kInstallTypeFull = 0,
};

uint64_t align_up(uint64_t value, uint64_t alignment) {
    return (value + alignment - 1) / alignment * alignment;
}

void pad_to(std::vector<uint8_t>& buffer, uint64_t alignment) {
    buffer.resize(align_up(buffer.size(), alignment), 0);
}

template <typename T>
void append(std::vector<uint8_t>& buffer, const T& value) {
    const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
    buffer.insert(buffer.end(), bytes, bytes + sizeof(T));
}

void append(std::vector<uint8_t>& buffer, const std::vector<uint8_t>& bytes) {
    buffer.insert(buffer.end(), bytes.begin(), bytes.end());
}

uint32_t romfs_path_hash(uint32_t parent, const std::string& name) {
    uint32_t hash = parent ^ 123456789;
    for (const char c : name) {
        hash = (hash >> 5) | (hash << 27);
        hash ^= static_cast<uint8_t>(c);
    }
    return hash;
}

uint32_t romfs_hash_table_count(uint32_t entries) {
    if (entries < 3) return 3;
    if (entries < 19) return entries | 1;
    uint32_t count = entries;
    while (count % 2 == 0 || count % 3 == 0 || count % 5 == 0 || count % 7 == 0 || count % 11 == 0 ||
           count % 13 == 0 || count % 17 == 0) {
        ++count;
    }
    return count;
}

// Each 0x4000 block of `level`, hashed: the IVFC level above it.
std::vector<uint8_t> ivfc_level_above(const std::vector<uint8_t>& level) {
    std::vector<uint8_t> hashes;
    for (std::size_t offset = 0; offset < level.size(); offset += kIvfcBlockSize) {
        const std::size_t size = std::min<std::size_t>(kIvfcBlockSize, level.size() - offset);
        append(hashes, sha256(level.data() + offset, size));
    }
    pad_to(hashes, kIvfcBlockSize);
    return hashes;
}

std::vector<uint8_t> build_pfs0(const std::vector<RomfsFile>& files) {
    std::vector<uint8_t> strings;
    std::vector<Pfs0FileEntry> entries;
    uint64_t data_offset = 0;
    for (const RomfsFile& file : files) {
        Pfs0FileEntry entry{};
        entry.data_offset = data_offset;
        entry.data_size = file.data.size();
        entry.name_offset = static_cast<uint32_t>(strings.size());
        entries.push_back(entry);
        strings.insert(strings.end(), file.name.begin(), file.name.end());
        strings.push_back(0);
        data_offset += file.data.size();
    }
    pad_to(strings, 0x20);

    std::vector<uint8_t> pfs0;
    Pfs0Header header{};
    header.magic = 0x30534650;
    header.file_count = static_cast<uint32_t>(files.size());
    header.string_table_size = static_cast<uint32_t>(strings.size());
    append(pfs0, header);
    for (const Pfs0FileEntry& entry : entries) append(pfs0, entry);
    append(pfs0, strings);
    for (const RomfsFile& file : files) append(pfs0, file.data);
    return pfs0;
}

void finish_section(NcaHeader& header, int index, uint64_t start, uint64_t end) {
    header.fs_table[index].media_start_offset = static_cast<uint32_t>(start / kMediaUnit);
    header.fs_table[index].media_end_offset = static_cast<uint32_t>(end / kMediaUnit);
    header.fs_table[index].reserved[0] = 1;
    const Sha256Digest hash = sha256(&header.fs_header[index], sizeof(FsHeader));
    std::memcpy(header.fs_header_hash[index], hash.data(), hash.size());
}

uint64_t section_start(const NcaHeader& header, int index) {
    return index == 0 ? kNcaHeaderSize : uint64_t(header.fs_table[index - 1].media_end_offset) * kMediaUnit;
}

// A PFS0 section: its hash table, then the PFS0 itself.
void write_pfs0_section(NcaHeader& header, int index, const std::vector<RomfsFile>& files,
                        uint32_t block_size, std::vector<uint8_t>& nca) {
    const std::vector<uint8_t> pfs0 = build_pfs0(files);
    std::vector<uint8_t> hash_table;
    for (std::size_t offset = 0; offset < pfs0.size(); offset += block_size) {
        const std::size_t size = std::min<std::size_t>(block_size, pfs0.size() - offset);
        append(hash_table, sha256(pfs0.data() + offset, size));
    }
    const uint64_t start = section_start(header, index);
    const uint64_t pfs0_offset = align_up(hash_table.size(), kMediaUnit);
    append(nca, hash_table);
    nca.resize(start + pfs0_offset, 0);
    append(nca, pfs0);
    pad_to(nca, kMediaUnit);

    FsHeader& fs = header.fs_header[index];
    fs.version = 2;
    fs.fs_type = kFsTypePfs0;
    fs.hash_type = kHashHierarchicalSha256;
    fs.encryption_type = kEncryptionNone;
    auto& data = fs.hash_data.sha256;
    const Sha256Digest master = sha256(hash_table.data(), hash_table.size());
    std::memcpy(data.master_hash, master.data(), master.size());
    data.block_size = block_size;
    data.layer_count = 2;
    data.hash_layer.offset = 0;
    data.hash_layer.size = hash_table.size();
    data.pfs0_layer.offset = pfs0_offset;
    data.pfs0_layer.size = pfs0.size();
    finish_section(header, index, start, nca.size());
}

// A romfs section: the romfs under six levels of IVFC hashes.
void write_romfs_section(NcaHeader& header, int index, const std::vector<RomfsFile>& files,
                         std::vector<uint8_t>& nca) {
    FsHeader& fs = header.fs_header[index];
    fs.version = 2;
    fs.fs_type = kFsTypeRomfs;
    fs.hash_type = kHashHierarchicalIntegrity;
    fs.encryption_type = kEncryptionNone;
    auto& info = fs.hash_data.integrity;
    info.magic = 0x43465649;  // "IVFC"
    info.version = 0x20000;
    info.master_hash_size = 0x20;
    info.max_layers = 7;

    std::vector<uint8_t> levels[kIvfcLevels];
    uint64_t romfs_size = 0;
    levels[kIvfcLevels - 1] = build_romfs(files, &romfs_size);
    info.levels[kIvfcLevels - 1].hash_data_size = romfs_size;
    for (int level = int(kIvfcLevels) - 2; level >= 0; --level) {
        levels[level] = ivfc_level_above(levels[level + 1]);
        info.levels[level].hash_data_size = levels[level].size();
    }
    uint64_t logical = 0;
    for (uint32_t level = 0; level < kIvfcLevels; ++level) {
        info.levels[level].logical_offset = logical;
        info.levels[level].block_size = 0x0E;  // 0x4000
        logical += info.levels[level].hash_data_size;
    }
    const Sha256Digest master = sha256(levels[0].data(), levels[0].size());
    std::memcpy(info.master_hash, master.data(), master.size());

    const uint64_t start = section_start(header, index);
    for (const auto& level : levels) append(nca, level);
    pad_to(nca, kMediaUnit);
    finish_section(header, index, start, nca.size());
}

BuiltNca finish_nca(NcaHeader& header, uint8_t nca_content_type, uint64_t title_id, ContentType type,
                    std::vector<uint8_t> nca, const EncryptNcaHeader& encrypt) {
    header.magic = kNca3Magic;
    header.distribution_type = 0;  // System
    header.content_type = nca_content_type;
    header.program_id = title_id;
    header.sdk_version = 0x000C1100;
    header.size = nca.size();
    std::memcpy(nca.data(), &header, sizeof(header));
    if (encrypt) encrypt(nca.data());
    BuiltNca built;
    built.hash = sha256(nca.data(), nca.size());
    built.data = std::move(nca);
    built.type = type;
    return built;
}

ContentInfo content_info(const BuiltNca& nca) {
    ContentInfo info{};
    std::memcpy(info.content_id, nca.hash.data(), sizeof(info.content_id));
    info.size_low = static_cast<uint32_t>(nca.data.size());
    info.size_high = static_cast<uint8_t>(uint64_t(nca.data.size()) >> 32);
    info.content_type = static_cast<uint8_t>(nca.type);
    return info;
}

template <typename T>
bool read_at(const std::vector<uint8_t>& bytes, std::size_t offset, T& out) {
    if (offset > bytes.size() || bytes.size() - offset < sizeof(T)) return false;
    std::memcpy(&out, bytes.data() + offset, sizeof(T));
    return true;
}

template <typename T>
void write_at(std::vector<uint8_t>& bytes, std::size_t offset, const T& value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(T));
}

// Replaces the first kernel capability of the kind `low_ones` names (a
// capability's kind is how many of its low bits are set, followed by a 0) with
// `value`. Sphaira's npdm_patch_kc.
bool patch_kernel_capability(std::vector<uint8_t>& npdm, std::size_t offset, std::size_t size,
                             uint32_t low_ones, uint32_t value) {
    const uint32_t pattern = (1u << low_ones) - 1;
    const uint32_t mask = (1u << low_ones) | pattern;
    if (offset > npdm.size() || npdm.size() - offset < size) return false;
    for (std::size_t i = 0; i + 4 <= size; i += 4) {
        uint32_t capability = 0;
        std::memcpy(&capability, npdm.data() + offset + i, 4);
        if ((capability & mask) == pattern) {
            capability = value | pattern;
            std::memcpy(npdm.data() + offset + i, &capability, 4);
            return true;
        }
    }
    return false;
}

void put_string(std::vector<uint8_t>& nacp, std::size_t offset, std::size_t size, const std::string& text) {
    std::memset(nacp.data() + offset, 0, size);
    std::memcpy(nacp.data() + offset, text.data(), std::min(text.size(), size - 1));
}

}  // namespace

std::vector<uint8_t> build_romfs(const std::vector<RomfsFile>& files, uint64_t* size) {
    // Every file in the root, in the order given, each one's sibling the next.
    const uint32_t file_count = static_cast<uint32_t>(files.size());
    const uint32_t dir_buckets = romfs_hash_table_count(1);
    const uint32_t file_buckets = romfs_hash_table_count(file_count);
    std::vector<uint32_t> dir_hash_table(dir_buckets, kRomfsEmpty);
    std::vector<uint32_t> file_hash_table(file_buckets, kRomfsEmpty);

    std::vector<uint32_t> entry_offsets;
    std::vector<uint64_t> data_offsets;
    uint32_t entry_offset = 0;
    uint64_t data_size = 0;
    for (const RomfsFile& file : files) {
        entry_offsets.push_back(entry_offset);
        entry_offset += sizeof(RomfsFileEntry) +
                        static_cast<uint32_t>(align_up(file.name.size() - 1, 4));
        data_size = align_up(data_size, 0x10);
        data_offsets.push_back(data_size);
        data_size += file.data.size();
    }

    std::vector<uint8_t> file_table;
    for (uint32_t i = 0; i < file_count; ++i) {
        const std::string name = files[i].name.substr(1);
        const uint32_t hash = romfs_path_hash(0, name) % file_buckets;
        RomfsFileEntry entry{};
        entry.parent = 0;
        entry.sibling = i + 1 < file_count ? entry_offsets[i + 1] : kRomfsEmpty;
        entry.data_offset = data_offsets[i];
        entry.data_size = files[i].data.size();
        entry.next_hash = file_hash_table[hash];
        file_hash_table[hash] = entry_offsets[i];
        entry.name_length = static_cast<uint32_t>(name.size());
        append(file_table, entry);
        file_table.insert(file_table.end(), name.begin(), name.end());
        pad_to(file_table, 4);
    }

    std::vector<uint8_t> dir_table;
    RomfsDirEntry root{};
    root.parent = 0;
    root.sibling = kRomfsEmpty;
    root.child_dir = kRomfsEmpty;
    root.child_file = file_count > 0 ? 0 : kRomfsEmpty;
    const uint32_t root_hash = romfs_path_hash(0, "") % dir_buckets;
    root.next_hash = dir_hash_table[root_hash];
    dir_hash_table[root_hash] = 0;
    append(dir_table, root);

    RomfsHeader header{};
    header.header_size = sizeof(RomfsHeader);
    header.file_data_offset = kRomfsFileDataOffset;
    header.dir_hash_table_offset = align_up(kRomfsFileDataOffset + data_size, 4);
    header.dir_hash_table_size = dir_buckets * 4;
    header.dir_table_offset = header.dir_hash_table_offset + header.dir_hash_table_size;
    header.dir_table_size = dir_table.size();
    header.file_hash_table_offset = header.dir_table_offset + header.dir_table_size;
    header.file_hash_table_size = file_buckets * 4;
    header.file_table_offset = header.file_hash_table_offset + header.file_hash_table_size;
    header.file_table_size = file_table.size();

    std::vector<uint8_t> romfs;
    append(romfs, header);
    for (uint32_t i = 0; i < file_count; ++i) {
        romfs.resize(kRomfsFileDataOffset + data_offsets[i], 0);
        append(romfs, files[i].data);
    }
    romfs.resize(header.dir_hash_table_offset, 0);
    for (const uint32_t bucket : dir_hash_table) append(romfs, bucket);
    append(romfs, dir_table);
    for (const uint32_t bucket : file_hash_table) append(romfs, bucket);
    append(romfs, file_table);

    if (size != nullptr) *size = romfs.size();
    pad_to(romfs, kIvfcBlockSize);
    return romfs;
}

bool patch_npdm(std::vector<uint8_t>& npdm, uint64_t title_id, bool force_debug) {
    NpdmMeta meta{};
    NpdmAci0 aci0{};
    NpdmAcid acid{};
    if (!read_at(npdm, 0, meta) || meta.magic != 0x4154454D ||
        !read_at(npdm, meta.aci0_offset, aci0) || aci0.magic != 0x30494341 ||
        !read_at(npdm, meta.acid_offset, acid) || acid.magic != 0x44494341) {
        return false;
    }

    std::memset(meta.title_name, 0, sizeof(meta.title_name));
    std::memcpy(meta.title_name, "Application", 11);
    std::memset(meta.product_code, 0, sizeof(meta.product_code));
    aci0.program_id = title_id;
    acid.program_id_min = title_id;
    acid.program_id_max = title_id;

    // Atmosphère 1.8.0 moved the kernel's debug flags for HOS 19.0.0; there,
    // hbloader needs force_debug rather than the force_debug_prod hbl.json
    // asks for. Sphaira's "Automatic", which picks by Atmosphère's version.
    if (force_debug) {
        patch_kernel_capability(npdm, meta.aci0_offset + aci0.kac_offset, aci0.kac_size, 16, 1u << 19);
        patch_kernel_capability(npdm, meta.acid_offset + acid.kac_offset, acid.kac_size, 16, 1u << 19);
    }

    write_at(npdm, 0, meta);
    write_at(npdm, meta.aci0_offset, aci0);
    write_at(npdm, meta.acid_offset, acid);
    return true;
}

void patch_nacp(std::vector<uint8_t>& nacp, const ForwarderContents& contents, uint64_t title_id) {
    namespace at = nacp_offset;
    nacp.resize(kNacpSize, 0);
    for (std::size_t language = 0; language < at::kLanguageCount; ++language) {
        const std::size_t entry = language * at::kLanguageEntrySize;
        if (!contents.name.empty()) put_string(nacp, entry, at::kNameSize, contents.name);
        if (!contents.author.empty()) {
            put_string(nacp, entry + at::kNameSize, at::kAuthorSize, contents.author);
        }
    }

    nacp[at::kStartupUserAccount] = 0;            // no profile to pick
    nacp[at::kUserAccountSwitchLock] = 0;
    nacp[at::kAddOnContentRegistrationType] = 1;  // on demand
    nacp[at::kScreenshot] = contents.screenshots ? 0 : 1;   // allow : deny
    nacp[at::kVideoCapture] = contents.video_capture ? 2 : 0;  // automatic : off
    nacp[at::kDataLossConfirmation] = 0;          // it keeps no save of its own
    nacp[at::kPlayLogPolicy] = 0;
    nacp[at::kLogoType] = 2;                      // Nintendo
    nacp[at::kLogoHandling] = 0;
    nacp[at::kPlayLogQueryCapability] = 0;
    nacp[at::kRequiredNetworkServiceLicenseOnLaunch] = 0;

    write_at(nacp, at::kPresenceGroupId, title_id);
    write_at(nacp, at::kSaveDataOwnerId, title_id);
    write_at(nacp, at::kPseudoDeviceIdSeed, title_id);
    write_at(nacp, at::kAddOnContentBaseId, title_id ^ 0x1000);
    for (int i = 0; i < 8; ++i) write_at(nacp, at::kLocalCommunicationId + i * 8, title_id);

    // No save data: the game's saves are Dualscreen's.
    const uint64_t none = 0;
    for (int i = 0; i < 4; ++i) {
        write_at(nacp, at::kUserAccountSaveDataSize + i * 8, none);
        write_at(nacp, at::kUserAccountSaveDataSizeMax + i * 8, none);
    }
}

std::string forwarder_argv(const ForwarderContents& contents) {
    return contents.arguments.empty() ? contents.nro_path : contents.nro_path + ' ' + contents.arguments;
}

uint64_t forwarder_title_id(const ForwarderContents& contents) {
    const std::string hashed = contents.nro_path + forwarder_argv(contents);
    const Sha256Digest hash = sha256(hashed.data(), hashed.size());
    uint64_t first = 0;
    for (int i = 0; i < 8; ++i) first |= uint64_t(hash[i]) << (i * 8);
    return 0x0500000000000000ull | (first & 0x00FFFFFFFFFFF000ull);
}

bool build_forwarder(const ForwarderContents& contents, uint8_t storage_id,
                     const EncryptNcaHeader& encrypt, ForwarderPackage& out, std::string* error) {
    const auto fail = [&](const std::string& why) {
        if (error != nullptr) *error = why;
        return false;
    };
    const std::string argv = forwarder_argv(contents);
    if (contents.nro_path.empty()) return fail("there is no .nro to start");
    if (contents.nro_path.size() > kMaxNroPath) return fail("the .nro's path is too long");
    if (argv.size() > kMaxArgv) return fail("the game's path is too long");
    if (contents.icon.empty()) return fail("there is no icon");
    if (contents.nacp.size() != kNacpSize) return fail("the .nro's control data could not be read");
    if (contents.hbl_main.empty() || contents.hbl_npdm.empty()) {
        return fail("this build has no hbloader to put in it");
    }

    out = ForwarderPackage{};
    out.title_id = forwarder_title_id(contents);

    // Program: hbloader, and what it is to start.
    {
        std::vector<RomfsFile> exefs{{"main", contents.hbl_main}, {"main.npdm", contents.hbl_npdm}};
        if (!patch_npdm(exefs[1].data, out.title_id, contents.force_debug)) {
            return fail("hbloader's main.npdm is not one this understands");
        }
        std::vector<RomfsFile> romfs{
            {"/nextArgv", std::vector<uint8_t>(argv.begin(), argv.end())},
            {"/nextNroPath", std::vector<uint8_t>(contents.nro_path.begin(), contents.nro_path.end())},
        };
        NcaHeader header{};
        std::vector<uint8_t> nca(sizeof(header), 0);
        write_pfs0_section(header, 0, exefs, kExefsHashBlockSize, nca);
        write_romfs_section(header, 1, romfs, nca);
        out.ncas.push_back(finish_nca(header, kNcaContentProgram, out.title_id, ContentType::Program,
                                      std::move(nca), encrypt));
    }

    // Control: the name, the publisher and the icon.
    {
        std::vector<uint8_t> nacp = contents.nacp;
        patch_nacp(nacp, contents, out.title_id);
        std::vector<RomfsFile> romfs{{"/control.nacp", nacp}, {"/icon_AmericanEnglish.dat", contents.icon}};
        NcaHeader header{};
        std::vector<uint8_t> nca(sizeof(header), 0);
        write_romfs_section(header, 0, romfs, nca);
        out.ncas.push_back(finish_nca(header, kNcaContentControl, out.title_id, ContentType::Control,
                                      std::move(nca), encrypt));
    }

    // Meta: which title, and which NCAs.
    CnmtHeader cnmt{};
    cnmt.title_id = out.title_id;
    cnmt.title_version = 0;
    cnmt.meta_type = kContentMetaTypeApplication;
    cnmt.meta_header.extended_header_size = sizeof(ApplicationMetaExtendedHeader);
    cnmt.meta_header.content_count = 2;
    cnmt.meta_header.content_meta_count = 1;
    cnmt.meta_header.storage_id = storage_id;
    ApplicationMetaExtendedHeader extended{};
    extended.patch_id = out.title_id | 0x800;
    {
        std::vector<uint8_t> cnmt_file;
        append(cnmt_file, cnmt);
        append(cnmt_file, extended);
        for (const BuiltNca& nca : out.ncas) {
            PackagedContentInfo packaged{};
            std::memcpy(packaged.hash, nca.hash.data(), nca.hash.size());
            packaged.info = content_info(nca);
            append(cnmt_file, packaged);
        }
        cnmt_file.resize(cnmt_file.size() + 0x20, 0);  // the digest

        char name[40];
        std::snprintf(name, sizeof(name), "Application_%016llX.cnmt",
                      static_cast<unsigned long long>(out.title_id));
        NcaHeader header{};
        std::vector<uint8_t> nca(sizeof(header), 0);
        write_pfs0_section(header, 0, {{name, cnmt_file}}, kMetaHashBlockSize, nca);
        out.ncas.push_back(finish_nca(header, kNcaContentMeta, out.title_id, ContentType::Meta,
                                      std::move(nca), encrypt));
    }

    out.meta_key.id = out.title_id;
    out.meta_key.version = cnmt.title_version;
    out.meta_key.type = cnmt.meta_type;
    out.meta_key.install_type = kInstallTypeFull;

    out.meta_data.header = cnmt.meta_header;
    out.meta_data.header.content_count = 3;
    out.meta_data.header.storage_id = 0;
    out.meta_data.extended = extended;
    out.meta_data.infos[0] = content_info(out.ncas[2]);
    out.meta_data.infos[1] = content_info(out.ncas[0]);
    out.meta_data.infos[2] = content_info(out.ncas[1]);

    out.storage_record.key = out.meta_key;
    out.storage_record.storage_id = storage_id;
    return true;
}

}  // namespace ds::forwarder
