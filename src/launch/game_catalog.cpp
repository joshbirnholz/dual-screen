#include "launch/game_catalog.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>

#include <nlohmann/json.hpp>

#include "core/system.hpp"
#include "launch/steam_shortcuts.hpp"

namespace ds {

namespace {

bool read_file(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

void by_title(std::vector<LaunchableGame>& games) {
    std::stable_sort(games.begin(), games.end(),
                     [](const LaunchableGame& a, const LaunchableGame& b) { return a.title < b.title; });
}

}  // namespace

std::vector<LaunchableGame> games_from_steam_shortcuts(const std::string& home) {
    std::vector<LaunchableGame> games;
    std::set<uint64_t> seen;
    for (const std::string& file : steam_shortcut_files(home)) {
        std::vector<uint8_t> bytes;
        std::vector<SteamShortcut> shortcuts;
        if (!read_file(file, bytes) || !parse_steam_shortcuts(bytes, shortcuts)) continue;
        for (const SteamShortcut& shortcut : shortcuts) {
            std::string path;
            if (shortcut.hidden || !dualscreen_game_of(shortcut, path)) continue;
            if (!seen.insert(shortcut.run_game_id()).second) continue;
            LaunchableGame game;
            game.id = "steam:" + std::to_string(shortcut.run_game_id());
            game.title = shortcut.name.empty() ? std::filesystem::path(path).stem().string() : shortcut.name;
            SystemId system = SystemId::Ds;
            game.system = system_for_rom(path, system) ? system_key(system) : "";
            game.path = path;
            game.steam_run_id = shortcut.run_game_id();
            games.push_back(game);
        }
    }
    by_title(games);
    return games;
}

std::vector<LaunchableGame> games_from_folders(const std::vector<std::string>& folders) {
    std::vector<LaunchableGame> games;
    std::set<std::string> seen;
    const auto consider = [&](const std::filesystem::path& file) {
        SystemId system = SystemId::Ds;
        // A Wii U game is a folder of its own, not a file to list.
        if (!system_for_rom(file.string(), system) || system == SystemId::WiiU) return;
        if (!seen.insert(file.string()).second) return;
        LaunchableGame game;
        game.id = "file:" + file.string();
        game.title = file.stem().string();
        game.system = system_key(system);
        game.path = file.string();
        games.push_back(game);
    };
    for (const std::string& folder : folders) {
        std::error_code error;
        if (folder.empty() || !std::filesystem::is_directory(folder, error)) continue;
        for (const auto& entry : std::filesystem::directory_iterator(folder, error)) {
            if (entry.is_regular_file(error)) {
                consider(entry.path());
            } else if (entry.is_directory(error)) {
                for (const auto& inner : std::filesystem::directory_iterator(entry.path(), error)) {
                    if (inner.is_regular_file(error)) consider(inner.path());
                }
            }
        }
    }
    by_title(games);
    return games;
}

namespace {

// The same file however it was named: a shortcut's quoted path, or one
// through a symlink, against the path the game was opened from.
std::string same_file_key(const std::string& path) {
    std::error_code error;
    const std::filesystem::path canonical = std::filesystem::weakly_canonical(path, error);
    return error ? path : canonical.string();
}

// At most this many kept: more than any row shows.
constexpr std::size_t kPlayedKept = 200;

}  // namespace

void remember_played(const std::string& record_path, const std::string& rom_path, int64_t when) {
    using json = nlohmann::json;
    json record = json::object();
    {
        std::ifstream in(record_path);
        if (in) {
            try {
                record = json::parse(in);
            } catch (const json::exception&) {
                record = json::object();
            }
        }
    }
    if (!record.is_object()) record = json::object();
    record[same_file_key(rom_path)] = when;
    // The oldest go once there are too many.
    while (record.size() > kPlayedKept) {
        auto oldest = record.begin();
        for (auto entry = record.begin(); entry != record.end(); ++entry) {
            if (entry->is_number() && (!oldest->is_number() || entry->get<int64_t>() < oldest->get<int64_t>())) {
                oldest = entry;
            }
        }
        record.erase(oldest);
    }
    std::error_code error;
    std::filesystem::create_directories(std::filesystem::path(record_path).parent_path(), error);
    const std::string temporary = record_path + ".tmp";
    {
        std::ofstream out(temporary, std::ios::trunc);
        if (!out) return;
        out << record.dump(2);
    }
    std::filesystem::rename(temporary, record_path, error);
}

std::vector<std::pair<std::string, int64_t>> played_games(const std::string& record_path) {
    using json = nlohmann::json;
    std::vector<std::pair<std::string, int64_t>> played;
    std::ifstream in(record_path);
    if (!in) return played;
    try {
        const json record = json::parse(in);
        if (!record.is_object()) return played;
        for (auto entry = record.begin(); entry != record.end(); ++entry) {
            if (entry->is_number()) played.emplace_back(entry.key(), entry->get<int64_t>());
        }
    } catch (const json::exception&) {
    }
    return played;
}

void add_played(std::vector<LaunchableGame>& games, const std::string& record_path) {
    std::map<std::string, int64_t> when;
    for (const auto& [path, at] : played_games(record_path)) when[path] = at;
    std::set<std::string> listed;
    for (LaunchableGame& game : games) {
        const std::string key = same_file_key(game.path);
        listed.insert(key);
        const auto found = when.find(key);
        if (found != when.end()) game.last_played = found->second;
    }
    for (const auto& [path, at] : when) {
        std::error_code error;
        SystemId system = SystemId::Ds;
        if (listed.count(path) != 0 || !system_for_rom(path, system) || !std::filesystem::exists(path, error)) {
            continue;
        }
        LaunchableGame game;
        game.id = "file:" + path;
        game.title = std::filesystem::path(path).stem().string();
        game.system = system_key(system);
        game.path = path;
        game.last_played = at;
        games.push_back(game);
    }
}

std::vector<LaunchableGame> launchable_games(const std::string& home, const std::vector<std::string>& folders) {
    std::vector<LaunchableGame> games = games_from_steam_shortcuts(home);
    if (!games.empty()) return games;
    return games_from_folders(folders);
}

}  // namespace ds
