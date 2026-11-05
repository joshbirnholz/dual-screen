#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "update/updater.hpp"

namespace ds::update {

// Cemu's community graphic packs -- resolution packs, 60 FPS packs, fixes --
// downloaded and kept up to date from Settings, as Cemu's own "Download
// community graphic packs" does, so that nobody has to open Cemu's window for
// them.
//
// They are cemu-project/cemu_graphic_packs's latest GitHub release: one zip,
// unpacked into graphicPacks/downloadedGraphicPacks in Cemu's user-data
// folder, beside a version.txt naming the release ("Cemu Graphic Packs:
// v981"). That is exactly where and how Cemu puts them, so Cemu's own window
// sees them as the ones it downloaded, and its check agrees on which they are.

inline constexpr const char* kGraphicPacksLatestUrl =
    "https://api.github.com/repos/cemu-project/cemu_graphic_packs/releases/latest";

// Where Cemu keeps its user data, graphicPacks included, as Cemu itself
// decides at start-up: a "portable" folder beside `cemu_program`; on Windows,
// the program's own folder when a settings.xml is there (as Cemu before 2.0
// kept it); else the platform's usual place -- `xdg_data_home` or
// ~/.local/share on Linux, `appdata` (%APPDATA%) on Windows, Application
// Support on a Mac. `cemu_program` may be empty: nothing set yet.
std::string cemu_user_data_dir(const std::string& cemu_program, const std::string& home,
                               const std::string& xdg_data_home, const std::string& appdata);
// The same, from this process's own environment.
std::string cemu_user_data_dir(const std::string& cemu_program);

// <user data>/graphicPacks/downloadedGraphicPacks.
std::string downloaded_graphic_packs_dir(const std::string& user_data);

struct GraphicPacksRelease {
    // As GitHub names it, and as version.txt keeps it: "Cemu Graphic Packs: v981".
    std::string name;
    std::string url;
};

// Reads the GitHub API's answer for the latest release: its name and its
// first file's download link, as Cemu does. False, with why, otherwise.
bool parse_graphic_packs_release(const std::string& json, GraphicPacksRelease& out, std::string* error);

// The release the downloaded packs are, from version.txt: empty when none
// have been downloaded.
std::string installed_graphic_packs(const std::string& user_data);

// What a release is called to a player: "v981", from "Cemu Graphic Packs:
// v981" -- or the whole name, when it is not shaped like that.
std::string graphic_packs_version(const std::string& name);

// Whether `latest` is other than what is here -- compared as Cemu compares
// them, ignoring case.
bool graphic_packs_differ(const std::string& installed, const std::string& latest);

// Puts the packs in `zip` in place of the ones downloaded before: unpacked
// beside the folder first, so a zip that is damaged, or a disk that fills,
// leaves the old ones as they were; then swapped in, and version.txt written.
bool install_graphic_packs(const std::string& zip, const std::string& user_data, const std::string& name,
                           std::string& error);

// Checks and downloads on a thread of its own, as Updater does, through the
// same kind of Backend: fetch() for the release, download() for its zip.
class GraphicPacksDownloader {
public:
    enum class Phase { Idle, Checking, Checked, Downloading, Installing, Installed, Failed };
    struct Status {
        Phase phase = Phase::Idle;
        // Which are here (empty for none), and, once checked, the newest.
        std::string installed;
        GraphicPacksRelease latest;
        // While Downloading: 0 to 1, or less than 0 while the size is not known.
        float progress = -1.0f;
        std::string error;
        // Which check or install this is, counting from 1.
        int round = 0;
        bool newer() const { return !latest.name.empty() && graphic_packs_differ(installed, latest.name); }
    };

    // `user_data` is Cemu's -- see cemu_user_data_dir().
    GraphicPacksDownloader(std::unique_ptr<Backend> backend, std::string user_data);
    ~GraphicPacksDownloader();
    GraphicPacksDownloader(const GraphicPacksDownloader&) = delete;
    GraphicPacksDownloader& operator=(const GraphicPacksDownloader&) = delete;

    // Another Cemu named in settings, with its own user data. Nothing while busy.
    void set_user_data(const std::string& user_data);
    const std::string& user_data() const { return user_data_; }

    void check();
    // Downloads and installs what the last check found.
    void install();
    void cancel() { cancel_ = true; }

    Status status() const;
    bool busy() const { return working_.load(); }
    // For tests: waits for the worker to finish.
    void wait();

private:
    void start(std::function<void()> work);
    void set(const std::function<void(Status&)>& change);
    void run_check();
    void run_install(GraphicPacksRelease release);

    std::unique_ptr<Backend> backend_;
    std::string user_data_;
    mutable std::mutex mutex_;
    Status status_;
    std::thread worker_;
    std::atomic<bool> working_{false};
    std::atomic<bool> cancel_{false};
};

}  // namespace ds::update
