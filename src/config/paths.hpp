#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ds {

// All on-disk locations the app uses, resolved once at startup so nothing else
// has to know a platform's rules.
//
// Where a platform keeps the two kinds of thing this app writes down.
//
// Split out from AppPaths so that the layout is a value rather than a set of
// conditionals: the answer for a platform this is not being compiled for can
// then be asked for, and checked, from one that it is. Which is the whole
// reason -- a directory convention is not something to get wrong quietly on
// somebody else's machine, and it is exactly the sort of thing that cannot be
// noticed from here.
struct PathConvention {
    // Settings and per-game files: things a person might reasonably edit.
    std::string config_root;
    // Saves, the system directory, the log: things the app owns.
    std::string data_root;
    // Where the log goes, which some platforms keep apart from the rest.
    std::string log_root;
};

// Which convention a platform follows.
enum class PathStyle {
    // Freedesktop's: ~/.config and ~/.local/share, overridable by
    // XDG_CONFIG_HOME and XDG_DATA_HOME. Linux, and the Switch and Android
    // builds, which have a writable home of their own and no opinion.
    Xdg,
    // Apple's: everything under ~/Library, with logs in their own place, and
    // no environment variable in sight -- macOS has no XDG convention and a
    // Mac application writing to ~/.config is a Mac application somebody has
    // to explain.
    //
    // iOS shares this. The home directory is the app's own sandbox container
    // rather than a person's, but the shape inside it is the same and
    // Application Support means the same thing: what the app owns, kept, and
    // backed up.
    AppleLibrary,
    // tvOS, which is the one Apple platform with nowhere permanent to write.
    //
    // There is no Application Support and no Documents on an Apple TV. An app
    // gets Library/Caches, which the system may empty whenever it wants the
    // space back, and tmp, which is worse. Apple's own answer for anything
    // that must survive is iCloud key-value storage, which is a megabyte of
    // preferences and not a place for files.
    //
    // So this writes to Caches and accepts what that means: settings can go
    // away between sessions, and an Apple TV that has forgotten its settings
    // comes back with the defaults. For a companion that is a small loss --
    // it finds hosts by looking rather than by remembering, and there are no
    // saves on this side of the link, because the game is running on the
    // other machine.
    AppleCaches,
    // A Switch: everything on the SD card beside the app itself, in
    // /switch/dualscreen, where homebrew keeps its files and where a player
    // copying saves off the card will look. There is no home directory, and
    // HOME is not set, so the XDG answer would be wherever the app happened
    // to be started from.
    SwitchSd,
};

// This build's own, which is the only one resolve() below will use.
PathStyle native_path_style();

// A Switch path without the SD card's device name in front of it: "sdmc:/roms"
// becomes "/roms", which the Switch's C library resolves against the SD card
// all the same. Everything that walks a path upwards assumes it starts at "/",
// and a path from the console -- a forwarder's argument, getcwd() -- does not.
// Anything else comes back unchanged, and on every other device this is the
// identity.
std::string without_sd_device(const std::string& path);

// The roots a style puts things under, given a home directory and a way to
// read the environment. `environment` returns an empty string for a variable
// that is unset, which is what makes this answerable without one.
PathConvention path_convention(PathStyle style, const std::string& home,
                               const std::function<std::string(const char*)>& environment);

struct AppPaths {
    std::string config_dir;    // ~/.config/dualscreen, or ~/Library/Application Support
    std::string games_dir;     // .../games
    std::string settings_file; // .../settings.json
    std::string saves_dir;     // .../saves
    std::string default_system_dir;  // .../system
    std::string log_file;            // .../dualscreen.log

    static AppPaths resolve();
    // The same thing from a convention already chosen, so the layout under a
    // root can be checked without being on that platform.
    static AppPaths under(const PathConvention& convention);
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
// Puts `temporary` where `path` is, replacing whatever was there -- the last
// step of writing a file safely. A rename, where the file system lets one
// replace a file; the Switch's SD card does not, and there the old file is
// moved aside to `path`.old first and removed once the new one is in place,
// so a power cut between the two leaves the old contents behind rather than
// nothing (read_file() falls back to them). False, with the reason, if the
// new file could not be put in place; the old one is then where it was.
bool replace_file(const std::string& temporary, const std::string& path, std::string* error);
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
