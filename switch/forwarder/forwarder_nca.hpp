// SPDX-License-Identifier: GPL-3.0-or-later
//
// A forwarder, packed: the three content archives (NCAs) a title on the
// Switch's HOME menu is made of, and what the console's content manager is
// told about them. Adapted from Sphaira's owo.cpp (itself after hacbrewpack's
// romfs building and yati's installing), which is GPL-3.0 -- so this file is
// GPL-3.0-or-later, like switch/melonds_deko, and unlike the rest of this
// repository. See THIRD_PARTY.md.
//
//   program  hbloader (exefs/main and its main.npdm, patched with the title's
//            id), and a romfs holding the .nro it starts and the command line
//            it starts it with -- Dualscreen, and the ROM.
//   control  control.nacp (the name, the publisher, whether the capture button
//            works) and icon_AmericanEnglish.dat, the icon.
//   meta     the content meta: which title these are, and which NCAs.
//
// Nothing here talks to the console: the header key the NCAs are encrypted
// with, and writing them, are forwarder_install.cpp's. So all of it is built
// and tested on a computer too (tests/test_forwarder_nca.cpp).

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ds::forwarder {

using Sha256Digest = std::array<uint8_t, 32>;
Sha256Digest sha256(const void* data, std::size_t size);

// The sizes the console's formats fix.
constexpr std::size_t kNacpSize = 0x4000;
constexpr std::size_t kNcaHeaderSize = 0xC00;
// What hbloader has room for: the command line, and the .nro's path.
constexpr std::size_t kMaxArgv = 2048 - 1;
constexpr std::size_t kMaxNroPath = 0x301 - 1;

// The values libnx's NcmContentType and NcmStorageId give them.
enum class ContentType : uint8_t { Meta = 0, Program = 1, Control = 3 };
constexpr uint8_t kStorageSdCard = 5;
constexpr uint8_t kStorageBuiltInUser = 4;

struct ForwarderContents {
    // The .nro hbloader starts, as the homebrew menu names it (sdmc:/...).
    std::string nro_path;
    // What follows its path on the command line.
    std::string arguments;
    // What the HOME menu shows. Empty keeps what `nacp` says.
    std::string name;
    std::string author;
    // A 256x256 JPEG.
    std::vector<uint8_t> icon;
    // The control data to start from -- the .nro's own -- kNacpSize bytes.
    std::vector<uint8_t> nacp;
    // hbloader: its NSO, and its NPDM as npdmtool made it.
    std::vector<uint8_t> hbl_main;
    std::vector<uint8_t> hbl_npdm;
    bool screenshots = true;
    bool video_capture = true;
    // Whether the NPDM asks for the kernel's newer force-debug flag rather
    // than the one hbl.json names -- see patch_npdm().
    bool force_debug = false;
};

// The command line hbloader passes on: the .nro's path, then the arguments.
std::string forwarder_argv(const ForwarderContents& contents);
// The title id, the same way Sphaira makes it, from the path and the command
// line: the same forwarder made twice is one title, replaced.
uint64_t forwarder_title_id(const ForwarderContents& contents);

struct BuiltNca {
    std::vector<uint8_t> data;
    // The SHA-256 of all of it, as written; its first 16 bytes are its
    // content id.
    Sha256Digest hash{};
    ContentType type = ContentType::Program;
};

// Laid out byte for byte as libnx's structures, which the installer copies
// them into.
#pragma pack(push, 1)
struct ContentMetaKey {  // NcmContentMetaKey
    uint64_t id;
    uint32_t version;
    uint8_t type;
    uint8_t install_type;
    uint8_t padding[2];
};
struct ContentMetaHeader {  // NcmContentMetaHeader
    uint16_t extended_header_size;
    uint16_t content_count;
    uint16_t content_meta_count;
    uint8_t attributes;
    uint8_t storage_id;
};
struct ApplicationMetaExtendedHeader {  // NcmApplicationMetaExtendedHeader
    uint64_t patch_id;
    uint32_t required_system_version;
    uint32_t required_application_version;
};
struct ContentInfo {  // NcmContentInfo
    uint8_t content_id[0x10];
    uint32_t size_low;
    uint8_t size_high;
    uint8_t attr;
    uint8_t content_type;
    uint8_t id_offset;
};
struct PackagedContentInfo {  // NcmPackagedContentInfo
    uint8_t hash[0x20];
    ContentInfo info;
};
// What ncm's content meta database is given for the title.
struct ContentMetaData {
    ContentMetaHeader header;
    ApplicationMetaExtendedHeader extended;
    ContentInfo infos[3];  // meta, program, control
};
// What ns's application record is given: where the title's content lives.
struct ContentStorageRecord {
    ContentMetaKey key;
    uint8_t storage_id;
    uint8_t padding[7];
};
#pragma pack(pop)
static_assert(sizeof(ContentMetaKey) == 0x10);
static_assert(sizeof(ContentMetaHeader) == 0x8);
static_assert(sizeof(ApplicationMetaExtendedHeader) == 0x10);
static_assert(sizeof(ContentInfo) == 0x18);
static_assert(sizeof(PackagedContentInfo) == 0x38);
static_assert(sizeof(ContentMetaData) == 0x60);
static_assert(sizeof(ContentStorageRecord) == 0x18);

struct ForwarderPackage {
    uint64_t title_id = 0;
    // Program, control, meta: the order they are written in.
    std::vector<BuiltNca> ncas;
    ContentMetaKey meta_key{};
    ContentMetaData meta_data{};
    ContentStorageRecord storage_record{};
};

// Encrypts an NCA's first kNcaHeaderSize bytes in place, as the console
// expects them: AES-128-XTS with its header key, 0x200-byte sectors numbered
// from 0, Nintendo's tweak. The console's; a test passes its own.
using EncryptNcaHeader = std::function<void(uint8_t* header)>;

// Packs `contents` for `storage_id` (kStorageSdCard or kStorageBuiltInUser).
// False, with why, when something it needs is missing or will not fit.
bool build_forwarder(const ForwarderContents& contents, uint8_t storage_id,
                     const EncryptNcaHeader& encrypt, ForwarderPackage& out, std::string* error);

// The pieces, for the tests.
struct RomfsFile {
    std::string name;  // with its leading '/'
    std::vector<uint8_t> data;
};
// A romfs holding `files` in its root, padded to the IVFC's 0x4000 blocks;
// `size` is its length before the padding.
std::vector<uint8_t> build_romfs(const std::vector<RomfsFile>& files, uint64_t* size);
// hbl.json's NPDM, given the title's id. False when it is not an NPDM this
// understands.
bool patch_npdm(std::vector<uint8_t>& npdm, uint64_t title_id, bool force_debug);
// The .nro's control data, made the forwarder's.
void patch_nacp(std::vector<uint8_t>& nacp, const ForwarderContents& contents, uint64_t title_id);

// Where the fields patch_nacp() sets are in a NacpStruct. forwarder_install.cpp
// checks each against libnx's own definition.
namespace nacp_offset {
constexpr std::size_t kLanguageEntrySize = 0x300;
constexpr std::size_t kLanguageCount = 16;
constexpr std::size_t kNameSize = 0x200;
constexpr std::size_t kAuthorSize = 0x100;
constexpr std::size_t kStartupUserAccount = 0x3025;
constexpr std::size_t kUserAccountSwitchLock = 0x3026;
constexpr std::size_t kAddOnContentRegistrationType = 0x3027;
constexpr std::size_t kScreenshot = 0x3034;
constexpr std::size_t kVideoCapture = 0x3035;
constexpr std::size_t kDataLossConfirmation = 0x3036;
constexpr std::size_t kPlayLogPolicy = 0x3037;
constexpr std::size_t kPresenceGroupId = 0x3038;
constexpr std::size_t kAddOnContentBaseId = 0x3070;
constexpr std::size_t kSaveDataOwnerId = 0x3078;
constexpr std::size_t kUserAccountSaveDataSize = 0x3080;       // then its journal, the
                                                              // device's and its journal
constexpr std::size_t kLocalCommunicationId = 0x30B0;          // eight of them
constexpr std::size_t kLogoType = 0x30F0;
constexpr std::size_t kLogoHandling = 0x30F1;
constexpr std::size_t kPseudoDeviceIdSeed = 0x30F8;
constexpr std::size_t kUserAccountSaveDataSizeMax = 0x3148;    // the same four again
constexpr std::size_t kPlayLogQueryCapability = 0x3210;
constexpr std::size_t kRequiredNetworkServiceLicenseOnLaunch = 0x3213;
}  // namespace nacp_offset

}  // namespace ds::forwarder
