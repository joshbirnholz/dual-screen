#include "update/updater.hpp"

#include <cctype>
#include <cstdio>
#include <exception>

#include <nlohmann/json.hpp>

#include "config/paths.hpp"
#include "util/log.hpp"

namespace ds::update {

bool parse_release_info(const std::string& text, ReleaseInfo& out, std::string* error) {
    const nlohmann::json document = nlohmann::json::parse(text, nullptr, false);
    if (document.is_discarded() || !document.is_object()) {
        if (error != nullptr) *error = "the release's version.json is not JSON";
        return false;
    }
    const auto build = document.find("build");
    if (build == document.end() || !build->is_number_integer() || build->get<int64_t>() <= 0 ||
        build->get<int64_t>() > 1000000000) {
        if (error != nullptr) *error = "the release's version.json names no build";
        return false;
    }
    ReleaseInfo info;
    info.build = static_cast<int>(build->get<int64_t>());
    const auto commit = document.find("commit");
    if (commit != document.end() && commit->is_string()) info.commit = commit->get<std::string>();
    const auto built = document.find("built");
    if (built != document.end() && built->is_string()) info.built = built->get<std::string>();
    out = info;
    return true;
}

std::string release_url(const std::string& repository, const std::string& file) {
    return "https://github.com/" + repository + "/releases/download/nightly/" + file;
}

std::string buildbot_index_url(const std::string& folder) { return folder + ".index-extended"; }

bool parse_buildbot_index(const std::string& text, const std::vector<std::string>& candidates,
                          ReleaseInfo& out) {
    // Each line "<date> <hash> <file>"; the candidates in order of preference.
    for (const std::string& wanted : candidates) {
        std::size_t at = 0;
        while (at < text.size()) {
            std::size_t end = text.find('\n', at);
            if (end == std::string::npos) end = text.size();
            std::string line = text.substr(at, end - at);
            at = end + 1;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const std::size_t first = line.find(' ');
            const std::size_t second = first == std::string::npos ? first : line.find(' ', first + 1);
            if (second == std::string::npos || line.substr(second + 1) != wanted) continue;
            ReleaseInfo info;
            info.built = line.substr(0, first);
            info.version = line.substr(0, second);
            info.asset = wanted;
            out = info;
            return true;
        }
    }
    return false;
}

std::string download_url(const ComponentSpec& component, const ReleaseInfo& release) {
    if (component.source == Source::Buildbot) return component.repository + release.asset;
    return release_url(component.repository, component.asset);
}

std::string describe_release(const ComponentSpec& component, const ReleaseInfo& release) {
    if (component.source == Source::Buildbot) {
        return release.built.empty() ? std::string("a new build") : "built " + release.built;
    }
    return release.build > 0 ? "build " + std::to_string(release.build) : std::string("a local build");
}

std::string core_library_name(const std::string& asset) {
    const std::string zip = ".zip";
    if (asset.size() > zip.size() && asset.compare(asset.size() - zip.size(), zip.size(), zip) == 0) {
        return asset.substr(0, asset.size() - zip.size());
    }
    return asset;
}

std::string core_version_file(const std::string& cores_dir, const std::string& library) {
    return path_join(cores_dir, library + ".version");
}

ComponentSpec core_component(Component id, const std::string& cores_dir, bool in_use) {
    ComponentSpec core;
    core.id = id;
    core.source = Source::Buildbot;
    core.repository = kBuildbotLinux;
    core.automatic = in_use;
    if (id == Component::ThreeDsCore) {
        core.name = "Azahar";
        // Azahar's own first, as core_search_paths() prefers it: the one with
        // motion. Citra where the buildbot has no Azahar.
        core.candidates = {"azahar_libretro.so.zip", "citra_libretro.so.zip"};
    } else {
        core.name = "melonDS DS";
        core.candidates = {"melondsds_libretro.so.zip"};
    }
    core.asset = core.candidates.front();
    core.installed = kNotInstalled;
    for (const std::string& candidate : core.candidates) {
        const std::string library = core_library_name(candidate);
        if (!file_exists(path_join(cores_dir, library))) continue;
        core.asset = candidate;
        std::vector<uint8_t> bytes;
        core.installed_version = read_file(core_version_file(cores_dir, library), bytes, nullptr)
                                     ? std::string(bytes.begin(), bytes.end())
                                     : std::string("unknown");
        while (!core.installed_version.empty() &&
               (core.installed_version.back() == '\n' || core.installed_version.back() == '\r')) {
            core.installed_version.pop_back();
        }
        core.installed = kLocalBuild;
        if (candidate.rfind("citra", 0) == 0) core.name = "Citra";
        break;
    }
    return core;
}

bool worth_offering(const ComponentSpec& component, const ReleaseInfo& release, bool asked) {
    if (!component.automatic && !asked) return false;
    if (component.source == Source::Buildbot) {
        // Told apart by date and hash: any other build than the one here is
        // the newer, the buildbot keeping only its latest.
        if (release.version.empty()) return false;
        if (component.installed_version.empty()) return asked;
        return release.version != component.installed_version;
    }
    if (release.build <= 0) return false;
    if (component.installed > 0) return release.build > component.installed;
    return asked;
}

std::vector<ComponentSpec> components_for(const std::string& platform_asset, int dualscreen_build,
                                          bool keeps_cemu, int cemu_installed, bool cemu_in_use) {
    std::vector<ComponentSpec> components;
    ComponentSpec dualscreen;
    dualscreen.id = Component::Dualscreen;
    dualscreen.name = "Dualscreen";
    dualscreen.repository = kDualscreenRepository;
    dualscreen.asset = platform_asset;
    dualscreen.installed = dualscreen_build;
    components.push_back(dualscreen);
    if (keeps_cemu) {
        ComponentSpec cemu;
        cemu.id = Component::Cemu;
        cemu.name = "Cemu";
        cemu.repository = kCemuRepository;
        cemu.asset = kCemuAsset;
        cemu.installed = cemu_installed;
        cemu.automatic = cemu_in_use;
        components.push_back(cemu);
    }
    return components;
}

std::string managed_cemu_path(const std::string& data_dir) {
    return path_join(path_join(data_dir, "cemu"), kCemuAsset);
}

std::string managed_cemu_build_file(const std::string& data_dir) {
    return path_join(path_join(data_dir, "cemu"), "build.txt");
}

int managed_cemu_build(const std::string& data_dir) {
    if (!file_exists(managed_cemu_path(data_dir))) return kNotInstalled;
    std::vector<uint8_t> bytes;
    if (!read_file(managed_cemu_build_file(data_dir), bytes, nullptr)) return kLocalBuild;
    int build = 0;
    for (const uint8_t byte : bytes) {
        if (!std::isdigit(byte)) break;
        if (build > 100000000) return kLocalBuild;
        build = build * 10 + (byte - '0');
    }
    return build;
}

Updater::Updater(std::unique_ptr<Backend> backend, std::vector<ComponentSpec> components)
    : backend_(std::move(backend)), components_(std::move(components)) {}

Updater::~Updater() {
    cancel();
    wait();
}

void Updater::wait() {
    if (worker_.joinable()) worker_.join();
}

bool Updater::busy() const { return working_.load(); }

std::vector<ComponentSpec> Updater::components() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return components_;
}

Status Updater::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

void Updater::set(const std::function<void(Status&)>& change) {
    std::lock_guard<std::mutex> lock(mutex_);
    change(status_);
}

void Updater::cancel() { cancel_ = true; }

void Updater::start(std::function<void()> work) {
    // The last one has finished -- busy() said so -- but its thread is still
    // to be joined before another can take its place.
    wait();
    working_ = true;
    cancel_ = false;
    worker_ = std::thread([this, work = std::move(work)] {
        try {
            work();
        } catch (const std::exception& error) {
            set([&](Status& status) {
                status.phase = Phase::Failed;
                status.error = error.what();
            });
        }
        working_ = false;
    });
}

void Updater::set_automatic(Component id, bool automatic) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (ComponentSpec& component : components_) {
        if (component.id == id) component.automatic = automatic;
    }
}

void Updater::check(bool asked, std::optional<Component> only) {
    if (busy() || !backend_) return;
    set([&](Status& status) {
        status.phase = Phase::Checking;
        status.asked = asked;
        status.error.clear();
        ++status.round;
    });
    start([this, asked, only] { run_check(asked, only); });
}

void Updater::run_check(bool asked, std::optional<Component> only) {
    std::vector<Offer> offers;
    int newest = 0;
    // The buildbot's index, fetched once for every core that wants it.
    std::string index;
    bool index_fetched = false;
    std::string index_error;
    for (ComponentSpec component : components()) {
        if (only && component.id != *only) continue;
        // Asked for, every one Dualscreen keeps is looked at -- in use or
        // not; at start-up, only the ones in use.
        if (!only && !component.automatic && !(asked && component.installed != kNotInstalled)) continue;
        std::string body;
        std::string error;
        ReleaseInfo release;
        bool seen = false;
        if (component.source == Source::Buildbot) {
            if (!index_fetched) {
                index_fetched = true;
                if (!backend_->fetch(buildbot_index_url(component.repository), index, cancel_, index_error)) {
                    index.clear();
                }
            }
            error = index_error;
            if (!index.empty()) {
                seen = parse_buildbot_index(index, component.candidates, release);
                if (!seen) error = "the buildbot has no " + component.name + " for this device";
            }
            if (seen) {
                // Whichever of its files the buildbot has is the one it is.
                component.asset = release.asset;
                if (release.asset.rfind("citra", 0) == 0) component.name = "Citra";
            }
        } else {
            seen = backend_->fetch(release_url(component.repository, "version.json"), body, cancel_, error) &&
                   parse_release_info(body, release, &error);
        }
        if (!seen) {
            log_info("updates: could not see " + component.name + "'s newest build: " + error);
            // Dualscreen's own is the one that matters -- or the one asked
            // about by name; a Cemu that cannot be checked beside Dualscreen
            // is only not offered.
            if (component.id == Component::Dualscreen || only) {
                set([&](Status& status) {
                    status.phase = Phase::Failed;
                    status.error = error;
                    status.offers.clear();
                });
                return;
            }
            continue;
        }
        if (component.id == Component::Dualscreen) newest = release.build;
        const bool offered = worth_offering(component, release, asked);
        if (component.source == Source::Buildbot) {
            log_info("updates: the buildbot's " + component.name + " is " + release.version +
                     (component.installed_version.empty() ? ", none here"
                                                          : ", the one here " + component.installed_version) +
                     (offered ? "; offering it" : ""));
        } else {
            log_info("updates: " + component.name + "'s newest is build " + std::to_string(release.build) +
                     (component.installed > 0 ? ", this is build " + std::to_string(component.installed)
                      : component.installed == kNotInstalled ? ", none here" : ", this is a local build") +
                     (offered ? "; offering it" : ""));
        }
        if (offered) offers.push_back(Offer{component, release});
    }
    set([&](Status& status) {
        status.phase = Phase::Checked;
        status.offers = offers;
        status.newest_build = newest;
    });
}

void Updater::install() {
    if (busy() || !backend_) return;
    std::vector<Offer> offers;
    set([&](Status& status) {
        offers = status.offers;
        if (offers.empty()) return;
        status.phase = Phase::Downloading;
        status.progress = -1.0f;
        status.error.clear();
        status.restart = false;
        status.handed_to_system = false;
        ++status.round;
    });
    if (offers.empty()) return;
    start([this, offers] { run_install(offers); });
}

void Updater::run_install(std::vector<Offer> offers) {
    const std::string staging = backend_->staging_dir();
    if (staging.empty() || !ensure_directory(staging)) {
        set([&](Status& status) {
            status.phase = Phase::Failed;
            status.error = "there is nowhere to download it to (" + staging + ")";
        });
        return;
    }
    // Every download first, then every install: a download that fails or is
    // cancelled leaves what is installed exactly as it was.
    std::vector<std::string> files;
    for (const Offer& offer : offers) {
        set([&](Status& status) {
            status.phase = Phase::Downloading;
            status.current = offer.component.name;
            status.progress = -1.0f;
        });
        const std::string file = path_join(staging, offer.component.asset);
        std::remove(file.c_str());
        std::string error;
        const std::string url = download_url(offer.component, offer.release);
        log_info("updates: downloading " + offer.component.name + " " +
                 describe_release(offer.component, offer.release) + " from " + url);
        const bool downloaded = backend_->download(
            url, file, cancel_, [this](int64_t done, int64_t total) {
                set([&](Status& status) {
                    status.progress = total > 0 ? static_cast<float>(static_cast<double>(done) / total) : -1.0f;
                });
            },
            error);
        if (!downloaded || cancel_) {
            std::remove(file.c_str());
            for (const std::string& earlier : files) std::remove(earlier.c_str());
            const bool cancelled = cancel_.load();
            log_info(cancelled ? "updates: the download was cancelled"
                               : "updates: " + offer.component.name + " did not download: " + error);
            set([&](Status& status) {
                if (cancelled) {
                    // Back to what the check found, to be offered again.
                    status.phase = Phase::Checked;
                } else {
                    status.phase = Phase::Failed;
                    status.error = error;
                }
            });
            return;
        }
        files.push_back(file);
    }
    bool restart = false;
    bool handed = false;
    for (std::size_t index = 0; index < offers.size(); ++index) {
        const Offer& offer = offers[index];
        set([&](Status& status) {
            status.phase = Phase::Installing;
            status.current = offer.component.name;
            status.progress = -1.0f;
        });
        std::string error;
        if (!backend_->install(offer.component, offer.release, files[index], error)) {
            log_warn("updates: " + offer.component.name + " was not installed: " + error);
            for (std::size_t rest = index; rest < files.size(); ++rest) std::remove(files[rest].c_str());
            set([&](Status& status) {
                status.phase = Phase::Failed;
                status.error = error;
            });
            return;
        }
        log_info("updates: " + offer.component.name + " " + describe_release(offer.component, offer.release) +
                 " is installed");
        if (offer.component.id == Component::Dualscreen) {
            if (backend_->restarts_after_install()) {
                restart = true;
            } else {
                handed = true;
            }
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (ComponentSpec& component : components_) {
                if (component.id != offer.component.id) continue;
                component.installed = offer.component.source == Source::Buildbot ? kLocalBuild : offer.release.build;
                component.installed_version = offer.release.version;
                component.asset = offer.component.asset;
                component.name = offer.component.name;
            }
        }
    }
    set([&](Status& status) {
        status.phase = Phase::Installed;
        status.restart = restart;
        status.handed_to_system = handed;
        status.offers.clear();
    });
}

}  // namespace ds::update
