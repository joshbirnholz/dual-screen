#include "config/paths.hpp"

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>

#include "util/log.hpp"

namespace ds {
namespace {

std::string home_dir() {
    const char* home = std::getenv("HOME");
    if (home != nullptr && home[0] != '\0') {
        return std::string(home);
    }
    return std::string(".");
}

std::string env_or(const char* name, const std::string& fallback) {
    const char* value = std::getenv(name);
    if (value != nullptr && value[0] != '\0') {
        return std::string(value);
    }
    return fallback;
}

}  // namespace

std::string path_join(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    if (b.front() == '/') return b;
    if (a.back() == '/') return a + b;
    return a + "/" + b;
}

std::string path_basename(const std::string& path) {
    const std::size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string path_dirname(const std::string& path) {
    const std::size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) return std::string(".");
    if (slash == 0) return std::string("/");
    return path.substr(0, slash);
}

std::string path_stem(const std::string& path) {
    std::string base = path_basename(path);
    const std::size_t dot = base.find_last_of('.');
    if (dot == std::string::npos || dot == 0) return base;
    return base.substr(0, dot);
}

std::string expand_user_path(const std::string& path) {
    if (path.empty()) return path;
    if (path[0] == '~' && (path.size() == 1 || path[1] == '/')) {
        // Drop the '/' as well as the '~': path_join treats a leading slash as
        // an absolute path and would discard the home directory entirely.
        return path.size() == 1 ? home_dir() : path_join(home_dir(), path.substr(2));
    }
    return path;
}

bool file_exists(const std::string& path) {
    struct stat st {};
    return !path.empty() && ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool directory_exists(const std::string& path) {
    struct stat st {};
    return !path.empty() && ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool ensure_directory(const std::string& path) {
    if (path.empty()) return false;
    if (directory_exists(path)) return true;

    // Create every missing component, mkdir -p style.
    std::string partial;
    std::size_t index = 0;
    if (path[0] == '/') {
        partial = "/";
        index = 1;
    }
    while (index <= path.size()) {
        const std::size_t slash = path.find('/', index);
        const std::size_t end = (slash == std::string::npos) ? path.size() : slash;
        const std::string component = path.substr(index, end - index);
        if (!component.empty()) {
            partial = path_join(partial, component);
            if (::mkdir(partial.c_str(), 0755) != 0 && errno != EEXIST) {
                log_error("could not create directory " + partial + ": " + std::strerror(errno));
                return false;
            }
        }
        if (slash == std::string::npos) break;
        index = slash + 1;
    }
    return directory_exists(path);
}

AppPaths AppPaths::resolve() {
    const std::string home = home_dir();
    const std::string xdg_config = env_or("XDG_CONFIG_HOME", path_join(home, ".config"));
    const std::string xdg_data = env_or("XDG_DATA_HOME", path_join(home, ".local/share"));

    AppPaths paths;
    paths.config_dir = path_join(xdg_config, "dualscreen");
    paths.games_dir = path_join(paths.config_dir, "games");
    paths.settings_file = path_join(paths.config_dir, "settings.json");
    paths.saves_dir = path_join(path_join(xdg_data, "dualscreen"), "saves");
    paths.default_system_dir = path_join(path_join(xdg_data, "dualscreen"), "system");
    return paths;
}

std::vector<std::string> candidate_system_dirs(const AppPaths& paths) {
    std::vector<std::string> candidates;
    candidates.push_back(paths.default_system_dir);

    const std::string home = home_dir();
    const std::string others[] = {
        path_join(paths.config_dir, "system"),
        path_join(home, ".var/app/org.libretro.RetroArch/config/retroarch/system"),
        path_join(home, ".config/retroarch/system"),
        path_join(home, ".local/share/retroarch/system"),
        path_join(home, ".var/app/net.kuribo64.melonDS/data/melonDS"),
        path_join(home, ".config/melonDS"),
        "/usr/share/dualscreen/system",
    };
    for (const std::string& candidate : others) {
        if (directory_exists(candidate)) {
            candidates.push_back(candidate);
        }
    }
    return candidates;
}

}  // namespace ds
