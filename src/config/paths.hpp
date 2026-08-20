#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ds {

// All on-disk locations the app uses, resolved once at startup so nothing else
// has to know about XDG rules.
struct AppPaths {
    std::string config_dir;    // ~/.config/dualscreen
    std::string games_dir;     // ~/.config/dualscreen/games
    std::string settings_file; // ~/.config/dualscreen/settings.json
    std::string saves_dir;     // ~/.local/share/dualscreen/saves
    std::string default_system_dir;  // ~/.local/share/dualscreen/system
    std::string log_file;            // ~/.local/share/dualscreen/dualscreen.log

    static AppPaths resolve();
};

// Directories the app will offer as candidates for the BIOS/firmware "system"
// directory in the settings screen (only those that exist are returned, plus
// the app's own default, which is always first).
std::vector<std::string> candidate_system_dirs(const AppPaths& paths);

// One entry from a directory listing.
struct DirectoryEntry {
    std::string name;
    std::string path;
    bool is_directory = false;
};

// Lists `directory`, sorted with sub-directories first and then files, each
// group alphabetical and case-insensitive. Hidden entries are skipped. Returns
// false with a readable reason if the directory cannot be read.
bool list_directory(const std::string& directory,
                    std::vector<DirectoryEntry>& out_entries,
                    std::string* error);

bool ensure_directory(const std::string& path);

// Reads a whole file into `out`. Returns false with a reason if it cannot be
// read; a missing file is a normal, quiet failure.
bool read_file(const std::string& path, std::vector<uint8_t>& out, std::string* error);

// Writes through a temporary file, flushed to disk, then renamed over the
// target. A save file is the one thing in here that must never be left
// half-written, and a handheld gets powered off mid-write for a living.
bool write_file_atomically(const std::string& path,
                           const void* data,
                           std::size_t size,
                           std::string* error);
bool file_exists(const std::string& path);
bool directory_exists(const std::string& path);
std::string path_join(const std::string& a, const std::string& b);
std::string path_basename(const std::string& path);
std::string path_stem(const std::string& path);
// The extension without the dot, lowercased. Empty when there is none.
std::string path_extension_lower(const std::string& path);
std::string path_dirname(const std::string& path);
// Expands a leading "~/". Anything else is returned unchanged -- in particular
// a relative path stays relative. Use absolute_path() when a path has to be
// one that can be walked upwards.
std::string expand_user_path(const std::string& path);
// Expands "~/", resolves the path against the working directory if it is
// relative, and folds away "." and ".." segments. The result always begins
// with "/", so it has a real parent to climb to; an empty path stays empty.
// Purely lexical: nothing has to exist for this to work.
std::string absolute_path(const std::string& path);

}  // namespace ds
