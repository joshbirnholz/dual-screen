// SPDX-License-Identifier: GPL-3.0-or-later
//
// Adapted from Sphaira's owo.cpp and yati (https://github.com/ITotalJustice/sphaira,
// GPL-3.0). See forwarder_install.hpp.

#include "forwarder_install.hpp"

#include <switch.h>

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <vector>

#include "util/log.hpp"

// hbloader, built by forwarder/hbl/Makefile and embedded by switch/Makefile.
namespace ds::assets {
extern const unsigned char kForwarderHblMain[];
extern const unsigned long kForwarderHblMain_size;
extern const unsigned char kForwarderHblNpdm[];
extern const unsigned long kForwarderHblNpdm_size;
}  // namespace ds::assets

namespace ds::forwarder {

// The layouts forwarder_nca.hpp copies, against libnx's own.
static_assert(sizeof(ContentMetaKey) == sizeof(NcmContentMetaKey));
static_assert(sizeof(ContentMetaHeader) == sizeof(NcmContentMetaHeader));
static_assert(sizeof(ApplicationMetaExtendedHeader) == sizeof(NcmApplicationMetaExtendedHeader));
static_assert(sizeof(ContentInfo) == sizeof(NcmContentInfo));
static_assert(sizeof(PackagedContentInfo) == sizeof(NcmPackagedContentInfo));
static_assert(sizeof(NacpStruct) == kNacpSize);
static_assert(sizeof(NacpLanguageEntry) == nacp_offset::kLanguageEntrySize);
static_assert(offsetof(NacpStruct, startup_user_account) == nacp_offset::kStartupUserAccount);
static_assert(offsetof(NacpStruct, user_account_switch_lock) == nacp_offset::kUserAccountSwitchLock);
static_assert(offsetof(NacpStruct, add_on_content_registration_type) ==
              nacp_offset::kAddOnContentRegistrationType);
static_assert(offsetof(NacpStruct, screenshot) == nacp_offset::kScreenshot);
static_assert(offsetof(NacpStruct, video_capture) == nacp_offset::kVideoCapture);
static_assert(offsetof(NacpStruct, data_loss_confirmation) == nacp_offset::kDataLossConfirmation);
static_assert(offsetof(NacpStruct, play_log_policy) == nacp_offset::kPlayLogPolicy);
static_assert(offsetof(NacpStruct, presence_group_id) == nacp_offset::kPresenceGroupId);
static_assert(offsetof(NacpStruct, add_on_content_base_id) == nacp_offset::kAddOnContentBaseId);
static_assert(offsetof(NacpStruct, save_data_owner_id) == nacp_offset::kSaveDataOwnerId);
static_assert(offsetof(NacpStruct, user_account_save_data_size) == nacp_offset::kUserAccountSaveDataSize);
static_assert(offsetof(NacpStruct, device_save_data_journal_size) ==
              nacp_offset::kUserAccountSaveDataSize + 24);
static_assert(offsetof(NacpStruct, local_communication_id) == nacp_offset::kLocalCommunicationId);
static_assert(offsetof(NacpStruct, logo_type) == nacp_offset::kLogoType);
static_assert(offsetof(NacpStruct, logo_handling) == nacp_offset::kLogoHandling);
static_assert(offsetof(NacpStruct, pseudo_device_id_seed) == nacp_offset::kPseudoDeviceIdSeed);
static_assert(offsetof(NacpStruct, user_account_save_data_size_max) ==
              nacp_offset::kUserAccountSaveDataSizeMax);
static_assert(offsetof(NacpStruct, device_save_data_journal_size_max) ==
              nacp_offset::kUserAccountSaveDataSizeMax + 24);
static_assert(offsetof(NacpStruct, play_log_query_capability) == nacp_offset::kPlayLogQueryCapability);
static_assert(offsetof(NacpStruct, required_network_service_license_on_launch) ==
              nacp_offset::kRequiredNetworkServiceLicenseOnLaunch);

namespace {

// The seeds the console turns into its NCA header key, which it never hands
// out itself: the same for every console. Sphaira's keys.cpp.
constexpr uint8_t kHeaderKekSource[0x10] = {
    0x1F, 0x12, 0x91, 0x3A, 0x4A, 0xCB, 0xF0, 0x0D, 0x4C, 0xDE, 0x3A, 0xF6, 0xD5, 0x23, 0x88, 0x2A,
};
constexpr uint8_t kHeaderKeySource[0x20] = {
    0x5A, 0x3E, 0xD8, 0x4F, 0xDE, 0xC0, 0xD8, 0x26, 0x31, 0xF7, 0xE2, 0x5D, 0x19, 0x7B, 0xF5, 0xD0,
    0x1C, 0x9B, 0x7B, 0xFA, 0xF6, 0x28, 0x18, 0x3D, 0x71, 0xF6, 0x4D, 0x73, 0xF1, 0x50, 0xB9, 0xD2,
};

constexpr std::size_t kWriteChunk = 1024 * 1024;

// A result as the console's own error screens write it: 2XXX-YYYY.
std::string describe(Result result) {
    char text[32];
    std::snprintf(text, sizeof(text), "%04u-%04u", 2000 + R_MODULE(result), R_DESCRIPTION(result));
    return text;
}

// A service for the length of a scope.
template <typename Exit>
struct Scoped {
    Exit exit;
    bool active = false;
    ~Scoped() {
        if (active) exit();
    }
};
template <typename Exit>
Scoped<Exit> scoped(bool active, Exit exit) {
    return Scoped<Exit>{exit, active};
}

// The control data in the .nro's asset section, after its code.
bool read_nro_nacp(const std::string& nro_path, std::vector<uint8_t>& nacp) {
    FILE* file = std::fopen(nro_path.c_str(), "rb");
    if (file == nullptr) return false;
    NroStart start{};
    NroHeader header{};
    NroAssetHeader assets{};
    bool ok = std::fread(&start, sizeof(start), 1, file) == 1 && std::fread(&header, sizeof(header), 1, file) == 1 &&
              header.magic == 0x304F524E &&  // "NRO0"
              std::fseek(file, static_cast<long>(header.size), SEEK_SET) == 0 &&
              std::fread(&assets, sizeof(assets), 1, file) == 1 && assets.magic == 0x54455341 &&  // "ASET"
              assets.nacp.size == sizeof(NacpStruct);
    if (ok) {
        nacp.assign(sizeof(NacpStruct), 0);
        ok = std::fseek(file, static_cast<long>(header.size + assets.nacp.offset), SEEK_SET) == 0 &&
             std::fread(nacp.data(), nacp.size(), 1, file) == 1;
    }
    std::fclose(file);
    return ok;
}

// Whether this is Atmosphère 1.8.0 or later, where hbloader needs the
// kernel's newer force-debug flag -- Sphaira's "Automatic".
bool wants_force_debug() {
    if (R_FAILED(splInitialize())) return false;
    u64 version = 0;
    const bool known = R_SUCCEEDED(splGetConfig(static_cast<SplConfigItem>(65000), &version));  // ExosphereVersion
    splExit();
    return known && (version >> 40) >= MAKEHOSVERSION(1, 8, 0);
}

bool header_key(uint8_t key[0x20], std::string* error) {
    uint8_t kek[0x10];
    Result result = splCryptoGenerateAesKek(kHeaderKekSource, 0, 0, kek);
    if (R_SUCCEEDED(result)) result = splCryptoGenerateAesKey(kek, kHeaderKeySource, key);
    if (R_SUCCEEDED(result)) result = splCryptoGenerateAesKey(kek, kHeaderKeySource + 0x10, key + 0x10);
    if (R_FAILED(result)) {
        if (error != nullptr) *error = "the console would not make its NCA key (" + describe(result) + ")";
        return false;
    }
    return true;
}

// ns's IApplicationManagerInterface: PushApplicationRecord (16) and
// InvalidateApplicationControlCache (404), which libnx does not wrap.
// Sphaira's yati/nx/ns.cpp.
Result push_application_record(Service* manager, u64 title_id, const ContentStorageRecord& record) {
    const struct {
        u8 last_modified_event;
        u8 padding[7];
        u64 title_id;
    } in = {0x3 /* Installed */, {0}, title_id};
    return serviceDispatchIn(manager, 16, in, .buffer_attrs = {SfBufferAttr_HipcMapAlias | SfBufferAttr_In},
                             .buffers = {{&record, sizeof(record)}});
}

Result invalidate_control_cache(Service* manager, u64 title_id) {
    return serviceDispatchIn(manager, 404, title_id);
}

bool write_ncas(const ForwarderPackage& package, std::string* error) {
    NcmContentStorage storage{};
    Result result = ncmOpenContentStorage(&storage, NcmStorageId_SdCard);
    if (R_FAILED(result)) {
        if (error != nullptr) *error = "the SD card's content storage would not open (" + describe(result) + ")";
        return false;
    }
    for (const BuiltNca& nca : package.ncas) {
        NcmContentId content_id{};
        std::memcpy(content_id.c, nca.hash.data(), sizeof(content_id.c));
        NcmPlaceHolderId placeholder{};
        result = ncmContentStorageGeneratePlaceHolderId(&storage, &placeholder);
        if (R_SUCCEEDED(result)) {
            ncmContentStorageDeletePlaceHolder(&storage, &placeholder);
            result = ncmContentStorageCreatePlaceHolder(&storage, &content_id, &placeholder,
                                                        static_cast<s64>(nca.data.size()));
        }
        for (std::size_t offset = 0; R_SUCCEEDED(result) && offset < nca.data.size(); offset += kWriteChunk) {
            const std::size_t size = std::min(kWriteChunk, nca.data.size() - offset);
            result = ncmContentStorageWritePlaceHolder(&storage, &placeholder, offset, nca.data.data() + offset, size);
        }
        if (R_SUCCEEDED(result)) {
            // One made before, identical, is already there under this id.
            ncmContentStorageDelete(&storage, &content_id);
            result = ncmContentStorageRegister(&storage, &content_id, &placeholder);
        }
        if (R_FAILED(result)) {
            ncmContentStorageDeletePlaceHolder(&storage, &placeholder);
            ncmContentStorageClose(&storage);
            if (error != nullptr) *error = "the forwarder could not be written to the SD card (" + describe(result) + ")";
            return false;
        }
    }
    ncmContentStorageClose(&storage);

    NcmContentMetaDatabase database{};
    result = ncmOpenContentMetaDatabase(&database, NcmStorageId_SdCard);
    if (R_SUCCEEDED(result)) {
        NcmContentMetaKey key{};
        std::memcpy(&key, &package.meta_key, sizeof(key));
        result = ncmContentMetaDatabaseSet(&database, &key, &package.meta_data, sizeof(package.meta_data));
        if (R_SUCCEEDED(result)) result = ncmContentMetaDatabaseCommit(&database);
        ncmContentMetaDatabaseClose(&database);
    }
    if (R_FAILED(result)) {
        if (error != nullptr) *error = "the console would not record the forwarder (" + describe(result) + ")";
        return false;
    }
    return true;
}

}  // namespace

bool install_forwarder(ForwarderContents contents, std::string* error) {
    const auto fail = [&](const std::string& why) {
        if (error != nullptr) *error = why;
        log_warn("forwarder: " + why);
        return false;
    };

    if (!read_nro_nacp(contents.nro_path, contents.nacp)) {
        return fail("the control data in " + contents.nro_path + " could not be read");
    }
    contents.hbl_main.assign(assets::kForwarderHblMain, assets::kForwarderHblMain + assets::kForwarderHblMain_size);
    contents.hbl_npdm.assign(assets::kForwarderHblNpdm, assets::kForwarderHblNpdm + assets::kForwarderHblNpdm_size);
    contents.force_debug = wants_force_debug();

    Result result = splCryptoInitialize();
    if (R_FAILED(result)) return fail("the console's crypto service would not open (" + describe(result) + ")");
    uint8_t key[0x20];
    const bool have_key = header_key(key, error);
    splCryptoExit();
    if (!have_key) return fail(error != nullptr ? *error : "no NCA key");

    const auto encrypt = [&key](uint8_t* header) {
        Aes128XtsContext context{};
        aes128XtsContextCreate(&context, key, key + 0x10, true);
        for (u64 sector = 0; sector < kNcaHeaderSize / 0x200; ++sector) {
            aes128XtsContextResetSector(&context, sector, true);
            aes128XtsEncrypt(&context, header + sector * 0x200, header + sector * 0x200, 0x200);
        }
    };
    ForwarderPackage package;
    std::string why;
    if (!build_forwarder(contents, kStorageSdCard, encrypt, package, &why)) return fail(why);
    char id[20];
    std::snprintf(id, sizeof(id), "%016llX", static_cast<unsigned long long>(package.title_id));
    log_info("forwarder: " + contents.name + " is title " + id + ", starting " + forwarder_argv(contents));

    result = nsInitialize();
    if (R_FAILED(result)) return fail("the console's application manager would not open (" + describe(result) + ")");
    auto ns = scoped(true, [] { nsExit(); });
    Service manager{};
    const bool own_manager = hosversionAtLeast(3, 0, 0);
    if (own_manager) {
        result = nsGetApplicationManagerInterface(&manager);
        if (R_FAILED(result)) {
            return fail("the console's application manager would not open (" + describe(result) + ")");
        }
    } else {
        manager = *nsGetServiceSession_ApplicationManagerInterface();
    }
    auto close_manager = scoped(own_manager, [&manager] { serviceClose(&manager); });

    // The same forwarder made before: gone first, so this one replaces it
    // rather than joining it. Nothing to remove the first time.
    nsDeleteApplicationCompletely(package.title_id);

    result = ncmInitialize();
    if (R_FAILED(result)) return fail("the console's content manager would not open (" + describe(result) + ")");
    const bool written = write_ncas(package, error);
    ncmExit();
    if (!written) return fail(error != nullptr ? *error : "the forwarder could not be written");

    result = push_application_record(&manager, package.title_id, package.storage_record);
    if (R_FAILED(result)) {
        return fail("the HOME menu would not take the forwarder (" + describe(result) + ")");
    }
    invalidate_control_cache(&manager, package.title_id);
    log_info("forwarder: installed " + contents.name);
    return true;
}

}  // namespace ds::forwarder
