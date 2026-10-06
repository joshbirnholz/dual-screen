#include "core/save_store.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <random>
#include <set>

#include <nlohmann/json.hpp>

#include "config/paths.hpp"
#include "config/sha1.hpp"
#include "core/console_title_info.hpp"
#include "core/ds_rom_info.hpp"
#include "core/save_folder.hpp"
#include "util/log.hpp"

namespace ds {
namespace {

using json = nlohmann::json;

constexpr const char* kSaveExtension = ".srm";
constexpr const char* kRecordExtension = ".srm.sync";

bool ends_with(const std::string& text, const std::string& suffix) {
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string to_hex(const std::vector<uint8_t>& bytes) {
    static const char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const uint8_t byte : bytes) {
        out.push_back(digits[byte >> 4]);
        out.push_back(digits[byte & 0x0F]);
    }
    return out;
}

std::vector<uint8_t> from_hex(const std::string& text) {
    const auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::vector<uint8_t> out;
    if (text.size() % 2 != 0) return out;
    out.reserve(text.size() / 2);
    for (std::size_t index = 0; index < text.size(); index += 2) {
        const int high = nibble(text[index]);
        const int low = nibble(text[index + 1]);
        if (high < 0 || low < 0) return {};
        out.push_back(static_cast<uint8_t>(high << 4 | low));
    }
    return out;
}

int64_t modified_at(const std::string& path) {
    struct stat info {};
    if (::stat(path.c_str(), &info) != 0) return 0;
    return static_cast<int64_t>(info.st_mtime);
}

uint64_t size_of(const std::string& path) {
    struct stat info {};
    if (::stat(path.c_str(), &info) != 0) return 0;
    return static_cast<uint64_t>(info.st_size);
}

int64_t now_seconds() { return static_cast<int64_t>(std::time(nullptr)); }

bool read_json(const std::string& path, json& out) {
    std::vector<uint8_t> bytes;
    if (!read_file(path, bytes, nullptr) || bytes.empty()) return false;
    try {
        out = json::parse(bytes.begin(), bytes.end());
        return out.is_object();
    } catch (const json::exception&) {
        log_warn("ignoring an unreadable " + path);
        return false;
    }
}

bool write_json(const std::string& path, const json& value, std::string* error) {
    const std::string text = value.dump(1);
    return write_file_atomically(path, text.data(), text.size(), error);
}

template <typename T>
T get_or(const json& object, const char* key, const T& fallback) {
    try {
        const auto found = object.find(key);
        if (found == object.end() || found->is_null()) return fallback;
        return found->get<T>();
    } catch (const json::exception&) {
        return fallback;
    }
}

// A ROM for `stem` in `directory` or one folder down: the same name with a DS
// extension, in whatever case the file system kept it.
std::string find_rom_named(const std::string& directory, const std::string& stem, int depth) {
    std::vector<DirectoryEntry> entries;
    if (!list_directory(directory, entries, nullptr)) return std::string();
    for (const DirectoryEntry& entry : entries) {
        if (!entry.is_directory && path_extension_lower(entry.name) == "nds" &&
            path_stem(entry.name) == stem) {
            return entry.path;
        }
    }
    if (depth <= 0) return std::string();
    for (const DirectoryEntry& entry : entries) {
        if (!entry.is_directory) continue;
        const std::string found = find_rom_named(entry.path, stem, depth - 1);
        if (!found.empty()) return found;
    }
    return std::string();
}

// A save's system from its name, for one without a record yet: a folder
// save's name is its game ID.
std::string system_for_stem(const std::string& stem) {
    if (stem.compare(0, 4, "CTR-") == 0 && title_id_from_game_id(stem) != 0) return kSaveSystem3ds;
    if (stem.compare(0, 4, "WUP-") == 0 && title_id_from_game_id(stem) != 0) return kSaveSystemWiiU;
    return kSaveSystemDs;
}

std::string hex8(uint32_t value) {
    char text[9];
    std::snprintf(text, sizeof(text), "%08x", value);
    return text;
}

constexpr const char* kThreeDsIds = "00000000000000000000000000000000";

bool is_3ds_rom(const std::string& path) {
    const std::string extension = path_extension_lower(path);
    return extension == "3ds" || extension == "cci" || extension == "cxi" || extension == "cia" ||
           extension == "app";
}

// A 3DS ROM with this title ID in `directory` or one folder down.
std::string find_3ds_rom(const std::string& directory, uint64_t title_id, int depth) {
    std::vector<DirectoryEntry> entries;
    if (!list_directory(directory, entries, nullptr)) return std::string();
    for (const DirectoryEntry& entry : entries) {
        if (entry.is_directory || !is_3ds_rom(entry.name)) continue;
        ConsoleTitleInfo info;
        if (read_3ds_title_info(entry.path, info, nullptr) && info.title_id == title_id) return entry.path;
    }
    if (depth <= 0) return std::string();
    for (const DirectoryEntry& entry : entries) {
        if (!entry.is_directory) continue;
        const std::string found = find_3ds_rom(entry.path, title_id, depth - 1);
        if (!found.empty()) return found;
    }
    return std::string();
}

// What a Wii U title is called, and its icon, from wherever this machine has
// the game: installed in Cemu's mlc01 (the game, else its update, which says
// the same), else as Cemu's game list remembers it -- a folder or a .wua
// somewhere else, read for its meta folder, or for a disc image only the name
// the list kept. `lists` are the parsed title_list_cache.xml files.
bool find_wii_u_title(const FolderSaveRoots& roots, const std::vector<CemuTitleListEntry>& lists, uint64_t title_id,
                      ConsoleTitleInfo& out) {
    const uint32_t low = static_cast<uint32_t>(title_id);
    const std::string low_dir = hex8(low);
    if (!roots.wii_u_mlc.empty()) {
        for (const std::string& high : {hex8(static_cast<uint32_t>(title_id >> 32)), std::string("0005000e")}) {
            const std::string meta = path_join(
                path_join(path_join(path_join(path_join(roots.wii_u_mlc, "usr"), "title"), high), low_dir), "meta");
            if (read_wii_u_meta(meta, out, nullptr) && !out.title.empty()) {
                out.title_id = title_id;
                return true;
            }
        }
    }
    // The game itself first, then its update, then its downloadable content.
    std::vector<const CemuTitleListEntry*> matches;
    for (const uint32_t high : {static_cast<uint32_t>(title_id >> 32), 0x0005000eu, 0x0005000cu}) {
        for (const CemuTitleListEntry& entry : lists) {
            if (entry.title_id == (uint64_t(high) << 32 | low)) matches.push_back(&entry);
        }
    }
    std::string name;
    for (const CemuTitleListEntry* entry : matches) {
        ConsoleTitleInfo found;
        bool read = false;
        if (entry->format == kCemuTitleFolder) {
            read = read_wii_u_meta(path_join(entry->path, "meta"), found, nullptr);
        } else if (entry->format == kCemuTitleArchive) {
            read = read_wii_u_archive_title_info(entry->path, entry->sub_path, entry->title_id, found, nullptr);
        }
        if (read && !found.title.empty()) {
            out = found;
            out.title_id = title_id;
            return true;
        }
        if (name.empty()) name = entry->name;
    }
    if (name.empty()) return false;
    out = ConsoleTitleInfo();
    out.title_id = title_id;
    out.title = name;
    return true;
}

}  // namespace

std::vector<std::string> find_cemu_title_lists(const std::string& cemu_program, const std::string& home,
                                               const std::string& mlc) {
    std::vector<std::string> folders;
    if (!cemu_program.empty() && cemu_program.find('/') != std::string::npos) {
        const std::string beside = path_dirname(cemu_program);
        folders.push_back(path_join(beside, "portable"));
        folders.push_back(beside);
    }
    if (!mlc.empty()) folders.push_back(path_dirname(mlc));
    if (!home.empty()) {
        folders.push_back(path_join(path_join(path_join(home, ".local"), "share"), "Cemu"));
        folders.push_back(path_join(path_join(path_join(home, ".var"), "app"), "info.cemu.Cemu/data/Cemu"));
        folders.push_back(path_join(home, "Library/Application Support/Cemu"));
    }
    std::vector<std::string> out;
    for (const std::string& folder : folders) {
        const std::string list = path_join(folder, "title_list_cache.xml");
        if (file_exists(list) && std::find(out.begin(), out.end(), list) == out.end()) out.push_back(list);
    }
    return out;
}

std::string find_cemu_mlc(const std::string& configured, const std::string& cemu_program,
                          const std::string& home) {
    if (!configured.empty()) return configured;
    // Each folder Cemu might keep settings.xml in, with the mlc01 it makes
    // there when the settings do not name another.
    std::vector<std::pair<std::string, std::string>> places;  // settings folder, default mlc01
    if (!cemu_program.empty() && cemu_program.find('/') != std::string::npos) {
        const std::string beside = path_dirname(cemu_program);
        places.push_back({path_join(beside, "portable"), path_join(path_join(beside, "portable"), "mlc01")});
        places.push_back({beside, path_join(beside, "mlc01")});
    }
    if (!home.empty()) {
        places.push_back({path_join(path_join(home, ".config"), "Cemu"),
                          path_join(path_join(path_join(home, ".local"), "share"), "Cemu/mlc01")});
        const std::string flatpak = path_join(path_join(path_join(home, ".var"), "app"), "info.cemu.Cemu");
        places.push_back({path_join(flatpak, "config/Cemu"), path_join(flatpak, "data/Cemu/mlc01")});
        const std::string mac = path_join(home, "Library/Application Support/Cemu");
        places.push_back({mac, path_join(mac, "mlc01")});
    }
    for (const auto& place : places) {
        std::vector<uint8_t> bytes;
        if (!read_file(path_join(place.first, "settings.xml"), bytes, nullptr)) continue;
        const std::string named = xml_text(std::string(bytes.begin(), bytes.end()), "mlc_path");
        if (!named.empty() && directory_exists(named)) return named;
        if (directory_exists(place.second)) return place.second;
    }
    for (const auto& place : places) {
        if (directory_exists(place.second)) return place.second;
    }
    return std::string();
}

std::string save_system_name(const std::string& system) {
    if (system == kSaveSystem3ds) return "3DS";
    if (system == kSaveSystemWiiU) return "Wii U";
    return "DS";
}

bool save_is_folder(const std::string& system) {
    return system == kSaveSystem3ds || system == kSaveSystemWiiU;
}

std::string new_save_version_id() {
    static std::mt19937_64 generator{std::random_device{}() ^
                                     static_cast<uint64_t>(std::time(nullptr))};
    static std::mutex mutex;
    std::lock_guard<std::mutex> lock(mutex);
    char text[33];
    std::snprintf(text, sizeof(text), "%016llx%016llx",
                  static_cast<unsigned long long>(generator()),
                  static_cast<unsigned long long>(generator()));
    return text;
}

SaveVersionState version_state(const SaveInfo& info) {
    SaveVersionState state;
    state.has_save = info.has_save;
    if (const SaveVersion* current = info.current()) {
        state.current_id = current->id;
        state.current_hash = current->hash;
    }
    for (const SaveVersion& version : info.history) state.history_ids.push_back(version.id);
    return state;
}

SaveComparison compare_saves(const SaveVersionState& here, const SaveVersionState& there) {
    if (!here.has_save && !there.has_save) return SaveComparison::Neither;
    if (!there.has_save) return SaveComparison::OnlyHere;
    if (!here.has_save) return SaveComparison::OnlyThere;
    if (!here.current_hash.empty() && here.current_hash == there.current_hash) {
        return SaveComparison::UpToDate;
    }
    const auto contains = [](const std::vector<std::string>& ids, const std::string& id) {
        return !id.empty() && std::find(ids.begin(), ids.end(), id) != ids.end();
    };
    // The other device's save is a version this one's has been, so this one
    // grew out of it -- and the reverse.
    if (contains(here.history_ids, there.current_id)) return SaveComparison::NewerHere;
    if (contains(there.history_ids, here.current_id)) return SaveComparison::NewerThere;
    return SaveComparison::Conflict;
}

SaveStore::SaveStore(std::string saves_dir, std::string device_name)
    : saves_dir_(std::move(saves_dir)), device_name_(std::move(device_name)) {
    roots_.three_ds_sdmc = {path_join(path_join(saves_dir_, "Azahar"), "sdmc"),
                            path_join(path_join(saves_dir_, "Citra"), "sdmc")};
}

void SaveStore::set_folder_roots(FolderSaveRoots roots) {
    std::lock_guard<std::mutex> lock(mutex_);
    roots_ = std::move(roots);
}

FolderSaveRoots SaveStore::folder_roots() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return roots_;
}

std::string SaveStore::save_folder(const std::string& system, uint64_t title_id) const {
    const std::string high = hex8(static_cast<uint32_t>(title_id >> 32));
    const std::string low = hex8(static_cast<uint32_t>(title_id));
    if (system == kSaveSystemWiiU) {
        if (roots_.wii_u_mlc.empty()) return std::string();
        return path_join(path_join(path_join(path_join(roots_.wii_u_mlc, "usr"), "save"), high), low);
    }
    if (system != kSaveSystem3ds || roots_.three_ds_sdmc.empty()) return std::string();
    const auto under = [&](const std::string& sdmc) {
        std::string path = path_join(path_join(sdmc, "Nintendo 3DS"), kThreeDsIds);
        path = path_join(path_join(path_join(path, kThreeDsIds), "title"), high);
        return path_join(path_join(path, low), "data");
    };
    // Wherever the game already has one; else in the emulator that has been
    // run here; else the first.
    for (const std::string& sdmc : roots_.three_ds_sdmc) {
        if (directory_exists(under(sdmc))) return under(sdmc);
    }
    for (const std::string& sdmc : roots_.three_ds_sdmc) {
        if (directory_exists(sdmc)) return under(sdmc);
    }
    return under(roots_.three_ds_sdmc.front());
}

std::string SaveStore::folder_for(const SaveInfo& info) const {
    return save_folder(info.system, title_id_from_game_id(info.game_id.empty() ? info.stem : info.game_id));
}

bool SaveStore::save_present(const SaveInfo& info) const {
    if (!save_is_folder(info.system)) return file_exists(save_path(info.stem));
    const std::string folder = folder_for(info);
    return !folder.empty() && save_folder_has_files(folder);
}

bool SaveStore::read_save(const SaveInfo& info, std::vector<uint8_t>& bytes, std::string* error) const {
    if (!save_is_folder(info.system)) return read_file(save_path(info.stem), bytes, error);
    const std::string folder = folder_for(info);
    if (folder.empty()) {
        if (error != nullptr) *error = "there is nowhere here for that game's save";
        return false;
    }
    return pack_save_folder(folder, bytes, error);
}

bool SaveStore::write_save(const SaveInfo& info, const std::vector<uint8_t>& bytes, std::string* error) const {
    if (!save_is_folder(info.system)) {
        return write_file_atomically(save_path(info.stem), bytes.data(), bytes.size(), error);
    }
    const std::string folder = folder_for(info);
    if (folder.empty()) {
        if (error != nullptr) {
            *error = info.system == kSaveSystemWiiU ? "Cemu's mlc01 folder was not found here"
                                                    : "there is nowhere here for that game's save";
        }
        return false;
    }
    return unpack_save_folder(bytes, folder, error);
}

bool SaveStore::delete_save(const SaveInfo& info, std::string* error) const {
    const std::string path = save_is_folder(info.system) ? folder_for(info) : save_path(info.stem);
    const bool gone = save_is_folder(info.system) ? remove_save_folder(path) : std::remove(path.c_str()) == 0;
    if (!gone && error != nullptr) *error = "could not delete " + path;
    return gone;
}

int64_t SaveStore::save_modified_at(const SaveInfo& info) const {
    if (!save_is_folder(info.system)) return modified_at(save_path(info.stem));
    return save_folder_modified_at(folder_for(info));
}

std::string SaveStore::device_name() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return device_name_;
}

void SaveStore::set_device_name(std::string name) {
    std::lock_guard<std::mutex> lock(mutex_);
    device_name_ = std::move(name);
}

void SaveStore::set_rom_dirs(std::vector<std::string> directories) {
    std::lock_guard<std::mutex> lock(mutex_);
    rom_dirs_ = std::move(directories);
}

void SaveStore::set_game_in_play(const std::string& game_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    game_in_play_ = game_id;
}

std::string SaveStore::game_in_play() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return game_in_play_;
}

std::string SaveStore::save_path(const std::string& stem) const {
    return path_join(saves_dir_, stem + kSaveExtension);
}

std::string SaveStore::record_path(const std::string& stem) const {
    return path_join(saves_dir_, stem + kRecordExtension);
}

std::string SaveStore::earlier_dir(const std::string& stem) const {
    return path_join(path_join(saves_dir_, "earlier"), stem);
}

bool SaveStore::load_record(const std::string& stem, SaveInfo& out) const {
    out = SaveInfo();
    out.stem = stem;
    out.system = system_for_stem(stem);
    if (save_is_folder(out.system)) out.game_id = stem;
    json record;
    const bool known = read_json(record_path(stem), record);
    if (known) {
        out.system = get_or<std::string>(record, "system", system_for_stem(stem));
        out.game_id = get_or<std::string>(record, "game_id", out.game_id);
        out.title = get_or<std::string>(record, "title", "");
        out.publisher = get_or<std::string>(record, "publisher", "");
        out.icon = from_hex(get_or<std::string>(record, "icon", ""));
        out.rom_path = get_or<std::string>(record, "rom", "");
        const auto history = record.find("history");
        if (history != record.end() && history->is_array()) {
            for (const json& entry : *history) {
                if (!entry.is_object()) continue;
                SaveVersion version;
                version.id = get_or<std::string>(entry, "id", "");
                version.hash = get_or<std::string>(entry, "hash", "");
                version.made_on = get_or<std::string>(entry, "made_on", "");
                version.made_at = get_or<int64_t>(entry, "made_at", 0);
                if (!version.id.empty()) out.history.push_back(version);
            }
        }
    }
    refresh_file_state(out);
    return known || out.has_save;
}

bool SaveStore::store_record(const SaveInfo& info) const {
    json history = json::array();
    for (const SaveVersion& version : info.history) {
        history.push_back({{"id", version.id},
                           {"hash", version.hash},
                           {"made_on", version.made_on},
                           {"made_at", version.made_at}});
    }
    const json record = {{"system", info.system},     {"game_id", info.game_id},
                         {"title", info.title},       {"publisher", info.publisher},
                         {"icon", to_hex(info.icon)}, {"rom", info.rom_path},
                         {"history", history}};
    std::string error;
    if (!write_json(record_path(info.stem), record, &error)) {
        log_warn("could not write " + record_path(info.stem) + ": " + error);
        return false;
    }
    return true;
}

void SaveStore::refresh_file_state(SaveInfo& info) const {
    info.has_save = save_present(info);
    if (!info.has_save) {
        info.size = 0;
    } else {
        info.size = save_is_folder(info.system) ? save_folder_size(folder_for(info)) : size_of(save_path(info.stem));
    }
    if (const SaveVersion* current = info.current(); current != nullptr && current->made_at > 0) {
        info.saved_at = current->made_at;
    } else {
        info.saved_at = info.has_save ? save_modified_at(info) : 0;
    }
}

void SaveStore::note_current_version(SaveInfo& info) {
    refresh_file_state(info);
    if (!info.has_save) return;
    std::vector<uint8_t> bytes;
    if (!read_save(info, bytes, nullptr)) return;
    const std::string hash = Sha1::hash_bytes(bytes.data(), bytes.size());
    if (!info.history.empty() && info.history.back().hash == hash) return;
    SaveVersion version;
    version.id = new_save_version_id();
    version.hash = hash;
    version.made_on = device_name_;
    version.made_at = save_modified_at(info);
    if (version.made_at == 0) version.made_at = now_seconds();
    info.history.push_back(version);
    if (info.history.size() > kSaveHistoryKept) {
        info.history.erase(info.history.begin(),
                           info.history.end() - static_cast<std::ptrdiff_t>(kSaveHistoryKept));
    }
    refresh_file_state(info);
    store_record(info);
}

bool SaveStore::match_unknown(SaveInfo& info) {
    if (save_is_folder(info.system)) {
        // Known by its title ID already; a 3DS ROM here with that ID names it.
        if (!info.title.empty() || info.system != kSaveSystem3ds) return true;
        const uint64_t title_id = title_id_from_game_id(info.game_id);
        for (const std::string& directory : rom_dirs_) {
            const std::string rom = find_3ds_rom(directory, title_id, 1);
            ConsoleTitleInfo title;
            if (rom.empty() || !read_3ds_title_info(rom, title, nullptr)) continue;
            info.title = title.title;
            info.publisher = title.publisher;
            info.icon = title.icon;
            info.rom_path = rom;
            store_record(info);
            log_info("matched the save " + info.stem + " to " + rom);
            return true;
        }
        return false;
    }
    if (!info.game_id.empty()) return true;
    for (const std::string& directory : rom_dirs_) {
        const std::string rom = find_rom_named(directory, info.stem, 1);
        if (rom.empty()) continue;
        DsRomInfo rom_info;
        if (!read_ds_rom_info(rom, rom_info, nullptr)) continue;
        info.game_id = rom_info.game_id;
        info.title = rom_info.title;
        info.publisher = rom_info.publisher;
        info.icon = rom_info.icon;
        info.rom_path = rom;
        note_current_version(info);
        store_record(info);
        log_info("matched the save " + info.stem + " to " + rom);
        return true;
    }
    return false;
}

void SaveStore::discover_folder_saves() {
    // Every title with a save in the emulators' folders that has no record
    // here yet -- played before this app knew of it, or in the emulator on
    // its own -- written down, so it is listed and has a version to compare.
    // Cemu's game lists, read when a Wii U game first needs one.
    bool lists_read = false;
    std::vector<CemuTitleListEntry> lists;
    const auto wii_u_title = [&](uint64_t title_id, ConsoleTitleInfo& title) {
        if (!lists_read) {
            lists_read = true;
            for (const std::string& path : roots_.wii_u_title_lists) {
                std::vector<CemuTitleListEntry> entries = read_cemu_title_list(path);
                lists.insert(lists.end(), entries.begin(), entries.end());
            }
        }
        return find_wii_u_title(roots_, lists, title_id, title);
    };
    const auto note = [&](const std::string& system, uint64_t title_id) {
        const std::string game_id =
            system == kSaveSystem3ds ? three_ds_game_id(title_id) : wii_u_game_id(title_id);
        if (file_exists(record_path(game_id))) {
            // Listed before, perhaps without a name or an icon: when the game
            // was not in Cemu yet, or this app did not yet know where else to
            // look. Looked up again once each run until it has both.
            if (system != kSaveSystemWiiU ||
                std::find(wii_u_looked_up_.begin(), wii_u_looked_up_.end(), game_id) != wii_u_looked_up_.end()) {
                return;
            }
            wii_u_looked_up_.push_back(game_id);
            SaveInfo known;
            if (!load_record(game_id, known) || (!known.title.empty() && !known.icon.empty())) return;
            ConsoleTitleInfo title;
            if (!wii_u_title(title_id, title)) return;
            const bool better = (known.title.empty() && !title.title.empty()) ||
                                (known.icon.empty() && !title.icon.empty());
            if (!better) return;
            if (!title.title.empty()) known.title = title.title;
            if (!title.publisher.empty()) known.publisher = title.publisher;
            if (!title.icon.empty()) known.icon = title.icon;
            store_record(known);
            log_info("found what " + game_id + " is: " + known.title +
                     (known.icon.empty() ? " (no icon)" : ""));
            return;
        }
        SaveInfo info;
        info.stem = game_id;
        info.system = system;
        info.game_id = game_id;
        if (!save_present(info)) return;
        if (system == kSaveSystemWiiU) {
            ConsoleTitleInfo title;
            if (wii_u_title(title_id, title)) {
                info.title = title.title;
                info.publisher = title.publisher;
                info.icon = title.icon;
            }
            wii_u_looked_up_.push_back(game_id);
        }
        store_record(info);
        note_current_version(info);
        log_info("found a " + std::string(system == kSaveSystem3ds ? "3DS" : "Wii U") + " save for " + game_id);
    };
    const auto each_title = [](const std::string& high_dir, uint32_t high, const auto& visit) {
        std::vector<DirectoryEntry> lows;
        if (!list_directory(high_dir, lows, nullptr)) return;
        for (const DirectoryEntry& low : lows) {
            if (!low.is_directory || low.name.size() != 8) continue;
            char* end = nullptr;
            const uint32_t value = static_cast<uint32_t>(std::strtoul(low.name.c_str(), &end, 16));
            if (end == nullptr || *end != '\0') continue;
            visit(uint64_t(high) << 32 | value);
        }
    };
    for (const std::string& sdmc : roots_.three_ds_sdmc) {
        const std::string titles = path_join(
            path_join(path_join(path_join(sdmc, "Nintendo 3DS"), kThreeDsIds), kThreeDsIds), "title");
        each_title(path_join(titles, "00040000"), 0x00040000u,
                   [&](uint64_t title_id) { note(kSaveSystem3ds, title_id); });
    }
    if (!roots_.wii_u_mlc.empty()) {
        each_title(path_join(path_join(path_join(roots_.wii_u_mlc, "usr"), "save"), "00050000"), 0x00050000u,
                   [&](uint64_t title_id) { note(kSaveSystemWiiU, title_id); });
    }
}

std::vector<SaveInfo> SaveStore::list() {
    std::lock_guard<std::mutex> lock(mutex_);
    discover_folder_saves();
    std::set<std::string> stems;
    std::vector<DirectoryEntry> entries;
    if (list_directory(saves_dir_, entries, nullptr)) {
        for (const DirectoryEntry& entry : entries) {
            if (entry.is_directory) continue;
            if (ends_with(entry.name, kRecordExtension)) {
                stems.insert(entry.name.substr(0, entry.name.size() - std::string(kRecordExtension).size()));
            } else if (ends_with(entry.name, kSaveExtension)) {
                stems.insert(entry.name.substr(0, entry.name.size() - std::string(kSaveExtension).size()));
            }
        }
    }
    std::vector<SaveInfo> out;
    for (const std::string& stem : stems) {
        SaveInfo info;
        if (!load_record(stem, info)) continue;
        if (info.game_id.empty() || (save_is_folder(info.system) && info.title.empty())) match_unknown(info);
        if (save_is_folder(info.system) && info.has_save) {
            // Played in the emulator on its own as often as from here: a
            // folder changed since it was last looked at is a new version.
            const int64_t changed = save_modified_at(info);
            int64_t& seen = folder_seen_at_[stem];
            const SaveVersion* current = info.current();
            if (current == nullptr || (changed > current->made_at && changed > seen)) note_current_version(info);
            seen = changed;
        }
        if (!info.has_save) {
            // Kept for its earlier saves, which are still there to go back to.
            std::vector<DirectoryEntry> earlier;
            if (!list_directory(earlier_dir(stem), earlier, nullptr) || earlier.empty()) continue;
        }
        out.push_back(info);
    }
    std::sort(out.begin(), out.end(), [](const SaveInfo& a, const SaveInfo& b) {
        if (a.has_save != b.has_save) return a.has_save;
        return a.saved_at > b.saved_at;
    });
    return out;
}

bool SaveStore::find_locked(const std::string& game_id, SaveInfo& out) {
    if (game_id.empty()) return false;
    std::vector<DirectoryEntry> entries;
    if (!list_directory(saves_dir_, entries, nullptr)) return false;
    bool found = false;
    for (const DirectoryEntry& entry : entries) {
        if (entry.is_directory || !ends_with(entry.name, kRecordExtension)) continue;
        const std::string stem =
            entry.name.substr(0, entry.name.size() - std::string(kRecordExtension).size());
        SaveInfo info;
        if (!load_record(stem, info) || info.game_id != game_id) continue;
        // One name can be left over from a game's earlier one, holding only
        // its earlier saves: the one with a save is the one that counts.
        if (!found || (info.has_save && !out.has_save)) {
            out = info;
            found = true;
        }
    }
    return found;
}

bool SaveStore::find(const std::string& game_id, SaveInfo& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    return find_locked(game_id, out);
}

bool SaveStore::identify(const std::string& rom_path, SaveInfo& out) const {
    out = SaveInfo();
    ConsoleTitleInfo title;
    if (is_3ds_rom(rom_path) && read_3ds_title_info(rom_path, title, nullptr)) {
        out.system = kSaveSystem3ds;
        out.game_id = three_ds_game_id(title.title_id);
    } else if (path_extension_lower(rom_path) == "rpx" && read_wii_u_title_info(rom_path, title, nullptr)) {
        out.system = kSaveSystemWiiU;
        out.game_id = wii_u_game_id(title.title_id);
    } else {
        return false;
    }
    out.stem = out.game_id;
    out.title = title.title;
    out.publisher = title.publisher;
    out.icon = title.icon;
    out.rom_path = rom_path;
    return true;
}

bool SaveStore::find_for_rom(const std::string& rom_path, SaveInfo& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    SaveInfo identified;
    if (identify(rom_path, identified)) {
        load_record(identified.stem, out);
        if (out.title.empty()) out.title = identified.title;
        return true;
    }
    return load_record(path_stem(rom_path), out) && !out.game_id.empty();
}

void SaveStore::note_game_started(const std::string& rom_path) {
    SaveInfo identified;
    if (identify(rom_path, identified)) {
        // A 3DS or Wii U game: its save is wherever the emulator keeps it,
        // under its title ID, so there is nothing to bring under another name.
        std::lock_guard<std::mutex> lock(mutex_);
        SaveInfo info;
        load_record(identified.stem, info);
        info.stem = identified.stem;
        info.system = identified.system;
        info.game_id = identified.game_id;
        if (!identified.title.empty()) info.title = identified.title;
        if (!identified.publisher.empty()) info.publisher = identified.publisher;
        if (!identified.icon.empty()) info.icon = identified.icon;
        info.rom_path = rom_path;
        store_record(info);
        note_current_version(info);
        game_in_play_ = info.game_id;
        started_stem_ = info.stem;
        return;
    }
    if (path_extension_lower(rom_path) != "nds") return;
    DsRomInfo rom;
    if (!read_ds_rom_info(rom_path, rom, nullptr)) return;
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string stem = path_stem(rom_path);
    // Kept under another name for the same game -- a ROM renamed since, or a
    // save that arrived from a device whose ROM is called something else:
    // brought under this one's, which is the name the emulator will look for.
    if (!file_exists(save_path(stem))) {
        SaveInfo elsewhere;
        if (find_locked(rom.game_id, elsewhere) && elsewhere.stem != stem && elsewhere.has_save) {
            std::rename(save_path(elsewhere.stem).c_str(), save_path(stem).c_str());
            std::rename(record_path(elsewhere.stem).c_str(), record_path(stem).c_str());
            if (directory_exists(earlier_dir(elsewhere.stem)) && !directory_exists(earlier_dir(stem))) {
                std::rename(earlier_dir(elsewhere.stem).c_str(), earlier_dir(stem).c_str());
            }
            log_info("the save for " + rom.game_id + " was kept as " + elsewhere.stem +
                     "; it is now " + stem + ", after this ROM");
        }
    }
    SaveInfo info;
    load_record(stem, info);
    info.stem = stem;
    info.system = kSaveSystemDs;
    info.game_id = rom.game_id;
    if (!rom.title.empty()) info.title = rom.title;
    if (!rom.publisher.empty()) info.publisher = rom.publisher;
    if (!rom.icon.empty()) info.icon = rom.icon;
    info.rom_path = rom_path;
    store_record(info);
    note_current_version(info);
    game_in_play_ = rom.game_id;
    started_stem_ = stem;
}

void SaveStore::note_game_stopped(const std::string& rom_path) {
    std::lock_guard<std::mutex> lock(mutex_);
    SaveInfo info;
    const std::string stem = started_stem_.empty() ? path_stem(rom_path) : started_stem_;
    if (load_record(stem, info) && !info.game_id.empty()) {
        note_current_version(info);
    }
    game_in_play_.clear();
    started_stem_.clear();
}

bool SaveStore::read_for_sending(const std::string& game_id, SaveInfo& info,
                                 std::vector<uint8_t>& bytes, std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!find_locked(game_id, info) || !info.has_save) {
        if (error != nullptr) *error = "there is no save for this game here";
        return false;
    }
    note_current_version(info);
    return read_save(info, bytes, error);
}

std::string SaveStore::free_stem(const std::string& wanted, const std::string& game_code) const {
    const auto taken = [this](const std::string& stem) {
        return file_exists(save_path(stem)) || file_exists(record_path(stem));
    };
    std::string stem = wanted.empty() ? game_code : wanted;
    if (!taken(stem)) return stem;
    for (int attempt = 2; attempt < 100; ++attempt) {
        const std::string candidate = stem + " (" + std::to_string(attempt) + ")";
        if (!taken(candidate)) return candidate;
    }
    return stem + " (" + new_save_version_id().substr(0, 6) + ")";
}

bool SaveStore::set_aside(const SaveInfo& info, const std::string& reason, std::string* error) {
    if (!info.has_save) return true;
    const std::string directory = earlier_dir(info.stem);
    ensure_directory(path_join(saves_dir_, "earlier"));
    if (!ensure_directory(directory)) {
        if (error != nullptr) *error = "could not make " + directory;
        return false;
    }
    std::vector<uint8_t> bytes;
    if (!read_save(info, bytes, error)) return false;
    // Named for when it was set aside, to the millisecond, so that the names
    // sort in that order however many land in one second.
    char stamp[20];
    std::snprintf(stamp, sizeof(stamp), "%013lld",
                  static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                             std::chrono::system_clock::now().time_since_epoch())
                                             .count()));
    const std::string id = std::string(stamp) + "-" + new_save_version_id().substr(0, 6);
    if (!write_file_atomically(path_join(directory, id + kSaveExtension), bytes.data(), bytes.size(),
                               error)) {
        return false;
    }
    const SaveVersion* current = info.current();
    const json about = {{"saved_at", info.saved_at},
                        {"made_on", current != nullptr ? current->made_on : device_name_},
                        {"reason", reason},
                        {"set_aside_at", now_seconds()}};
    if (!write_json(path_join(directory, id + ".json"), about, error)) return false;
    prune_earlier(info.stem);
    return true;
}

std::vector<EarlierSave> SaveStore::earlier(const std::string& game_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<EarlierSave> out;
    SaveInfo info;
    if (!find_locked(game_id, info)) return out;
    std::vector<std::pair<int64_t, EarlierSave>> found;
    std::vector<DirectoryEntry> entries;
    if (!list_directory(earlier_dir(info.stem), entries, nullptr)) return out;
    for (const DirectoryEntry& entry : entries) {
        if (entry.is_directory || !ends_with(entry.name, ".json")) continue;
        json about;
        if (!read_json(entry.path, about)) continue;
        EarlierSave save;
        save.id = entry.name.substr(0, entry.name.size() - 5);
        save.path = path_join(earlier_dir(info.stem), save.id + kSaveExtension);
        if (!file_exists(save.path)) continue;
        save.saved_at = get_or<int64_t>(about, "saved_at", 0);
        save.made_on = get_or<std::string>(about, "made_on", "");
        save.reason = get_or<std::string>(about, "reason", "");
        found.emplace_back(get_or<int64_t>(about, "set_aside_at", 0), save);
    }
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) {
        if (a.first != b.first) return a.first > b.first;
        return a.second.id > b.second.id;
    });
    for (auto& [when, save] : found) out.push_back(std::move(save));
    return out;
}

void SaveStore::prune_earlier(const std::string& stem) const {
    std::vector<DirectoryEntry> entries;
    if (!list_directory(earlier_dir(stem), entries, nullptr)) return;
    std::vector<std::pair<int64_t, std::string>> kept;
    for (const DirectoryEntry& entry : entries) {
        if (entry.is_directory || !ends_with(entry.name, ".json")) continue;
        json about;
        const int64_t when = read_json(entry.path, about) ? get_or<int64_t>(about, "set_aside_at", 0) : 0;
        kept.emplace_back(when, entry.name.substr(0, entry.name.size() - 5));
    }
    if (kept.size() <= kEarlierSavesKept) return;
    std::sort(kept.begin(), kept.end(), [](const auto& a, const auto& b) {
        if (a.first != b.first) return a.first > b.first;
        return a.second > b.second;
    });
    for (std::size_t index = kEarlierSavesKept; index < kept.size(); ++index) {
        std::remove(path_join(earlier_dir(stem), kept[index].second + ".json").c_str());
        std::remove(path_join(earlier_dir(stem), kept[index].second + kSaveExtension).c_str());
    }
}

void SaveStore::delete_resume_point(const std::string& stem) const {
    const std::string path = path_join(saves_dir_, stem + ".resume");
    if (file_exists(path) && std::remove(path.c_str()) == 0) {
        log_info("deleted the resume point for " + stem + ": it holds the save that was replaced");
    }
}

bool SaveStore::has_resume_point(const std::string& game_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    SaveInfo info;
    return find_locked(game_id, info) && file_exists(path_join(saves_dir_, info.stem + ".resume"));
}

bool SaveStore::accept(const SaveInfo& incoming, const std::vector<uint8_t>& bytes,
                       const std::string& from_device, std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (incoming.game_id.empty()) {
        if (error != nullptr) *error = "the save does not say which game it is for";
        return false;
    }
    if (incoming.game_id == game_in_play_) {
        if (error != nullptr) *error = "that game is being played here";
        return false;
    }
    SaveInfo local;
    const bool known = find_locked(incoming.game_id, local);
    if (!known && save_is_folder(incoming.system)) {
        // Named by its game ID, and perhaps in the emulator's folder already
        // without this app having written it down.
        load_record(incoming.game_id, local);
        local.stem = incoming.game_id;
        local.system = incoming.system;
        local.game_id = incoming.game_id;
        refresh_file_state(local);
    } else if (!known) {
        const std::string code = incoming.game_id.substr(0, incoming.game_id.find('-'));
        local = SaveInfo();
        local.stem = free_stem(incoming.stem, code);
    }
    if (!set_aside(local, "Replaced by " + from_device + "'s save", error)) return false;
    SaveInfo target = local;
    target.system = incoming.system;
    target.game_id = incoming.game_id;
    if (!write_save(target, bytes, error)) return false;
    SaveInfo updated = incoming;
    updated.stem = local.stem;
    updated.rom_path = local.rom_path;
    if (updated.title.empty()) updated.title = local.title;
    if (updated.publisher.empty()) updated.publisher = local.publisher;
    if (updated.icon.empty()) updated.icon = local.icon;
    refresh_file_state(updated);
    store_record(updated);
    delete_resume_point(local.stem);
    log_info("the save for " + updated.display_title() + " is now " + from_device + "'s");
    return true;
}

bool SaveStore::remove(const std::string& game_id, std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (game_id == game_in_play_) {
        if (error != nullptr) *error = "that game is being played here";
        return false;
    }
    SaveInfo info;
    if (!find_locked(game_id, info) || !info.has_save) {
        if (error != nullptr) *error = "there is no save for this game here";
        return false;
    }
    if (!set_aside(info, "Deleted", error)) return false;
    if (!delete_save(info, error)) return false;
    delete_resume_point(info.stem);
    log_info("deleted the save for " + info.display_title() + "; it is in Earlier Saves");
    return true;
}

bool SaveStore::restore(const std::string& game_id, const std::string& earlier_id,
                        std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (game_id == game_in_play_) {
        if (error != nullptr) *error = "that game is being played here";
        return false;
    }
    SaveInfo info;
    if (!find_locked(game_id, info)) {
        if (error != nullptr) *error = "there is nothing known about this game here";
        return false;
    }
    const std::string directory = earlier_dir(info.stem);
    const std::string earlier_save = path_join(directory, earlier_id + kSaveExtension);
    const std::string earlier_about = path_join(directory, earlier_id + ".json");
    json about;
    std::vector<uint8_t> bytes;
    if (!read_json(earlier_about, about) || !read_file(earlier_save, bytes, error)) {
        if (error != nullptr && error->empty()) *error = "that earlier save is gone";
        return false;
    }
    if (!set_aside(info, "Replaced by a restored save", error)) return false;
    if (!write_save(info, bytes, error)) return false;
    std::remove(earlier_save.c_str());
    std::remove(earlier_about.c_str());
    // A version of its own, although its contents were seen before: put back,
    // it is newer than whatever it replaced, here or anywhere else.
    SaveVersion version;
    version.id = new_save_version_id();
    version.hash = Sha1::hash_bytes(bytes.data(), bytes.size());
    version.made_on = get_or<std::string>(about, "made_on", device_name_);
    version.made_at = get_or<int64_t>(about, "saved_at", now_seconds());
    info.history.push_back(version);
    if (info.history.size() > kSaveHistoryKept) {
        info.history.erase(info.history.begin(),
                           info.history.end() - static_cast<std::ptrdiff_t>(kSaveHistoryKept));
    }
    refresh_file_state(info);
    store_record(info);
    delete_resume_point(info.stem);
    log_info("restored an earlier save for " + info.display_title());
    return true;
}

}  // namespace ds
