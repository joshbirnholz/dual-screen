#pragma once

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

    static AppPaths resolve();
};

// Directories the app will offer as candidates for the BIOS/firmware "system"
// directory in the settings screen (only those that exist are returned, plus
// the app's own default, which is always first).
std::vector<std::string> candidate_system_dirs(const AppPaths& paths);

bool ensure_directory(const std::string& path);
bool file_exists(const std::string& path);
bool directory_exists(const std::string& path);
std::string path_join(const std::string& a, const std::string& b);
std::string path_basename(const std::string& path);
std::string path_stem(const std::string& path);
std::string path_dirname(const std::string& path);
// Expands a leading "~" and makes the result absolute where possible.
std::string expand_user_path(const std::string& path);

}  // namespace ds
