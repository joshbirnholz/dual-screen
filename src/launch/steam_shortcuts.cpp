#include "launch/steam_shortcuts.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>

namespace ds {

namespace {

// The format's kinds of entry, each a byte before the entry's name.
constexpr uint8_t kMap = 0x00;
constexpr uint8_t kString = 0x01;
constexpr uint8_t kInt = 0x02;
constexpr uint8_t kEnd = 0x08;

class Reader {
public:
    explicit Reader(const std::vector<uint8_t>& bytes) : bytes_(bytes) {}

    bool byte(uint8_t& out) {
        if (at_ >= bytes_.size()) return false;
        out = bytes_[at_++];
        return true;
    }
    bool text(std::string& out) {
        out.clear();
        while (at_ < bytes_.size()) {
            const uint8_t next = bytes_[at_++];
            if (next == 0) return true;
            out.push_back(static_cast<char>(next));
        }
        return false;
    }
    bool u32(uint32_t& out) {
        if (at_ > bytes_.size() || bytes_.size() - at_ < 4) return false;
        out = static_cast<uint32_t>(bytes_[at_]) | (static_cast<uint32_t>(bytes_[at_ + 1]) << 8) |
              (static_cast<uint32_t>(bytes_[at_ + 2]) << 16) | (static_cast<uint32_t>(bytes_[at_ + 3]) << 24);
        at_ += 4;
        return true;
    }
    // Past a map whose name has been read, nested maps and all.
    bool skip_map(int depth = 0) {
        if (depth > 32) return false;
        while (true) {
            uint8_t kind = 0;
            if (!byte(kind)) return false;
            if (kind == kEnd) return true;
            std::string name;
            if (!text(name)) return false;
            if (kind == kMap) {
                if (!skip_map(depth + 1)) return false;
            } else if (kind == kString) {
                std::string value;
                if (!text(value)) return false;
            } else if (kind == kInt) {
                uint32_t value = 0;
                if (!u32(value)) return false;
            } else {
                return false;
            }
        }
    }

private:
    const std::vector<uint8_t>& bytes_;
    std::size_t at_ = 0;
};

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

// One shortcut's map, its name already read.
bool read_shortcut(Reader& reader, SteamShortcut& out) {
    while (true) {
        uint8_t kind = 0;
        if (!reader.byte(kind)) return false;
        if (kind == kEnd) return true;
        std::string name;
        if (!reader.text(name)) return false;
        // Steam has spelt these both ways over the years.
        const std::string key = lower(name);
        if (kind == kMap) {
            if (!reader.skip_map()) return false;
        } else if (kind == kString) {
            std::string value;
            if (!reader.text(value)) return false;
            if (key == "appname") out.name = value;
            else if (key == "exe") out.exe = value;
            else if (key == "startdir") out.start_dir = value;
            else if (key == "launchoptions") out.launch_options = value;
        } else if (kind == kInt) {
            uint32_t value = 0;
            if (!reader.u32(value)) return false;
            if (key == "appid") out.appid = value;
            else if (key == "ishidden") out.hidden = value != 0;
        } else {
            return false;
        }
    }
}

void put_text(std::vector<uint8_t>& out, const std::string& text) {
    out.insert(out.end(), text.begin(), text.end());
    out.push_back(0);
}

void put_string(std::vector<uint8_t>& out, const std::string& name, const std::string& value) {
    out.push_back(kString);
    put_text(out, name);
    put_text(out, value);
}

void put_int(std::vector<uint8_t>& out, const std::string& name, uint32_t value) {
    out.push_back(kInt);
    put_text(out, name);
    for (int shift = 0; shift < 32; shift += 8) out.push_back(static_cast<uint8_t>(value >> shift));
}

// The words of a command line, quotes taken off.
std::vector<std::string> words_of(const std::string& line) {
    std::vector<std::string> words;
    std::string word;
    bool in_word = false;
    char quote = 0;
    for (std::size_t at = 0; at < line.size(); ++at) {
        const char c = line[at];
        if (quote != 0) {
            if (c == quote) {
                quote = 0;
            } else if (c == '\\' && quote == '"' && at + 1 < line.size() &&
                       (line[at + 1] == '"' || line[at + 1] == '\\')) {
                word.push_back(line[++at]);
            } else {
                word.push_back(c);
            }
        } else if (c == '"' || c == '\'') {
            quote = c;
            in_word = true;
        } else if (std::isspace(static_cast<unsigned char>(c)) != 0) {
            if (in_word) words.push_back(word);
            word.clear();
            in_word = false;
        } else {
            word.push_back(c);
            in_word = true;
        }
    }
    if (in_word) words.push_back(word);
    return words;
}

}  // namespace

bool parse_steam_shortcuts(const std::vector<uint8_t>& bytes, std::vector<SteamShortcut>& out) {
    out.clear();
    Reader reader(bytes);
    uint8_t kind = 0;
    std::string name;
    if (!reader.byte(kind) || kind != kMap || !reader.text(name) || lower(name) != "shortcuts") return false;
    while (true) {
        if (!reader.byte(kind) || kind == kEnd) return true;
        if (!reader.text(name)) return true;
        if (kind != kMap) return true;
        SteamShortcut shortcut;
        if (!read_shortcut(reader, shortcut)) return true;
        out.push_back(shortcut);
    }
}

std::vector<uint8_t> write_steam_shortcuts(const std::vector<SteamShortcut>& shortcuts) {
    std::vector<uint8_t> out;
    out.push_back(kMap);
    put_text(out, "shortcuts");
    for (std::size_t index = 0; index < shortcuts.size(); ++index) {
        const SteamShortcut& shortcut = shortcuts[index];
        out.push_back(kMap);
        put_text(out, std::to_string(index));
        put_int(out, "appid", shortcut.appid);
        put_string(out, "AppName", shortcut.name);
        put_string(out, "Exe", shortcut.exe);
        put_string(out, "StartDir", shortcut.start_dir);
        put_string(out, "LaunchOptions", shortcut.launch_options);
        put_int(out, "IsHidden", shortcut.hidden ? 1u : 0u);
        out.push_back(kMap);
        put_text(out, "tags");
        out.push_back(kEnd);
        out.push_back(kEnd);
    }
    out.push_back(kEnd);
    out.push_back(kEnd);
    return out;
}

std::vector<std::string> steam_shortcut_files(const std::string& home) {
    std::vector<std::string> files;
    if (home.empty()) return files;
    const std::filesystem::path base(home);
    const std::filesystem::path roots[] = {
        base / ".local/share/Steam/userdata",
        base / ".steam/steam/userdata",
        base / ".var/app/com.valvesoftware.Steam/.local/share/Steam/userdata",
    };
    std::vector<std::filesystem::path> seen;
    for (const std::filesystem::path& root : roots) {
        std::error_code error;
        if (!std::filesystem::is_directory(root, error)) continue;
        for (const auto& user : std::filesystem::directory_iterator(root, error)) {
            const std::filesystem::path file = user.path() / "config" / "shortcuts.vdf";
            if (!std::filesystem::is_regular_file(file, error)) continue;
            // ~/.steam/steam is usually a link to the first: once each.
            const std::filesystem::path real = std::filesystem::weakly_canonical(file, error);
            if (std::find(seen.begin(), seen.end(), real) != seen.end()) continue;
            seen.push_back(real);
            files.push_back(file.string());
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

bool dualscreen_game_of(const SteamShortcut& shortcut, std::string& game_path) {
    // The program, whichever way it was installed: the app itself, its
    // AppImage, or a script or Flatpak named for it.
    const std::vector<std::string> exe_words = words_of(shortcut.exe);
    std::string program = exe_words.empty() ? shortcut.exe : exe_words.front();
    const std::string base = lower(std::filesystem::path(program).filename().string());
    const std::string whole = lower(shortcut.exe + " " + shortcut.launch_options);
    if (base.find("dualscreen") == std::string::npos && whole.find("dualscreen") == std::string::npos) {
        return false;
    }
    std::vector<std::string> words = exe_words;
    const std::vector<std::string> options = words_of(shortcut.launch_options);
    words.insert(words.end(), options.begin(), options.end());
    for (std::size_t index = 0; index + 1 < words.size(); ++index) {
        if (words[index] == "--game" && !words[index + 1].empty()) {
            game_path = words[index + 1];
            return true;
        }
    }
    return false;
}

}  // namespace ds
