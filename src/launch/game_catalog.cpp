#include "launch/game_catalog.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>

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

std::vector<LaunchableGame> launchable_games(const std::string& home, const std::vector<std::string>& folders) {
    std::vector<LaunchableGame> games = games_from_steam_shortcuts(home);
    if (!games.empty()) return games;
    return games_from_folders(folders);
}

}  // namespace ds
