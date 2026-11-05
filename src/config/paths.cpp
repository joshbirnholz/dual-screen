#include "config/paths.hpp"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#if defined(_WIN32)
#include <direct.h>
#include <io.h>
#include <windows.h>
#endif

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
#if defined(_WIN32)
    // USERPROFILE is the home folder on Windows; HOME is only set by shells
    // brought over from elsewhere (Git Bash, MSYS2), which would put files
    // somewhere a Windows program would not.
    for (const char* name : {"USERPROFILE", "HOME"}) {
        const char* home = std::getenv(name);
        if (home != nullptr && home[0] != '\0') return native_to_generic_path(home);
    }
    return std::string(".");
#else
    const char* home = std::getenv("HOME");
    if (home != nullptr && home[0] != '\0') {
        return std::string(home);
    }
    return std::string(".");
#endif
}

}  // namespace

std::string native_to_generic_path(const std::string& path) {
#if defined(_WIN32)
    std::string generic = path;
    std::replace(generic.begin(), generic.end(), '\\', '/');
    return generic;
#else
    return path;
#endif
}

std::string generic_to_native_path(const std::string& path) {
#if defined(_WIN32)
    std::string native = path;
    std::replace(native.begin(), native.end(), '/', '\\');
    return native;
#else
    return path;
#endif
}

std::size_t path_root_length(const std::string& path) {
#if defined(_WIN32)
    // "C:/..." and "C:\..." -- a drive. (A bare "C:" is relative to that
    // drive's working directory, which nothing here means; it counts as the
    // drive's root.)
    if (path.size() >= 2 && std::isalpha(static_cast<unsigned char>(path[0])) && path[1] == ':') {
        return path.size() >= 3 && (path[2] == '/' || path[2] == '\\') ? 3 : 2;
    }
    // "//server/share/..." -- a network share. Its root is all of that.
    if (path.size() >= 2 && (path[0] == '/' || path[0] == '\\') && (path[1] == '/' || path[1] == '\\')) {
        std::size_t slashes = 0;
        for (std::size_t index = 2; index < path.size(); ++index) {
            if (path[index] == '/' || path[index] == '\\') {
                if (++slashes == 2) return index + 1;
            }
        }
        return path.size();
    }
    if (!path.empty() && (path[0] == '/' || path[0] == '\\')) return 1;
    return 0;
#else
    return !path.empty() && path[0] == '/' ? 1 : 0;
#endif
}

bool is_absolute_path(const std::string& path) { return path_root_length(path) > 0; }

std::string path_join(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    if (is_absolute_path(b)) return b;
    if (a.back() == '/') return a + b;
#if defined(_WIN32)
    if (a.back() == '\\') return a + b;
#endif
    return a + "/" + b;
}

namespace {
std::size_t last_separator(const std::string& path) {
#if defined(_WIN32)
    return path.find_last_of("/\\");
#else
    return path.find_last_of('/');
#endif
}
}  // namespace

std::string path_basename(const std::string& path) {
    const std::size_t slash = last_separator(path);
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string path_dirname(const std::string& path) {
    const std::size_t slash = last_separator(path);
    if (slash == std::string::npos) return std::string(".");
    const std::size_t root = path_root_length(path);
    if (root > 0 && slash < root) return path.substr(0, root);
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

std::string without_sd_device(const std::string& path) {
#if defined(__SWITCH__)
    constexpr const char kDevice[] = "sdmc:";
    constexpr std::size_t kLength = sizeof(kDevice) - 1;
    if (path.compare(0, kLength, kDevice) == 0) {
        const std::string rest = path.substr(kLength);
        return rest.empty() || rest.front() != '/' ? "/" + rest : rest;
    }
#endif
    return path;
}

std::string expand_user_path(const std::string& raw) {
    const std::string path = native_to_generic_path(without_sd_device(raw));
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
    if (full.empty() || !is_absolute_path(full)) {
        // A relative path is relative to wherever the app was started from,
        // which is the same thing the C library would resolve it against.
        char buffer[4096];
        const char* working = ::getcwd(buffer, sizeof(buffer));
        full = path_join(working != nullptr
                             ? native_to_generic_path(without_sd_device(std::string(working)))
                             : std::string("/"),
                         full);
    }
    // The root ("/", "C:/", "//server/share/") is kept as it is; only what
    // follows it is folded.
    std::size_t root_length = path_root_length(full);
    std::string root = full.substr(0, root_length);
    if (!root.empty() && root.back() != '/') root += '/';  // "C:" -> "C:/"
    full = full.substr(root_length);

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

    std::string result = root.empty() ? std::string("/") : root;
    for (std::size_t index = 0; index < parts.size(); ++index) {
        if (index > 0) result += "/";
        result += parts[index];
    }
    return result;
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
    std::size_t index = path_root_length(path);
    if (index > 0) partial = path.substr(0, index);
    while (index <= path.size()) {
        const std::size_t slash = path.find('/', index);
        const std::size_t end = (slash == std::string::npos) ? path.size() : slash;
        const std::string component = path.substr(index, end - index);
        if (!component.empty()) {
            partial = path_join(partial, component);
#if defined(_WIN32)
            const int made = ::_mkdir(partial.c_str());
#else
            const int made = ::mkdir(partial.c_str(), 0755);
#endif
            if (made != 0 && errno != EEXIST) {
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
    if (file == nullptr && errno == ENOENT) {
        // A replace_file() cut short between moving the old file aside and
        // putting the new one in place: the old contents are all there is.
        file = std::fopen((path + ".old").c_str(), "rb");
        if (file == nullptr) errno = ENOENT;
    }
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
#if defined(_WIN32)
        ok = (::_commit(::_fileno(file)) == 0);
#else
        ok = (::fsync(::fileno(file)) == 0);
#endif
    }
    std::fclose(file);

    if (!ok) {
        std::remove(temporary.c_str());
        if (error != nullptr) *error = std::string("cannot write ") + temporary;
        return false;
    }
    if (!replace_file(temporary, path, error)) {
        std::remove(temporary.c_str());
        return false;
    }
    return true;
}

bool replace_file(const std::string& temporary, const std::string& path, std::string* error) {
#if defined(_WIN32)
    // rename() on Windows will not replace a file that is there; this does,
    // in one step, as rename() does elsewhere.
    if (::MoveFileExA(temporary.c_str(), path.c_str(),
                      MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0) {
        return true;
    }
#endif
    if (std::rename(temporary.c_str(), path.c_str()) == 0) {
        return true;
    }
    const int rename_errno = errno;
    // Nothing there to replace, so the rename failed for some other reason,
    // and moving things aside would not help.
    if (!file_exists(path)) {
        if (error != nullptr) {
            *error = std::string("cannot replace ") + path + ": " + std::strerror(rename_errno);
        }
        return false;
    }
    // A file system whose rename will not replace an existing file -- the
    // Switch's, which is FAT underneath and says so only by failing. A
    // leftover from an earlier replace cut short goes first: the file at
    // `path` is newer than it.
    const std::string aside = path + ".old";
    std::remove(aside.c_str());
    if (std::rename(path.c_str(), aside.c_str()) != 0) {
        if (error != nullptr) {
            *error = std::string("cannot move ") + path + " aside to replace it: " +
                     std::strerror(errno);
        }
        return false;
    }
    if (std::rename(temporary.c_str(), path.c_str()) != 0) {
        const int second_errno = errno;
        std::rename(aside.c_str(), path.c_str());
        if (error != nullptr) {
            *error = std::string("cannot replace ") + path + ": " + std::strerror(second_errno);
        }
        return false;
    }
    std::remove(aside.c_str());
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
        // Windows's dirent has no d_type at all, so it always asks stat().
#if defined(_WIN32)
        const bool typed = false;
#else
        const bool typed = entry->d_type == DT_DIR || entry->d_type == DT_REG;
#endif
        if (typed) {
#if !defined(_WIN32)
            listed.is_directory = entry->d_type == DT_DIR;
#endif
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

PathStyle native_path_style() {
#if defined(__SWITCH__)
    return PathStyle::SwitchSd;
#elif defined(_WIN32)
    return PathStyle::Windows;
#elif defined(__APPLE__)
#if TARGET_OS_TV
    return PathStyle::AppleCaches;
#else
    return PathStyle::AppleLibrary;
#endif
#else
    return PathStyle::Xdg;
#endif
}

PathConvention path_convention(PathStyle style, const std::string& home,
                               const std::function<std::string(const char*)>& environment) {
    PathConvention convention;
    if (style == PathStyle::AppleCaches) {
        // Everything in the one place the system will let an Apple TV keep a
        // file at all, including the log -- Library/Logs is not writable here
        // and there is no Console to read it with anyway. See PathStyle.
        const std::string caches = path_join(path_join(home, "Library"), "Caches");
        convention.config_root = caches;
        convention.data_root = caches;
        convention.log_root = caches;
        return convention;
    }
    if (style == PathStyle::AppleLibrary) {
        // Where a Mac application is expected to put things, and where a
        // person looking for them will look. Application Support for what the
        // app owns, Logs for the log, and no environment variable in either:
        // XDG_CONFIG_HOME on a Mac is a variable nothing sets and nothing
        // reads, so honouring it would only mean a second place for the same
        // file to hide in.
        //
        // Settings and saves share a root here, unlike the XDG split. That is
        // the platform's own arrangement rather than a shortcut -- macOS
        // separates by *who owns a file*, not by what kind of file it is, and
        // both of these are the application's.
        const std::string library = path_join(home, "Library");
        convention.config_root = path_join(library, "Application Support");
        convention.data_root = convention.config_root;
        convention.log_root = path_join(library, "Logs");
        return convention;
    }
    if (style == PathStyle::SwitchSd) {
        // One root for all of it, /switch, so AppPaths::under() lands
        // everything in /switch/dualscreen beside the app. `home` means
        // nothing here and is not used.
        convention.config_root = "/switch";
        convention.data_root = convention.config_root;
        convention.log_root = convention.config_root;
        return convention;
    }
    if (style == PathStyle::Windows) {
        // Settings in the roaming profile, which follows a person between
        // PCs on a domain; saves, the system folder and the log in the local
        // one. Both as the environment says, with the usual places under the
        // home folder when it does not.
        // Slashes made forward whichever machine is asking: the style is
        // Windows's, so its separators are too.
        const auto forward = [](std::string path) {
            std::replace(path.begin(), path.end(), '\\', '/');
            return path;
        };
        const std::string roaming = forward(environment("APPDATA"));
        const std::string local = forward(environment("LOCALAPPDATA"));
        convention.config_root = roaming.empty() ? path_join(home, "AppData/Roaming") : roaming;
        convention.data_root = local.empty() ? path_join(home, "AppData/Local") : local;
        convention.log_root = convention.data_root;
        return convention;
    }
    const std::string config = environment("XDG_CONFIG_HOME");
    const std::string data = environment("XDG_DATA_HOME");
    convention.config_root = config.empty() ? path_join(home, ".config") : config;
    convention.data_root = data.empty() ? path_join(home, ".local/share") : data;
    convention.log_root = convention.data_root;
    return convention;
}

AppPaths AppPaths::under(const PathConvention& convention) {
    AppPaths paths;
    paths.config_dir = path_join(convention.config_root, "dualscreen");
    paths.games_dir = path_join(paths.config_dir, "games");
    paths.settings_file = path_join(paths.config_dir, "settings.json");
    const std::string data = path_join(convention.data_root, "dualscreen");
    paths.data_dir = data;
    paths.saves_dir = path_join(data, "saves");
    paths.default_system_dir = path_join(data, "system");
    paths.log_file = path_join(path_join(convention.log_root, "dualscreen"), "dualscreen.log");
    return paths;
}

AppPaths AppPaths::resolve() {
    return under(path_convention(native_path_style(), home_dir(), [](const char* name) {
        const char* value = std::getenv(name);
        return std::string(value != nullptr ? value : "");
    }));
}

std::vector<std::string> candidate_system_dirs(const AppPaths& paths) {
    std::vector<std::string> candidates;
    candidates.push_back(paths.default_system_dir);

    const std::string home = home_dir();
#if defined(__SWITCH__)
    // RetroArch's own, on the same card, offered in Settings: a player who
    // has set up melonDS there already has its BIOS and firmware in place.
    if (directory_exists("/retroarch/cores/system")) {
        candidates.push_back("/retroarch/cores/system");
    }
#endif
#if defined(_WIN32)
    // RetroArch's on Windows: in the roaming profile when installed, or
    // beside retroarch.exe in its usual folders.
    const char* appdata = std::getenv("APPDATA");
    const std::string roaming = appdata != nullptr ? native_to_generic_path(appdata) : std::string();
    const std::string windows_others[] = {
        roaming.empty() ? std::string() : path_join(roaming, "RetroArch/system"),
        "C:/RetroArch-Win64/system",
        "C:/RetroArch/system",
        roaming.empty() ? std::string() : path_join(roaming, "melonDS"),
    };
    for (const std::string& candidate : windows_others) {
        if (!candidate.empty() && directory_exists(candidate)) candidates.push_back(candidate);
    }
#endif
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
