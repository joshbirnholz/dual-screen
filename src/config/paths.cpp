#include "config/paths.hpp"

#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

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

std::string path_extension_lower(const std::string& path) {
    const std::string base = path_basename(path);
    const std::size_t dot = base.find_last_of('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 >= base.size()) {
        return std::string();
    }
    std::string extension = base.substr(dot + 1);
    for (char& character : extension) {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    return extension;
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

std::string absolute_path(const std::string& path) {
    if (path.empty()) return path;

    std::string full = expand_user_path(path);
    if (full.empty() || full.front() != '/') {
        // A relative path is relative to wherever the app was started from,
        // which is the same thing the C library would resolve it against.
        char buffer[4096];
        const char* working = ::getcwd(buffer, sizeof(buffer));
        full = path_join(working != nullptr ? std::string(working) : std::string("/"), full);
    }

    // Fold the path lexically. Without this a "." survives into the result and
    // path_dirname maps it to itself, which is a directory with no way out.
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= full.size()) {
        const std::size_t slash = full.find('/', start);
        const std::size_t end = (slash == std::string::npos) ? full.size() : slash;
        const std::string part = full.substr(start, end - start);
        if (part == "..") {
            if (!parts.empty()) parts.pop_back();
        } else if (!part.empty() && part != ".") {
            parts.push_back(part);
        }
        if (slash == std::string::npos) break;
        start = slash + 1;
    }

    std::string result;
    for (const std::string& part : parts) {
        result += "/";
        result += part;
    }
    return result.empty() ? std::string("/") : result;
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

bool read_file(const std::string& path, std::vector<uint8_t>& out, std::string* error) {
    out.clear();
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        if (error != nullptr) {
            *error = std::string("cannot open ") + path + ": " + std::strerror(errno);
        }
        return false;
    }
    std::fseek(file, 0, SEEK_END);
    const long length = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    if (length < 0) {
        std::fclose(file);
        if (error != nullptr) *error = std::string("cannot size ") + path;
        return false;
    }
    out.resize(static_cast<std::size_t>(length));
    const std::size_t read = out.empty() ? 0 : std::fread(out.data(), 1, out.size(), file);
    const bool ok = (read == out.size()) && std::ferror(file) == 0;
    std::fclose(file);
    if (!ok) {
        out.clear();
        if (error != nullptr) *error = std::string("cannot read ") + path;
        return false;
    }
    return true;
}

bool write_file_atomically(const std::string& path,
                           const void* data,
                           std::size_t size,
                           std::string* error) {
    if (!ensure_directory(path_dirname(path))) {
        if (error != nullptr) *error = "cannot create " + path_dirname(path);
        return false;
    }

    const std::string temporary = path + ".tmp";
    std::FILE* file = std::fopen(temporary.c_str(), "wb");
    if (file == nullptr) {
        if (error != nullptr) {
            *error = std::string("cannot open ") + temporary + ": " + std::strerror(errno);
        }
        return false;
    }

    bool ok = (size == 0) || (std::fwrite(data, 1, size, file) == size);
    if (ok) {
        ok = (std::fflush(file) == 0);
    }
    if (ok) {
        // Without this the rename can land before the contents do, leaving an
        // empty save behind after a power cut.
        ok = (::fsync(::fileno(file)) == 0);
    }
    std::fclose(file);

    if (!ok) {
        std::remove(temporary.c_str());
        if (error != nullptr) *error = std::string("cannot write ") + temporary;
        return false;
    }
    if (std::rename(temporary.c_str(), path.c_str()) != 0) {
        std::remove(temporary.c_str());
        if (error != nullptr) {
            *error = std::string("cannot replace ") + path + ": " + std::strerror(errno);
        }
        return false;
    }
    return true;
}

bool list_directory(const std::string& directory,
                    std::vector<DirectoryEntry>& out_entries,
                    std::string* error) {
    out_entries.clear();

    DIR* handle = ::opendir(directory.c_str());
    if (handle == nullptr) {
        if (error != nullptr) {
            *error = std::string("cannot open ") + directory + ": " + std::strerror(errno);
        }
        return false;
    }

    while (const struct dirent* entry = ::readdir(handle)) {
        const std::string name = entry->d_name;
        // "." and ".." are handled by the caller, and dotfiles are noise here.
        if (name.empty() || name[0] == '.') {
            continue;
        }

        DirectoryEntry listed;
        listed.name = name;
        listed.path = path_join(directory, name);

        // d_type is not filled in on every filesystem, so fall back to stat.
        // stat() also follows symlinks, which is what a browser should do.
        if (entry->d_type == DT_DIR) {
            listed.is_directory = true;
        } else if (entry->d_type == DT_REG) {
            listed.is_directory = false;
        } else {
            struct stat status {};
            if (::stat(listed.path.c_str(), &status) != 0) {
                continue;  // a broken symlink, or something we cannot see
            }
            if (S_ISDIR(status.st_mode)) {
                listed.is_directory = true;
            } else if (S_ISREG(status.st_mode)) {
                listed.is_directory = false;
            } else {
                continue;
            }
        }
        out_entries.push_back(std::move(listed));
    }
    ::closedir(handle);

    std::sort(out_entries.begin(), out_entries.end(),
              [](const DirectoryEntry& a, const DirectoryEntry& b) {
                  if (a.is_directory != b.is_directory) {
                      return a.is_directory;  // directories first
                  }
                  return std::lexicographical_compare(
                      a.name.begin(), a.name.end(), b.name.begin(), b.name.end(),
                      [](unsigned char x, unsigned char y) {
                          return std::tolower(x) < std::tolower(y);
                      });
              });
    return true;
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
    paths.log_file = path_join(path_join(xdg_data, "dualscreen"), "dualscreen.log");
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
