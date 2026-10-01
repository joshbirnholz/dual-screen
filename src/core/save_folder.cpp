#include "core/save_folder.hpp"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "config/paths.hpp"

namespace ds {
namespace {

constexpr char kMagic[8] = {'D', 'S', 'F', 'O', 'L', 'D', 'E', 'R'};
constexpr uint32_t kVersion = 1;
constexpr uint8_t kKindDirectory = 0;
constexpr uint8_t kKindFile = 1;
// Far more than any console's save, and small enough that a corrupt archive
// cannot ask for the device's whole memory.
constexpr uint64_t kMaxFileSize = 256ull << 20;

struct Entry {
    std::string path;  // relative, '/' between parts
    bool directory = false;
};

// Everything under `root`, dotfiles included -- an emulator's save is whatever
// it wrote, whatever it is called.
bool walk(const std::string& root, const std::string& relative, std::vector<Entry>& out, std::string* error) {
    const std::string here = relative.empty() ? root : path_join(root, relative);
    DIR* handle = ::opendir(here.c_str());
    if (handle == nullptr) {
        if (error != nullptr) *error = "cannot open " + here + ": " + std::strerror(errno);
        return false;
    }
    std::vector<Entry> children;
    while (const struct dirent* entry = ::readdir(handle)) {
        const std::string name = entry->d_name;
        if (name == "." || name == "..") continue;
        const std::string child = relative.empty() ? name : relative + "/" + name;
        struct stat status {};
        if (::stat(path_join(root, child).c_str(), &status) != 0) continue;
        if (S_ISDIR(status.st_mode)) {
            children.push_back({child, true});
        } else if (S_ISREG(status.st_mode)) {
            children.push_back({child, false});
        }
    }
    ::closedir(handle);
    std::sort(children.begin(), children.end(), [](const Entry& a, const Entry& b) { return a.path < b.path; });
    for (const Entry& child : children) {
        out.push_back(child);
        if (child.directory && !walk(root, child.path, out, error)) return false;
    }
    return true;
}

void put_u16(std::vector<uint8_t>& out, uint16_t value) {
    out.push_back(static_cast<uint8_t>(value));
    out.push_back(static_cast<uint8_t>(value >> 8));
}
void put_u32(std::vector<uint8_t>& out, uint32_t value) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(value >> (i * 8)));
}
void put_u64(std::vector<uint8_t>& out, uint64_t value) {
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<uint8_t>(value >> (i * 8)));
}

struct Reader {
    const std::vector<uint8_t>& bytes;
    std::size_t at = 0;
    bool ok = true;

    uint64_t take(int width) {
        if (!ok || bytes.size() - at < static_cast<std::size_t>(width)) {
            ok = false;
            return 0;
        }
        uint64_t value = 0;
        for (int i = 0; i < width; ++i) value |= uint64_t(bytes[at + i]) << (i * 8);
        at += static_cast<std::size_t>(width);
        return value;
    }
};

// A path an archive may name: relative, every part a real name.
bool safe_path(const std::string& path) {
    if (path.empty() || path[0] == '/' || path.find('\\') != std::string::npos ||
        path.find('\0') != std::string::npos) {
        return false;
    }
    std::size_t start = 0;
    while (start <= path.size()) {
        const std::size_t slash = path.find('/', start);
        const std::string part = path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (part.empty() || part == "." || part == "..") return false;
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    return true;
}

bool remove_tree(const std::string& path) {
    struct stat status {};
    if (::lstat(path.c_str(), &status) != 0) return errno == ENOENT;
    if (!S_ISDIR(status.st_mode)) return std::remove(path.c_str()) == 0;
    if (DIR* handle = ::opendir(path.c_str())) {
        while (const struct dirent* entry = ::readdir(handle)) {
            const std::string name = entry->d_name;
            if (name == "." || name == "..") continue;
            remove_tree(path_join(path, name));
        }
        ::closedir(handle);
    }
    return ::rmdir(path.c_str()) == 0;
}

bool make_directories(const std::string& path) {
    if (path.empty() || directory_exists(path)) return true;
    const std::string parent = path_dirname(path);
    if (!parent.empty() && parent != path && !make_directories(parent)) return false;
    return ensure_directory(path);
}

}  // namespace

bool pack_save_folder(const std::string& directory, std::vector<uint8_t>& out, std::string* error) {
    out.assign(kMagic, kMagic + sizeof(kMagic));
    put_u32(out, kVersion);
    std::vector<Entry> entries;
    if (directory_exists(directory) && !walk(directory, "", entries, error)) return false;
    put_u32(out, static_cast<uint32_t>(entries.size()));
    for (const Entry& entry : entries) {
        out.push_back(entry.directory ? kKindDirectory : kKindFile);
        put_u16(out, static_cast<uint16_t>(entry.path.size()));
        out.insert(out.end(), entry.path.begin(), entry.path.end());
        if (entry.directory) continue;
        std::vector<uint8_t> contents;
        if (!read_file(path_join(directory, entry.path), contents, error)) {
            if (error != nullptr && error->empty()) *error = "cannot read " + entry.path;
            return false;
        }
        put_u64(out, contents.size());
        out.insert(out.end(), contents.begin(), contents.end());
    }
    return true;
}

bool unpack_save_folder(const std::vector<uint8_t>& archive, const std::string& directory,
                        std::string* error) {
    const auto fail = [&](const std::string& why) {
        if (error != nullptr) *error = why;
        return false;
    };
    if (archive.size() < sizeof(kMagic) + 8 || std::memcmp(archive.data(), kMagic, sizeof(kMagic)) != 0) {
        return fail("that is not a save folder");
    }
    Reader reader{archive, sizeof(kMagic)};
    if (reader.take(4) != kVersion) return fail("that save folder is from a newer version of Dualscreen");
    const uint64_t count = reader.take(4);

    // Written beside the real one, and swapped in only once all of it is.
    const std::string incoming = directory + ".incoming";
    remove_tree(incoming);
    if (!make_directories(incoming)) return fail("could not make " + incoming);
    for (uint64_t index = 0; index < count && reader.ok; ++index) {
        const uint64_t kind = reader.take(1);
        const uint64_t length = reader.take(2);
        if (!reader.ok || archive.size() - reader.at < length) break;
        const std::string path(reinterpret_cast<const char*>(archive.data() + reader.at), length);
        reader.at += length;
        if (!safe_path(path)) {
            remove_tree(incoming);
            return fail("the save folder names a file outside itself");
        }
        const std::string target = path_join(incoming, path);
        if (kind == kKindDirectory) {
            if (!make_directories(target)) {
                remove_tree(incoming);
                return fail("could not make " + target);
            }
            continue;
        }
        const uint64_t size = reader.take(8);
        if (kind != kKindFile || !reader.ok || size > kMaxFileSize || archive.size() - reader.at < size) {
            reader.ok = false;
            break;
        }
        std::string write_error;
        if (!make_directories(path_dirname(target)) ||
            !write_file_atomically(target, archive.data() + reader.at, static_cast<std::size_t>(size),
                                   &write_error)) {
            remove_tree(incoming);
            return fail("could not write " + target + (write_error.empty() ? "" : ": " + write_error));
        }
        reader.at += static_cast<std::size_t>(size);
    }
    if (!reader.ok || reader.at != archive.size()) {
        remove_tree(incoming);
        return fail("the save folder is cut short or damaged");
    }

    const std::string outgoing = directory + ".outgoing";
    remove_tree(outgoing);
    if (directory_exists(directory) && std::rename(directory.c_str(), outgoing.c_str()) != 0) {
        remove_tree(incoming);
        return fail("could not move " + directory + " aside");
    }
    make_directories(path_dirname(directory));
    if (std::rename(incoming.c_str(), directory.c_str()) != 0) {
        std::rename(outgoing.c_str(), directory.c_str());
        remove_tree(incoming);
        return fail("could not put the new save in " + directory);
    }
    remove_tree(outgoing);
    return true;
}

bool save_folder_has_files(const std::string& directory) {
    std::vector<Entry> entries;
    if (!directory_exists(directory) || !walk(directory, "", entries, nullptr)) return false;
    return std::any_of(entries.begin(), entries.end(), [](const Entry& entry) { return !entry.directory; });
}

uint64_t save_folder_size(const std::string& directory) {
    std::vector<Entry> entries;
    if (!directory_exists(directory) || !walk(directory, "", entries, nullptr)) return 0;
    uint64_t total = 0;
    for (const Entry& entry : entries) {
        struct stat status {};
        if (!entry.directory && ::stat(path_join(directory, entry.path).c_str(), &status) == 0) {
            total += static_cast<uint64_t>(status.st_size);
        }
    }
    return total;
}

int64_t save_folder_modified_at(const std::string& directory) {
    std::vector<Entry> entries;
    if (!directory_exists(directory) || !walk(directory, "", entries, nullptr)) return 0;
    int64_t newest = 0;
    for (const Entry& entry : entries) {
        struct stat status {};
        if (!entry.directory && ::stat(path_join(directory, entry.path).c_str(), &status) == 0) {
            newest = std::max<int64_t>(newest, static_cast<int64_t>(status.st_mtime));
        }
    }
    return newest;
}

bool remove_save_folder(const std::string& directory) { return remove_tree(directory); }

}  // namespace ds
