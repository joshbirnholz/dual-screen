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
#include "core/ds_rom_info.hpp"
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

}  // namespace

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
    : saves_dir_(std::move(saves_dir)), device_name_(std::move(device_name)) {}

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
    json record;
    const bool known = read_json(record_path(stem), record);
    if (known) {
        out.system = get_or<std::string>(record, "system", kSaveSystemDs);
        out.game_id = get_or<std::string>(record, "game_id", "");
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
    const std::string path = save_path(info.stem);
    info.has_save = file_exists(path);
    info.size = info.has_save ? size_of(path) : 0;
    if (const SaveVersion* current = info.current(); current != nullptr && current->made_at > 0) {
        info.saved_at = current->made_at;
    } else {
        info.saved_at = info.has_save ? modified_at(path) : 0;
    }
}

void SaveStore::note_current_version(SaveInfo& info) {
    refresh_file_state(info);
    if (!info.has_save) return;
    std::vector<uint8_t> bytes;
    if (!read_file(save_path(info.stem), bytes, nullptr)) return;
    const std::string hash = Sha1::hash_bytes(bytes.data(), bytes.size());
    if (!info.history.empty() && info.history.back().hash == hash) return;
    SaveVersion version;
    version.id = new_save_version_id();
    version.hash = hash;
    version.made_on = device_name_;
    version.made_at = modified_at(save_path(info.stem));
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

std::vector<SaveInfo> SaveStore::list() {
    std::lock_guard<std::mutex> lock(mutex_);
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
        if (info.game_id.empty()) match_unknown(info);
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

bool SaveStore::find_for_rom(const std::string& rom_path, SaveInfo& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    return load_record(path_stem(rom_path), out) && !out.game_id.empty();
}

void SaveStore::note_game_started(const std::string& rom_path) {
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
}

void SaveStore::note_game_stopped(const std::string& rom_path) {
    std::lock_guard<std::mutex> lock(mutex_);
    SaveInfo info;
    if (load_record(path_stem(rom_path), info) && !info.game_id.empty()) {
        note_current_version(info);
    }
    game_in_play_.clear();
}

bool SaveStore::read_for_sending(const std::string& game_id, SaveInfo& info,
                                 std::vector<uint8_t>& bytes, std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!find_locked(game_id, info) || !info.has_save) {
        if (error != nullptr) *error = "there is no save for this game here";
        return false;
    }
    note_current_version(info);
    if (!read_file(save_path(info.stem), bytes, error)) return false;
    return true;
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
    if (!read_file(save_path(info.stem), bytes, error)) return false;
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
    if (!known) {
        const std::string code = incoming.game_id.substr(0, incoming.game_id.find('-'));
        local = SaveInfo();
        local.stem = free_stem(incoming.stem, code);
    }
    if (!set_aside(local, "Replaced by " + from_device + "'s save", error)) return false;
    if (!write_file_atomically(save_path(local.stem), bytes.data(), bytes.size(), error)) return false;
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
    if (std::remove(save_path(info.stem).c_str()) != 0) {
        if (error != nullptr) *error = "could not delete " + save_path(info.stem);
        return false;
    }
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
    if (!write_file_atomically(save_path(info.stem), bytes.data(), bytes.size(), error)) return false;
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
