#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace ds::update {

// Keeping Dualscreen up to date from the nightly builds CI publishes.
//
// Every push builds every platform and replaces one rolling pre-release,
// "nightly", on the repository's Releases page (.github/workflows/builds.yml).
// Beside its files is version.json, which says which build it is: the number
// of the CI run that made it. A build from CI carries the same number in
// itself (build_number(), util/build_stamp.hpp), so whether there is a newer
// one is one small download and a comparison.
//
// Cemu, which plays Wii U games beside this app on a PC or a Steam Deck, is
// published the same way from the fork's repository, and kept up to date the
// same way when Dualscreen is the one keeping it -- see cemu_component().
//
// Nothing is installed without being asked: a newer build is offered, and
// the player says yes. What installing means is the platform's -- see Backend.

// What a nightly release says about itself.
struct ReleaseInfo {
    int build = 0;
    std::string commit;
    // When it was built, as CI wrote it: an ISO 8601 time in UTC. For a
    // libretro core, the buildbot's date.
    std::string built;
    // A libretro core's: which build it is, as the buildbot's index says --
    // its date and hash -- and which of the component's files it is.
    std::string version;
    std::string asset;
};

// Reads version.json. False, with why, for anything that is not one -- a
// page of HTML from a captive portal, say -- or that names no build.
bool parse_release_info(const std::string& text, ReleaseInfo& out, std::string* error);

// Where a file of a repository's nightly release is downloaded from.
std::string release_url(const std::string& repository, const std::string& file);

enum class Component { Dualscreen, Cemu, DsCore, ThreeDsCore };

// Where a component's builds come from: a GitHub repository's rolling
// "nightly" release, numbered by CI, or libretro's buildbot -- the same place
// RetroArch's own core downloader looks -- whose builds are told apart by
// their date and hash.
enum class Source { GitHubNightly, Buildbot };

// The buildbot's folder of x86-64 Linux cores, and its index of them: a line
// per core, "<date> <hash> <file>".
inline constexpr const char* kBuildbotLinux = "https://buildbot.libretro.com/nightly/linux/x86_64/latest/";
std::string buildbot_index_url(const std::string& folder);
// Reads that index: for the first of `candidates` it lists, its file, and its
// date and hash as the version. False when it lists none of them.
bool parse_buildbot_index(const std::string& text, const std::vector<std::string>& candidates,
                          ReleaseInfo& out);

// The installed builds of a component, as far as this device knows them.
inline constexpr int kNotInstalled = -1;
// A build made somewhere other than CI, which knows no number of its own.
inline constexpr int kLocalBuild = 0;

struct ComponentSpec {
    Component id = Component::Dualscreen;
    // What the player is told it is: "Dualscreen", "Cemu", "melonDS DS".
    std::string name;
    // Whose nightly release it comes from: "owner/repository". For a core,
    // the buildbot's folder.
    std::string repository;
    // Its file in that release.
    std::string asset;
    // The build here: a CI run's number, kLocalBuild or kNotInstalled.
    int installed = kLocalBuild;
    // Whether a check made at start-up looks at it at all. Cemu and the
    // cores only while they are the ones games are played with. Settings'
    // Check for Updates looks at every one installed too, and a missing one
    // is downloaded when Settings asks for it by name.
    bool automatic = true;
    Source source = Source::GitHubNightly;
    // A core's: the files it may be on the buildbot, the one wanted first
    // first -- Azahar, then Citra -- and the version installed, empty when
    // none is (installed is then kNotInstalled).
    std::vector<std::string> candidates;
    std::string installed_version;
};

// Where a component's release file is downloaded from.
std::string download_url(const ComponentSpec& component, const ReleaseInfo& release);
// What a release is called to the player: "build 15", or a core's date.
std::string describe_release(const ComponentSpec& component, const ReleaseInfo& release);

// Whether `release` is worth offering in place of what `component` has. A
// build from CI is offered anything newer. A local build, or nothing at all,
// is offered the nightly only when the player asked -- a build made by hand
// is somebody working on this, and an app they did not ask for is not one to
// put on their screen at every start.
// `asked` is whether the player asked: Settings' Check for Updates, or its
// row for downloading an emulator not here yet.
bool worth_offering(const ComponentSpec& component, const ReleaseInfo& release, bool asked);

// Where the work of fetching and installing is done, which is all a platform's
// own: a Steam Deck has curl and tar and a folder it can write to, a Switch
// has libcurl and its homebrew loader, Android has its package installer.
class Backend {
public:
    virtual ~Backend() = default;

    // A small file, whole -- version.json. Follows redirects, as every
    // download from a GitHub release is one. Gives up, as download() does,
    // once `cancel` is set: the app may be closing.
    virtual bool fetch(const std::string& url, std::string& body, const std::atomic<bool>& cancel,
                       std::string& error) = 0;

    // A release's file, into `path`. `progress` is told how far it has got,
    // with a total of 0 while that is not known. Stops, false, as soon as it
    // can once `cancel` is set.
    virtual bool download(const std::string& url, const std::string& path, const std::atomic<bool>& cancel,
                          const std::function<void(int64_t done, int64_t total)>& progress,
                          std::string& error) = 0;

    // Where downloads go until they are installed.
    virtual std::string staging_dir() = 0;

    // Puts a downloaded component in place of the one that is there.
    virtual bool install(const ComponentSpec& component, const ReleaseInfo& release, const std::string& file,
                         std::string& error) = 0;

    // Whether installing Dualscreen ends with this app starting again (true),
    // or with the system's installer taking over (false: Android's, which
    // replaces the app itself and offers to open it after).
    virtual bool restarts_after_install() const { return true; }
};

// One component with a newer build to offer.
struct Offer {
    ComponentSpec component;
    ReleaseInfo release;
};

enum class Phase {
    // Nothing asked yet.
    Idle,
    Checking,
    // Checked: `offers` is what there is, and may be nothing.
    Checked,
    Downloading,
    Installing,
    // Installed: what the shell does next is `restart`.
    Installed,
    // `error` says why. From a check or from an install.
    Failed,
};

struct Status {
    Phase phase = Phase::Idle;
    // Whether the last check was asked for in Settings rather than made at
    // start-up -- see worth_offering().
    bool asked = false;
    std::vector<Offer> offers;
    // The newest Dualscreen build the last check saw, 0 when none has.
    int newest_build = 0;
    std::string error;
    // While Downloading or Installing: whose, and how far, 0 to 1, or less
    // than 0 when the size is not known.
    std::string current;
    float progress = -1.0f;
    // Once Installed: whether Dualscreen was among what was installed and the
    // app has to start again for it to take effect.
    bool restart = false;
    // Whether the system's installer was handed the app instead.
    bool handed_to_system = false;
    // Which check or install this is, counting from 1, so a shell can tell
    // one that has just finished from one it has already answered.
    int round = 0;
};

// Checks and installs on a thread of its own, so a slow network never holds
// up a frame. Everything here is called from the shell's thread.
class Updater {
public:
    Updater(std::unique_ptr<Backend> backend, std::vector<ComponentSpec> components);
    ~Updater();

    Updater(const Updater&) = delete;
    Updater& operator=(const Updater&) = delete;

    // Looks for newer builds -- of `only`, when given, and of everything
    // automatic otherwise. Nothing, while a check or an install is going.
    void check(bool asked, std::optional<Component> only = std::nullopt);
    // Whether `id` is looked at without being asked for by name -- see
    // ComponentSpec::automatic. Cemu becomes so once it is the Wii U program.
    void set_automatic(Component id, bool automatic);
    // Downloads and installs what the last check offered.
    void install();
    // Stops a download as soon as it can.
    void cancel();

    Status status() const;
    // What each component has installed, as of the last install.
    std::vector<ComponentSpec> components() const;
    bool busy() const;
    // See Backend::restarts_after_install().
    bool restarts_after_install() const { return backend_ && backend_->restarts_after_install(); }

    // For tests: waits for the worker to finish.
    void wait();

private:
    void run_check(bool asked, std::optional<Component> only);
    void run_install(std::vector<Offer> offers);
    void start(std::function<void()> work);
    void set(const std::function<void(Status&)>& change);

    std::unique_ptr<Backend> backend_;
    std::vector<ComponentSpec> components_;
    mutable std::mutex mutex_;
    Status status_;
    std::thread worker_;
    std::atomic<bool> working_{false};
    std::atomic<bool> cancel_{false};
};

// The components a device keeps up to date, with what it has of each.
//
// `platform_asset` is this platform's file in Dualscreen's release, and the
// Cemu entry is there only when `cemu_installed` is given -- a PC or a Steam
// Deck whose Wii U program is the one Dualscreen keeps (see
// managed_cemu_path()).
std::vector<ComponentSpec> components_for(const std::string& platform_asset, int dualscreen_build,
                                          bool keeps_cemu, int cemu_installed, bool cemu_in_use = true);

// Where the repositories are.
inline constexpr const char* kDualscreenRepository = "joshbirnholz/dual-screen";
inline constexpr const char* kCemuRepository = "joshbirnholz/cemu-dualscreen";
inline constexpr const char* kCemuAsset = "Cemu-dualscreen-x86_64.AppImage";

// Where Dualscreen keeps a Cemu it downloaded, under the app's data folder
// (`data_dir`, AppPaths::data_dir), and the file beside it that says which
// build it is.
std::string managed_cemu_path(const std::string& data_dir);
std::string managed_cemu_build_file(const std::string& data_dir);
// The build of the Cemu there, or kNotInstalled.
int managed_cemu_build(const std::string& data_dir);

// The DS and 3DS cores, from the buildbot, kept in `cores_dir` -- this app's
// own cores folder, the first place core_search_paths() looks -- beside a
// "<core>.version" file saying which build each is. `in_use` is whether the
// one there is the one games are played with, which makes it automatic.
ComponentSpec core_component(Component id, const std::string& cores_dir, bool in_use);
// The file a core's zip holds: "melondsds_libretro.so.zip" holds
// "melondsds_libretro.so".
std::string core_library_name(const std::string& asset);
std::string core_version_file(const std::string& cores_dir, const std::string& library);

}  // namespace ds::update
