#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace ds {

// The games' own saves -- the cartridge save each game writes as <game>.srm in
// the saves folder -- and what Data Management knows about each: which game it
// is, what it looks like, and every version it has been, so that two devices
// comparing one game's save can tell which of them is newer without trusting
// either one's clock.
//
// Beside each save, <game>.srm.sync holds that. Replaced and deleted saves go
// to earlier/<game>/ in the same folder, the newest kEarlierSavesKept of them,
// so every change made here can be undone.
//
// DS only for now, but nothing below is about the DS except where the ID,
// title and icon come from: a save is tagged with its system, so 3DS and Wii U
// saves can join it later.

inline constexpr const char* kSaveSystemDs = "ds";
inline constexpr std::size_t kEarlierSavesKept = 10;
inline constexpr std::size_t kSaveHistoryKept = 64;

// One version of a save: made somewhere, at some time, with these contents.
// The ID is what two devices compare; the hash is what tells whether the file
// has changed since the last version was written down.
struct SaveVersion {
    std::string id;
    std::string hash;
    std::string made_on;  // a device's name
    int64_t made_at = 0;  // seconds since the epoch
};

// One game's save on this device.
struct SaveInfo {
    std::string stem;  // the save is <stem>.srm
    std::string system = kSaveSystemDs;
    std::string game_id;  // empty when the game is not known yet
    std::string title;
    std::string publisher;
    std::vector<uint8_t> icon;  // as the ROM stores it -- see DsRomInfo::icon
    std::string rom_path;
    std::vector<SaveVersion> history;  // oldest first; the last is the save as it is
    bool has_save = false;
    uint64_t size = 0;

    const SaveVersion* current() const {
        return has_save && !history.empty() ? &history.back() : nullptr;
    }
    // When the save last changed: its current version's time, which travels
    // with it from device to device, or the file's own when there is none.
    int64_t saved_at = 0;
    // What to show: the title, or the file's name for a game never seen.
    std::string display_title() const { return title.empty() ? stem : title; }
};

// A save that was replaced or deleted, kept to go back to.
struct EarlierSave {
    std::string id;
    std::string path;
    int64_t saved_at = 0;   // when it was made
    std::string made_on;    // where
    std::string reason;     // "Replaced by steamdeck's save", "Deleted", ...
};

// How the save on one device stands against the save on another.
enum class SaveComparison {
    UpToDate,    // the same save
    NewerHere,   // the other device's save is one this one grew out of
    NewerThere,  // the reverse
    Conflict,    // each has changed since they last matched, or they never have
    OnlyHere,
    OnlyThere,
    Neither,
};

// What one side knows of its save, which is all the comparison needs.
struct SaveVersionState {
    bool has_save = false;
    std::string current_id;
    std::string current_hash;
    std::vector<std::string> history_ids;
};

SaveVersionState version_state(const SaveInfo& info);
SaveComparison compare_saves(const SaveVersionState& here, const SaveVersionState& there);

class SaveStore {
public:
    SaveStore(std::string saves_dir, std::string device_name);

    const std::string& saves_dir() const { return saves_dir_; }
    std::string device_name() const;
    void set_device_name(std::string name);

    // Folders to look in for the ROM of a save this device has not seen
    // played -- one copied in by hand -- so it can still be matched.
    void set_rom_dirs(std::vector<std::string> directories);

    // The game at `rom_path` is starting here: its save is brought under this
    // ROM's name if it is kept under another name with the same game ID (a
    // renamed ROM, or a save that came from another device), its details are
    // written down, and its save's version noted.
    void note_game_started(const std::string& rom_path);
    // It has stopped, and its save may have changed.
    void note_game_stopped(const std::string& rom_path);
    // The game whose save is open right now, which nothing here will replace
    // or delete until it stops. Empty for none.
    void set_game_in_play(const std::string& game_id);
    std::string game_in_play() const;

    // Every game with a save, or with earlier saves to go back to. Newest
    // save first.
    std::vector<SaveInfo> list();
    bool find(const std::string& game_id, SaveInfo& out);
    // By the ROM's own name, for the game being played.
    bool find_for_rom(const std::string& rom_path, SaveInfo& out);

    // The save and everything about it, to send to another device. Its
    // current version is written down first, so a change since is included.
    bool read_for_sending(const std::string& game_id, SaveInfo& info, std::vector<uint8_t>& bytes,
                          std::string* error);
    // A save from `from_device` replaces this device's, whose own goes to
    // Earlier Saves. Takes on the incoming save's history, so the two devices
    // now agree. The resume point goes, since it holds the old save.
    bool accept(const SaveInfo& incoming, const std::vector<uint8_t>& bytes,
                const std::string& from_device, std::string* error);
    bool remove(const std::string& game_id, std::string* error);
    std::vector<EarlierSave> earlier(const std::string& game_id);
    bool restore(const std::string& game_id, const std::string& earlier_id, std::string* error);

    bool has_resume_point(const std::string& game_id);

    // For tests: the sidecar's path for a stem.
    std::string record_path(const std::string& stem) const;

private:
    std::string save_path(const std::string& stem) const;
    std::string earlier_dir(const std::string& stem) const;
    bool load_record(const std::string& stem, SaveInfo& out) const;
    bool store_record(const SaveInfo& info) const;
    void refresh_file_state(SaveInfo& info) const;
    void note_current_version(SaveInfo& info);
    bool find_locked(const std::string& game_id, SaveInfo& out);
    bool match_unknown(SaveInfo& info);
    bool set_aside(const SaveInfo& info, const std::string& reason, std::string* error);
    void prune_earlier(const std::string& stem) const;
    void delete_resume_point(const std::string& stem) const;
    std::string free_stem(const std::string& wanted, const std::string& game_code) const;

    mutable std::mutex mutex_;
    std::string saves_dir_;
    std::string device_name_;
    std::vector<std::string> rom_dirs_;
    std::string game_in_play_;
};

// A fresh, unguessable version ID.
std::string new_save_version_id();

}  // namespace ds
