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

bool worth_offering(const ComponentSpec& component, const ReleaseInfo& release, bool asked) {
    if (release.build <= 0) return false;
    if (component.installed > 0) return release.build > component.installed;
    return asked;
}

std::vector<ComponentSpec> components_for(const std::string& platform_asset, int dualscreen_build,
                                          bool keeps_cemu, int cemu_installed) {
    std::vector<ComponentSpec> components;
    components.push_back(
        ComponentSpec{Component::Dualscreen, "Dualscreen", kDualscreenRepository, platform_asset, dualscreen_build});
    if (keeps_cemu) {
        components.push_back(ComponentSpec{Component::Cemu, "Cemu", kCemuRepository, kCemuAsset, cemu_installed});
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

void Updater::check(bool asked) {
    if (busy() || !backend_) return;
    set([&](Status& status) {
        status.phase = Phase::Checking;
        status.asked = asked;
        status.error.clear();
        ++status.round;
    });
    start([this, asked] { run_check(asked); });
}

void Updater::run_check(bool asked) {
    std::vector<Offer> offers;
    int newest = 0;
    for (const ComponentSpec& component : components()) {
        std::string body;
        std::string error;
        ReleaseInfo release;
        const std::string url = release_url(component.repository, "version.json");
        if (!backend_->fetch(url, body, cancel_, error) || !parse_release_info(body, release, &error)) {
            log_info("updates: could not see " + component.name + "'s newest build: " + error);
            // Dualscreen's own is the one that matters; a Cemu that cannot be
            // checked is only not offered.
            if (component.id == Component::Dualscreen) {
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
        log_info("updates: " + component.name + "'s newest is build " + std::to_string(release.build) +
                 (component.installed > 0 ? ", this is build " + std::to_string(component.installed)
                  : component.installed == kNotInstalled ? ", none here" : ", this is a local build") +
                 (offered ? "; offering it" : ""));
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
        const std::string url = release_url(offer.component.repository, offer.component.asset);
        log_info("updates: downloading " + offer.component.name + " build " + std::to_string(offer.release.build) +
                 " from " + url);
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
        log_info("updates: " + offer.component.name + " build " + std::to_string(offer.release.build) +
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
                if (component.id == offer.component.id) component.installed = offer.release.build;
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
