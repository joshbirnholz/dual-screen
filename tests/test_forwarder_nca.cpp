// A forwarder, packed (switch/forwarder/forwarder_nca.cpp): what the Switch is
// given reads back the way the console and hbloader read it -- the romfs
// looked up the way hbloader and Horizon look it up, every hash layer
// checked, and the content meta pointing at the NCAs actually made.

#include "test_support.hpp"

#include <cstring>
#include <string>
#include <vector>

#include "forwarder/forwarder_nca.hpp"

using namespace ds::forwarder;

namespace {

std::string hex(const Sha256Digest& digest) {
    static const char* digits = "0123456789abcdef";
    std::string text;
    for (const uint8_t byte : digest) {
        text += digits[byte >> 4];
        text += digits[byte & 15];
    }
    return text;
}

template <typename T>
T read(const std::vector<uint8_t>& bytes, std::size_t offset) {
    T value{};
    if (offset + sizeof(T) <= bytes.size()) std::memcpy(&value, bytes.data() + offset, sizeof(T));
    return value;
}

std::vector<uint8_t> slice(const std::vector<uint8_t>& bytes, std::size_t offset, std::size_t size) {
    if (offset > bytes.size() || bytes.size() - offset < size) return {};
    return std::vector<uint8_t>(bytes.begin() + offset, bytes.begin() + offset + size);
}

std::vector<uint8_t> bytes_of(const std::string& text) { return std::vector<uint8_t>(text.begin(), text.end()); }

void put32(std::vector<uint8_t>& bytes, std::size_t offset, uint32_t value) {
    std::memcpy(bytes.data() + offset, &value, 4);
}

// An NPDM shaped as npdmtool writes hbl.json: META, then ACID, then ACI0, each
// with kernel capabilities that include the debug flags (force_debug_prod).
std::vector<uint8_t> fake_npdm() {
    std::vector<uint8_t> npdm(0x80 + 0x240 + 0x10 + 0x40 + 0x10, 0);
    const uint32_t acid = 0x80, aci0 = 0x80 + 0x250;
    put32(npdm, 0x0, 0x4154454D);
    std::memcpy(npdm.data() + 0x20, "Application", 11);
    put32(npdm, 0x70, aci0);
    put32(npdm, 0x74, 0x50);
    put32(npdm, 0x78, acid);
    put32(npdm, 0x7C, 0x250);
    // ACID: kernel capabilities at +0x240, 0x10 bytes.
    put32(npdm, acid + 0x200, 0x44494341);
    put32(npdm, acid + 0x230, 0x240);
    put32(npdm, acid + 0x234, 0x10);
    put32(npdm, acid + 0x240, 0x020073B7);             // kernel flags
    put32(npdm, acid + 0x244, 0xFFFF | (1u << 18));    // debug flags: force_debug_prod
    put32(npdm, acid + 0x248, 0xFFFFFFFF);
    put32(npdm, acid + 0x24C, 0xFFFFFFFF);
    // ACI0: the same capabilities at +0x40.
    put32(npdm, aci0, 0x30494341);
    put32(npdm, aci0 + 0x30, 0x40);
    put32(npdm, aci0 + 0x34, 0x10);
    put32(npdm, aci0 + 0x40, 0x020073B7);
    put32(npdm, aci0 + 0x44, 0xFFFF | (1u << 18));
    put32(npdm, aci0 + 0x48, 0xFFFFFFFF);
    put32(npdm, aci0 + 0x4C, 0xFFFFFFFF);
    return npdm;
}

ForwarderContents contents_for(const std::string& rom) {
    ForwarderContents contents;
    contents.nro_path = "sdmc:/switch/Dualscreen.nro";
    contents.arguments = "--game \"" + rom + "\"";
    contents.name = "Mario Kart DS";
    contents.author = "Nintendo";
    contents.icon = {0xFF, 0xD8, 0xFF, 0xE0, 1, 2, 3, 0xFF, 0xD9};
    contents.nacp.assign(kNacpSize, 0);
    std::memcpy(contents.nacp.data(), "Dualscreen", 10);
    std::memcpy(contents.nacp.data() + 0x3060, "0.1.0", 5);  // display version, kept
    contents.hbl_main.assign(0x23456, 0);
    for (std::size_t i = 0; i < contents.hbl_main.size(); ++i) contents.hbl_main[i] = uint8_t(i * 7);
    contents.hbl_npdm = fake_npdm();
    return contents;
}

// A file in a romfs, found the way hbloader finds it -- the root's first file,
// then each one's sibling -- and the way Horizon does, through the hash table.
// Both have to agree.
bool romfs_file(const std::vector<uint8_t>& romfs, const std::string& name, std::vector<uint8_t>& out) {
    const uint64_t dir_table = read<uint64_t>(romfs, 0x18);
    const uint64_t file_hash_table = read<uint64_t>(romfs, 0x28);
    const uint64_t file_hash_size = read<uint64_t>(romfs, 0x30);
    const uint64_t file_table = read<uint64_t>(romfs, 0x38);
    const uint64_t data = read<uint64_t>(romfs, 0x48);
    if (read<uint64_t>(romfs, 0) != 0x50 || data != 0x200) return false;

    uint32_t by_sibling = 0xFFFFFFFF;
    for (uint32_t offset = read<uint32_t>(romfs, dir_table + 12); offset != 0xFFFFFFFF;
         offset = read<uint32_t>(romfs, file_table + offset + 4)) {
        const uint32_t length = read<uint32_t>(romfs, file_table + offset + 0x1C);
        if (std::string(reinterpret_cast<const char*>(romfs.data() + file_table + offset + 0x20), length) == name) {
            by_sibling = offset;
            break;
        }
    }

    uint32_t hash = 123456789;
    for (const char c : name) {
        hash = (hash >> 5) | (hash << 27);
        hash ^= uint8_t(c);
    }
    uint32_t by_hash = 0xFFFFFFFF;
    const uint32_t buckets = uint32_t(file_hash_size / 4);
    for (uint32_t offset = read<uint32_t>(romfs, file_hash_table + (hash % buckets) * 4); offset != 0xFFFFFFFF;
         offset = read<uint32_t>(romfs, file_table + offset + 0x18)) {
        const uint32_t length = read<uint32_t>(romfs, file_table + offset + 0x1C);
        if (std::string(reinterpret_cast<const char*>(romfs.data() + file_table + offset + 0x20), length) == name) {
            by_hash = offset;
            break;
        }
    }
    if (by_sibling == 0xFFFFFFFF || by_sibling != by_hash) return false;
    out = slice(romfs, data + read<uint64_t>(romfs, file_table + by_hash + 8),
                read<uint64_t>(romfs, file_table + by_hash + 0x10));
    return true;
}

// A PFS0 file by name.
bool pfs0_file(const std::vector<uint8_t>& pfs0, const std::string& name, std::vector<uint8_t>& out) {
    if (read<uint32_t>(pfs0, 0) != 0x30534650) return false;
    const uint32_t count = read<uint32_t>(pfs0, 4);
    const uint32_t strings = 0x10 + count * 0x18;
    const uint32_t data = strings + read<uint32_t>(pfs0, 8);
    for (uint32_t i = 0; i < count; ++i) {
        const std::size_t entry = 0x10 + i * 0x18;
        const char* entry_name = reinterpret_cast<const char*>(pfs0.data() + strings + read<uint32_t>(pfs0, entry + 0x10));
        if (name == entry_name) {
            out = slice(pfs0, data + read<uint64_t>(pfs0, entry), read<uint64_t>(pfs0, entry + 8));
            return true;
        }
    }
    return false;
}

// An NCA's section, its hashes checked from the header down: the header's
// hash of the section's own header, then the master hash and every layer.
// Gives back the PFS0 or the romfs inside.
bool open_section(const std::vector<uint8_t>& nca, int index, std::vector<uint8_t>& inner) {
    const std::size_t fs_header = 0x400 + index * 0x200;
    const std::vector<uint8_t> header_bytes = slice(nca, fs_header, 0x200);
    if (hex(sha256(header_bytes.data(), header_bytes.size())) !=
        hex(read<Sha256Digest>(nca, 0x280 + index * 0x20))) {
        return false;
    }
    const uint64_t start = uint64_t(read<uint32_t>(nca, 0x240 + index * 0x10)) * 0x200;
    const uint64_t end = uint64_t(read<uint32_t>(nca, 0x244 + index * 0x10)) * 0x200;
    if (start < 0xC00 || end > nca.size() || end <= start) return false;
    const std::vector<uint8_t> section = slice(nca, start, end - start);
    const uint8_t fs_type = nca[fs_header + 2];
    const uint8_t hash_type = nca[fs_header + 3];
    if (read<uint16_t>(nca, fs_header) != 2 || nca[fs_header + 4] != 1) return false;  // v2, no encryption

    if (fs_type == 1 && hash_type == 2) {
        const Sha256Digest master = read<Sha256Digest>(nca, fs_header + 8);
        const uint32_t block = read<uint32_t>(nca, fs_header + 0x28);
        const uint64_t table_size = read<uint64_t>(nca, fs_header + 0x38);
        const uint64_t pfs0_offset = read<uint64_t>(nca, fs_header + 0x40);
        const uint64_t pfs0_size = read<uint64_t>(nca, fs_header + 0x48);
        const std::vector<uint8_t> table = slice(section, 0, table_size);
        if (hex(sha256(table.data(), table.size())) != hex(master)) return false;
        inner = slice(section, pfs0_offset, pfs0_size);
        if (inner.empty() || pfs0_offset % 0x200 != 0) return false;
        for (std::size_t i = 0, n = 0; i < inner.size(); i += block, ++n) {
            const std::size_t size = std::min<std::size_t>(block, inner.size() - i);
            if (hex(sha256(inner.data() + i, size)) != hex(read<Sha256Digest>(table, n * 32))) return false;
        }
        return true;
    }
    if (fs_type == 0 && hash_type == 3) {
        const std::size_t ivfc = fs_header + 8;
        if (read<uint32_t>(nca, ivfc) != 0x43465649 || read<uint32_t>(nca, ivfc + 0xC) != 7) return false;
        std::vector<uint8_t> levels[6];
        for (int level = 0; level < 6; ++level) {
            const std::size_t info = ivfc + 0x10 + level * 0x18;
            if (read<uint32_t>(nca, info + 0x10) != 0x0E) return false;
            levels[level] = slice(section, read<uint64_t>(nca, info), read<uint64_t>(nca, info + 8));
            if (levels[level].empty()) return false;
        }
        if (hex(sha256(levels[0].data(), levels[0].size())) != hex(read<Sha256Digest>(nca, ivfc + 0xC0))) {
            return false;
        }
        for (int level = 1; level < 6; ++level) {
            const auto& data = levels[level];
            for (std::size_t i = 0, n = 0; i < data.size(); i += 0x4000, ++n) {
                std::vector<uint8_t> block = slice(data, i, std::min<std::size_t>(0x4000, data.size() - i));
                block.resize(0x4000, 0);  // a short last block is hashed whole, zeros after it
                if (hex(sha256(block.data(), block.size())) != hex(read<Sha256Digest>(levels[level - 1], n * 32))) {
                    return false;
                }
            }
        }
        inner = levels[5];
        return true;
    }
    return false;
}

void test_sha256_is_sha256() {
    CHECK(hex(sha256("", 0)) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(hex(sha256("abc", 3)) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    const std::string two_blocks = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    CHECK(hex(sha256(two_blocks.data(), two_blocks.size())) ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    const std::string million(1000000, 'a');
    CHECK(hex(sha256(million.data(), million.size())) ==
          "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

void test_the_title_id() {
    const ForwarderContents mario = contents_for("sdmc:/roms/nds/Mario Kart DS.nds");
    const ForwarderContents zelda = contents_for("sdmc:/roms/nds/Zelda Spirit Tracks.nds");
    const uint64_t id = forwarder_title_id(mario);
    // In the range Sphaira's forwarders use, on an application's boundary.
    CHECK((id >> 56) == 0x05);
    CHECK((id & 0xFFF) == 0);
    CHECK(forwarder_title_id(mario) == id);
    CHECK(forwarder_title_id(zelda) != id);
    CHECK(forwarder_argv(mario) == "sdmc:/switch/Dualscreen.nro --game \"sdmc:/roms/nds/Mario Kart DS.nds\"");
}

void test_the_romfs_reads_back() {
    std::vector<RomfsFile> files;
    files.push_back({"/nextArgv", bytes_of("one two")});
    files.push_back({"/nextNroPath", bytes_of("sdmc:/switch/Dualscreen.nro")});
    files.push_back({"/third", std::vector<uint8_t>(0x5000, 0xAB)});
    files.push_back({"/four", bytes_of("4")});
    uint64_t size = 0;
    const std::vector<uint8_t> romfs = build_romfs(files, &size);
    CHECK(romfs.size() % 0x4000 == 0);
    CHECK(size <= romfs.size());
    for (const RomfsFile& file : files) {
        std::vector<uint8_t> data;
        CHECK(romfs_file(romfs, file.name.substr(1), data));
        CHECK(data == file.data);
    }
    std::vector<uint8_t> missing;
    CHECK(!romfs_file(romfs, "nope", missing));
}

void test_the_npdm_is_the_titles() {
    std::vector<uint8_t> npdm = fake_npdm();
    CHECK(patch_npdm(npdm, 0x0512345678901000ull, false));
    CHECK(read<uint64_t>(npdm, 0x80 + 0x250 + 0x10) == 0x0512345678901000ull);  // ACI0
    CHECK(read<uint64_t>(npdm, 0x80 + 0x210) == 0x0512345678901000ull);          // ACID min
    CHECK(read<uint64_t>(npdm, 0x80 + 0x218) == 0x0512345678901000ull);          // and max
    CHECK(read<uint32_t>(npdm, 0x80 + 0x244) == (0xFFFFu | (1u << 18)));         // left alone

    std::vector<uint8_t> newer = fake_npdm();
    CHECK(patch_npdm(newer, 1, true));
    CHECK(read<uint32_t>(newer, 0x80 + 0x244) == (0xFFFFu | (1u << 19)));
    CHECK(read<uint32_t>(newer, 0x80 + 0x250 + 0x44) == (0xFFFFu | (1u << 19)));
    CHECK(read<uint32_t>(newer, 0x80 + 0x240) == 0x020073B7u);  // the kernel flags are not debug flags

    std::vector<uint8_t> junk(0x400, 0);
    CHECK(!patch_npdm(junk, 1, false));
}

void test_a_forwarder_is_packed() {
    const ForwarderContents contents = contents_for("sdmc:/roms/nds/Mario Kart DS.nds");
    int encrypted = 0;
    const auto encrypt = [&](uint8_t* header) {
        ++encrypted;
        (void)header;
    };
    ForwarderPackage package;
    std::string error;
    CHECK(build_forwarder(contents, kStorageSdCard, encrypt, package, &error));
    CHECK(error.empty());
    CHECK(encrypted == 3);
    CHECK(package.title_id == forwarder_title_id(contents));
    CHECK(package.ncas.size() == 3);
    if (package.ncas.size() != 3) return;

    const uint8_t nca_types[3] = {0, 2, 1};  // program, control, meta, as an NCA names them
    const ContentType types[3] = {ContentType::Program, ContentType::Control, ContentType::Meta};
    for (int i = 0; i < 3; ++i) {
        const BuiltNca& nca = package.ncas[i];
        CHECK(nca.type == types[i]);
        CHECK(hex(nca.hash) == hex(sha256(nca.data.data(), nca.data.size())));
        CHECK(read<uint32_t>(nca.data, 0x200) == 0x3341434E);
        CHECK(nca.data[0x205] == nca_types[i]);
        CHECK(read<uint64_t>(nca.data, 0x208) == nca.data.size());
        CHECK(read<uint64_t>(nca.data, 0x210) == package.title_id);
        CHECK(nca.data.size() % 0x200 == 0);
    }

    // The program: hbloader, patched with the title's id, and what it starts.
    std::vector<uint8_t> exefs, main, npdm, romfs, argv, nro;
    CHECK(open_section(package.ncas[0].data, 0, exefs));
    CHECK(pfs0_file(exefs, "main", main));
    CHECK(main == contents.hbl_main);
    CHECK(pfs0_file(exefs, "main.npdm", npdm));
    CHECK(read<uint64_t>(npdm, 0x80 + 0x250 + 0x10) == package.title_id);
    CHECK(open_section(package.ncas[0].data, 1, romfs));
    CHECK(romfs_file(romfs, "nextArgv", argv));
    CHECK(std::string(argv.begin(), argv.end()) == forwarder_argv(contents));
    CHECK(romfs_file(romfs, "nextNroPath", nro));
    CHECK(std::string(nro.begin(), nro.end()) == contents.nro_path);

    // The control data: the name and publisher in every language, the icon,
    // the capture button, no save data, the rest the .nro's own.
    std::vector<uint8_t> control, nacp, icon;
    CHECK(open_section(package.ncas[1].data, 0, control));
    CHECK(romfs_file(control, "control.nacp", nacp));
    CHECK(nacp.size() == kNacpSize);
    CHECK(romfs_file(control, "icon_AmericanEnglish.dat", icon));
    CHECK(icon == contents.icon);
    if (nacp.size() == kNacpSize) {
        for (int language = 0; language < 16; ++language) {
            CHECK(std::string(reinterpret_cast<const char*>(nacp.data() + language * 0x300)) == "Mario Kart DS");
            CHECK(std::string(reinterpret_cast<const char*>(nacp.data() + language * 0x300 + 0x200)) == "Nintendo");
        }
        CHECK(std::string(reinterpret_cast<const char*>(nacp.data() + 0x3060)) == "0.1.0");
        CHECK(nacp[nacp_offset::kScreenshot] == 0);
        CHECK(nacp[nacp_offset::kVideoCapture] == 2);
        CHECK(read<uint64_t>(nacp, nacp_offset::kSaveDataOwnerId) == package.title_id);
        CHECK(read<uint64_t>(nacp, nacp_offset::kUserAccountSaveDataSize) == 0);
    }

    // The meta: this title, and the two NCAs by their hashes.
    std::vector<uint8_t> meta, cnmt;
    CHECK(open_section(package.ncas[2].data, 0, meta));
    char cnmt_name[40];
    std::snprintf(cnmt_name, sizeof(cnmt_name), "Application_%016llX.cnmt",
                  static_cast<unsigned long long>(package.title_id));
    CHECK(pfs0_file(meta, cnmt_name, cnmt));
    CHECK(read<uint64_t>(cnmt, 0) == package.title_id);
    CHECK(cnmt[0xC] == 0x80);
    CHECK(read<uint16_t>(cnmt, 0x10) == 2);
    for (int i = 0; i < 2; ++i) {
        const std::size_t packaged = 0x30 + i * 0x38;
        CHECK(hex(read<Sha256Digest>(cnmt, packaged)) == hex(package.ncas[i].hash));
        CHECK(std::memcmp(cnmt.data() + packaged + 0x20, package.ncas[i].hash.data(), 0x10) == 0);
        CHECK(cnmt[packaged + 0x20 + 0x16] == uint8_t(package.ncas[i].type));
    }

    // What ncm and ns are told.
    CHECK(package.meta_key.id == package.title_id);
    CHECK(package.meta_key.type == 0x80);
    CHECK(package.meta_data.header.content_count == 3);
    CHECK(package.meta_data.header.storage_id == 0);
    CHECK(package.meta_data.extended.patch_id == (package.title_id | 0x800));
    const int order[3] = {2, 0, 1};  // meta, program, control
    for (int i = 0; i < 3; ++i) {
        const BuiltNca& nca = package.ncas[order[i]];
        CHECK(std::memcmp(package.meta_data.infos[i].content_id, nca.hash.data(), 0x10) == 0);
        CHECK(package.meta_data.infos[i].size_low == nca.data.size());
        CHECK(package.meta_data.infos[i].content_type == uint8_t(nca.type));
    }
    CHECK(package.storage_record.key.id == package.title_id);
    CHECK(package.storage_record.storage_id == kStorageSdCard);
}

void test_the_header_is_hashed_encrypted() {
    // The content id is the hash of the NCA as written, so of its header
    // encrypted, not of the header before.
    const ForwarderContents contents = contents_for("sdmc:/a.nds");
    ForwarderPackage plain, scrambled;
    CHECK(build_forwarder(contents, kStorageSdCard, nullptr, plain, nullptr));
    CHECK(build_forwarder(contents, kStorageSdCard,
                          [](uint8_t* header) {
                              for (std::size_t i = 0; i < kNcaHeaderSize; ++i) header[i] ^= 0x5A;
                          },
                          scrambled, nullptr));
    CHECK(plain.ncas.size() == 3 && scrambled.ncas.size() == 3);
    if (plain.ncas.size() != 3 || scrambled.ncas.size() != 3) return;
    CHECK(scrambled.ncas[0].data[0x200] == (plain.ncas[0].data[0x200] ^ 0x5A));
    CHECK(std::memcmp(scrambled.ncas[0].data.data() + kNcaHeaderSize, plain.ncas[0].data.data() + kNcaHeaderSize,
                      plain.ncas[0].data.size() - kNcaHeaderSize) == 0);
    CHECK(hex(scrambled.ncas[0].hash) != hex(plain.ncas[0].hash));
}

void test_what_cannot_be_packed() {
    ForwarderPackage package;
    std::string error;
    ForwarderContents no_icon = contents_for("sdmc:/a.nds");
    no_icon.icon.clear();
    CHECK(!build_forwarder(no_icon, kStorageSdCard, nullptr, package, &error));
    CHECK(!error.empty());

    ForwarderContents long_path = contents_for("sdmc:/" + std::string(3000, 'x') + ".nds");
    CHECK(!build_forwarder(long_path, kStorageSdCard, nullptr, package, &error));
    CHECK(error == "the game's path is too long");

    ForwarderContents no_hbl = contents_for("sdmc:/a.nds");
    no_hbl.hbl_main.clear();
    CHECK(!build_forwarder(no_hbl, kStorageSdCard, nullptr, package, &error));
    CHECK(error == "this build has no hbloader to put in it");
}

}  // namespace

int main() {
    test_sha256_is_sha256();
    test_the_title_id();
    test_the_romfs_reads_back();
    test_the_npdm_is_the_titles();
    test_a_forwarder_is_packed();
    test_the_header_is_hashed_encrypted();
    test_what_cannot_be_packed();
    return test::finish("forwarder_nca");
}
