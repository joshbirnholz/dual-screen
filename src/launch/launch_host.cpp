#include "launch/launch_host.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "config/paths.hpp"
#include "config/settings.hpp"
#include "core/console_title_info.hpp"
#include "core/ds_rom_info.hpp"
#include "core/save_store.hpp"
#include "core/system.hpp"
#include "net/launch_service.hpp"
#include "net/save_exchange.hpp"
#include "util/child_process.hpp"
#include "util/log.hpp"

#if defined(__linux__) && !defined(__ANDROID__)
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#define DUALSCREEN_CAN_SPAWN 1
#endif

namespace ds {

namespace {

constexpr const char* kUnitName = "dualscreen-service.service";

std::atomic<bool> g_stop{false};

void on_stop_signal(int) { g_stop = true; }

std::string home_dir() {
    const char* home = std::getenv("HOME");
    return home != nullptr ? home : "";
}

std::string unit_path() {
    const char* config = std::getenv("XDG_CONFIG_HOME");
    const std::string base = config != nullptr && *config != '\0' ? config : home_dir() + "/.config";
    return base + "/systemd/user/" + kUnitName;
}

// Where the ROMs are likely to be, when Steam has no shortcuts: the folder
// the file picker was last in, its parent, and EmuDeck's.
std::vector<std::string> rom_folders(const Settings& settings) {
    std::vector<std::string> folders;
    if (!settings.last_browse_dir.empty()) {
        folders.push_back(settings.last_browse_dir);
        folders.push_back(std::filesystem::path(settings.last_browse_dir).parent_path().string());
    }
    const std::string home = home_dir();
    if (!home.empty()) {
        for (const char* console : {"nds", "n3ds", "3ds"}) {
            folders.push_back(home + "/Emulation/roms/" + console);
        }
    }
    return folders;
}

// Runs a program to its end, for systemctl: true when it said yes.
bool run_and_wait(const std::string& program, const std::vector<std::string>& arguments, std::string* error) {
#ifdef DUALSCREEN_CAN_SPAWN
    const pid_t pid = fork();
    if (pid < 0) {
        if (error != nullptr) *error = "could not run " + program;
        return false;
    }
    if (pid == 0) {
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(program.c_str()));
        for (const std::string& argument : arguments) argv.push_back(const_cast<char*>(argument.c_str()));
        argv.push_back(nullptr);
        execvp(program.c_str(), argv.data());
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) return true;
    if (error != nullptr) *error = program + " said no";
    return false;
#else
    (void)arguments;
    if (error != nullptr) *error = program + " cannot be run on this system";
    return false;
#endif
}

// Starts a program on its own, so it lives on after this process: forked
// twice, the middle one gone at once, so nothing is left to reap.
bool spawn_detached(const std::string& program, const std::vector<std::string>& arguments, std::string* error) {
#ifdef DUALSCREEN_CAN_SPAWN
    const pid_t pid = fork();
    if (pid < 0) {
        if (error != nullptr) *error = "could not start " + program;
        return false;
    }
    if (pid == 0) {
        setsid();
        if (fork() != 0) _exit(0);
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(program.c_str()));
        for (const std::string& argument : arguments) argv.push_back(const_cast<char*>(argument.c_str()));
        argv.push_back(nullptr);
        execvp(program.c_str(), argv.data());
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return true;
#else
    (void)arguments;
    if (error != nullptr) *error = program + " cannot be started on this system";
    return false;
#endif
}

bool on_path(const std::string& program) {
    const char* path = std::getenv("PATH");
    if (path == nullptr) return false;
    std::string folders = path;
    std::size_t start = 0;
    while (start <= folders.size()) {
        const std::size_t end = std::min(folders.find(':', start), folders.size());
        const std::string folder = folders.substr(start, end - start);
        std::error_code error;
        if (!folder.empty() && std::filesystem::exists(std::filesystem::path(folder) / program, error)) return true;
        start = end + 1;
    }
    return false;
}

}  // namespace

bool launchable_game_icon(const LaunchableGame& game, int& width, int& height, std::vector<uint8_t>& rgba) {
    SystemId system = SystemId::Ds;
    if (game.path.empty() || !system_for_rom(game.path, system)) return false;
    if (system == SystemId::Ds) {
        DsRomInfo info;
        return read_ds_rom_info(game.path, info, nullptr) && !info.icon.empty() &&
               save_icon_rgba(kSaveSystemDs, info.icon, width, height, rgba);
    }
    ConsoleTitleInfo info;
    const bool read = system == SystemId::ThreeDs ? read_3ds_title_info(game.path, info, nullptr)
                      : path_extension_lower(game.path) == "wua"
                          ? read_wii_u_archive_title_info(game.path, "", 0, info, nullptr)
                          : read_wii_u_title_info(game.path, info, nullptr);
    return read && !info.icon.empty() && decode_rgba_icon(info.icon, width, height, rgba);
}

std::string this_program() {
    // An AppImage runs from a mount that is gone once it stops; it says where
    // it really is.
    if (const char* image = std::getenv("APPIMAGE"); image != nullptr && *image != '\0') return image;
#ifdef DUALSCREEN_CAN_SPAWN
    std::error_code error;
    const std::filesystem::path self = std::filesystem::read_symlink("/proc/self/exe", error);
    if (!error) return self.string();
#endif
    return "dualscreen";
}

void note_running_game(const std::string& record_path, const std::string& rom_path, const std::string& title) {
#ifdef DUALSCREEN_CAN_SPAWN
    std::error_code error;
    std::filesystem::create_directories(std::filesystem::path(record_path).parent_path(), error);
    std::ofstream out(record_path, std::ios::trunc);
    out << nlohmann::json{{"path", rom_path}, {"title", title}, {"pid", static_cast<int>(getpid())}}.dump();
#else
    (void)record_path;
    (void)rom_path;
    (void)title;
#endif
}

void clear_running_game(const std::string& record_path) {
#ifdef DUALSCREEN_CAN_SPAWN
    RunningGame running;
    if (!running_game(record_path, running) || running.pid != static_cast<int>(getpid())) return;
    std::error_code error;
    std::filesystem::remove(record_path, error);
#else
    (void)record_path;
#endif
}

bool running_game(const std::string& record_path, RunningGame& out) {
#ifdef DUALSCREEN_CAN_SPAWN
    std::ifstream in(record_path);
    if (!in) return false;
    try {
        const nlohmann::json record = nlohmann::json::parse(in);
        out.path = record.value("path", std::string());
        out.title = record.value("title", std::string());
        out.pid = record.value("pid", 0);
    } catch (const nlohmann::json::exception&) {
        return false;
    }
    // A record left by one that ended without clearing it -- killed -- is no
    // game running.
    return out.pid > 0 && !out.path.empty() && kill(out.pid, 0) == 0;
#else
    (void)record_path;
    (void)out;
    return false;
#endif
}

bool close_running_game(const RunningGame& game, int timeout_ms) {
#ifdef DUALSCREEN_CAN_SPAWN
    if (game.pid <= 0) return false;
    if (kill(game.pid, SIGTERM) != 0) return false;
    log_info("closing " + game.title + " for a connected device");
    for (int waited = 0; waited < timeout_ms; waited += 100) {
        if (kill(game.pid, 0) != 0) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return kill(game.pid, 0) != 0;
#else
    (void)game;
    (void)timeout_ms;
    return false;
#endif
}

bool same_game_file(const std::string& a, const std::string& b) {
    if (a.empty() || b.empty()) return false;
    std::error_code error;
    const std::filesystem::path first = std::filesystem::weakly_canonical(a, error);
    if (error) return a == b;
    const std::filesystem::path second = std::filesystem::weakly_canonical(b, error);
    if (error) return a == b;
    return first == second;
}

bool launch_game_here(const LaunchableGame& game, std::string* error) {
    if (game.steam_run_id != 0) {
        // Handed to the Steam already running, which starts the shortcut as
        // if it had been chosen in Steam.
        const std::string url = "steam://rungameid/" + std::to_string(game.steam_run_id);
        if (on_path("steam")) return spawn_detached("steam", {url}, error);
        if (on_path("flatpak")) return spawn_detached("flatpak", {"run", "com.valvesoftware.Steam", url}, error);
        return spawn_detached("xdg-open", {url}, error);
    }
    return spawn_detached(this_program(), {"--from-remote-start", "--game", game.path}, error);
}

bool service_starts_at_login() {
    std::error_code error;
    return std::filesystem::exists(unit_path(), error) &&
           std::filesystem::exists(std::filesystem::path(unit_path()).parent_path() / "default.target.wants" / kUnitName,
                                   error);
}

namespace {

// The unit as this build writes it: run from where this program is now, and
// started again whenever it ends -- as it does, on purpose, once the program
// is updated under it (see run_launch_service()).
std::string unit_text() {
    std::ostringstream unit;
    unit << "[Unit]\n"
         << "Description=Dualscreen: start games from other devices\n"
         << "After=network-online.target\n\n"
         << "[Service]\n"
         << "ExecStart=\"" << this_program() << "\" --service\n"
         << "Restart=always\n"
         << "RestartSec=5\n\n"
         << "[Install]\n"
         << "WantedBy=default.target\n";
    return unit.str();
}

std::string read_whole(const std::string& path) {
    std::ifstream in(path);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

}  // namespace

bool refresh_service() {
    if (!service_starts_at_login()) return true;
    if (read_whole(unit_path()) == unit_text()) return true;
    // Written by an older build, or for this program somewhere else: put
    // right, and the service started again from here.
    {
        std::ofstream unit(unit_path(), std::ios::trunc);
        if (!unit) return false;
        unit << unit_text();
    }
    run_and_wait("systemctl", {"--user", "daemon-reload"}, nullptr);
    log_info("the Remote Start service was updated to this version");
    return run_and_wait("systemctl", {"--user", "restart", kUnitName}, nullptr);
}

bool set_service_starts_at_login(bool on, std::string* error) {
    if (!on) {
        run_and_wait("systemctl", {"--user", "disable", "--now", kUnitName}, nullptr);
        std::error_code ignored;
        std::filesystem::remove(unit_path(), ignored);
        run_and_wait("systemctl", {"--user", "daemon-reload"}, nullptr);
        log_info("other devices can no longer start games here");
        return true;
    }
    std::error_code fs_error;
    std::filesystem::create_directories(std::filesystem::path(unit_path()).parent_path(), fs_error);
    {
        std::ofstream unit(unit_path(), std::ios::trunc);
        if (!unit) {
            if (error != nullptr) *error = "could not write " + unit_path();
            return false;
        }
        unit << unit_text();
    }
    run_and_wait("systemctl", {"--user", "daemon-reload"}, nullptr);
    if (!run_and_wait("systemctl", {"--user", "enable", "--now", kUnitName}, error)) return false;
    log_info("other devices can start games here, from now on and after every login");
    return true;
}

int run_launch_service() {
    const AppPaths paths = AppPaths::resolve();
    set_log_file(paths.data_dir + "/dualscreen-service.log");
    std::signal(SIGTERM, on_stop_signal);
    std::signal(SIGINT, on_stop_signal);

    net::LaunchService::Config config;
    {
        const Settings settings = Settings::load(paths);
        config.name = settings.device_name;
    }
    config.kind = net::this_device_kind();
    config.record_path = paths.config_dir + "/remote_start.json";
    config.games = [paths] {
        // Read again each time: a folder the file picker moved to since counts.
        const Settings settings = Settings::load(paths);
        std::vector<LaunchableGame> games = launchable_games(home_dir(), rom_folders(settings));
        add_played(games, paths.data_dir + "/played_games.json");
        return games;
    };
    config.launch = [](const LaunchableGame& game, std::string* error) { return launch_game_here(game, error); };
    const std::string running_record = paths.data_dir + "/running_game.json";
    config.running = [running_record](const LaunchableGame& wanted, std::string& title, bool& same) {
        RunningGame running;
        if (!running_game(running_record, running)) return false;
        title = running.title;
        same = same_game_file(running.path, wanted.path);
        return true;
    };
    config.close_running = [running_record] {
        RunningGame running;
        if (!running_game(running_record, running)) return true;
        return close_running_game(running, 20000);
    };
    config.icon = [](const LaunchableGame& game, int& width, int& height, std::vector<uint8_t>& rgba) {
        return launchable_game_icon(game, width, height, rgba);
    };
    net::LaunchService service(config);
    std::string error;
    if (!service.start(&error)) {
        log_error("other devices cannot start games here: " + error);
        return 1;
    }
    // Updated under it -- a new AppImage over the old one -- this copy
    // leaves, and systemd starts the new one in its place: otherwise the
    // old build would answer until the next login.
    const std::string program = this_program();
    std::error_code time_error;
    const auto built = std::filesystem::last_write_time(program, time_error);
    const bool watching = !time_error;
    int waited = 0;
    while (!g_stop) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        if (!watching || (waited += 200) < 5000) continue;
        waited = 0;
        std::error_code error;
        const auto now = std::filesystem::last_write_time(program, error);
        if (!error && now != built) {
            log_info("Dualscreen was updated; the Remote Start service will restart");
            break;
        }
    }
    service.stop();
    return 0;
}

}  // namespace ds
