#include "update/graphic_packs.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <system_error>
#include <vector>

#include <nlohmann/json.hpp>

#include "config/paths.hpp"
#include "update/zip_extract.hpp"
#include "util/log.hpp"

namespace ds::update {
namespace {

namespace fs = std::filesystem;

std::string env(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr ? std::string(value) : std::string();
}

std::string lower(std::string text) {
    for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

std::string trimmed(const std::string& text) {
    const std::size_t start = text.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return std::string();
    const std::size_t end = text.find_last_not_of(" \t\r\n");
    return text.substr(start, end - start + 1);
}

bool remove_tree(const std::string& path) {
    std::error_code code;
    fs::remove_all(fs::u8path(path), code);
    return !code;
}

bool move_tree(const std::string& from, const std::string& to, std::string& error) {
    std::error_code code;
    fs::rename(fs::u8path(from), fs::u8path(to), code);
    if (code) error = "could not move " + from + " to " + to + ": " + code.message();
    return !code;
}

}  // namespace

std::string cemu_user_data_dir(const std::string& cemu_program, const std::string& home,
                               const std::string& xdg_data_home, const std::string& appdata) {
    const bool named = !cemu_program.empty() && cemu_program.find_first_of("/\\") != std::string::npos;
    if (named) {
        const std::string beside = path_dirname(cemu_program);
        const std::string portable = path_join(beside, "portable");
        if (directory_exists(portable)) return portable;
#if defined(_WIN32)
        if (file_exists(path_join(beside, "settings.xml"))) return beside;
#endif
    }
#if defined(_WIN32)
    if (!appdata.empty()) return path_join(appdata, "Cemu");
    return home.empty() ? std::string() : path_join(home, "AppData/Roaming/Cemu");
#elif defined(__APPLE__)
    (void)xdg_data_home;
    (void)appdata;
    return home.empty() ? std::string() : path_join(home, "Library/Application Support/Cemu");
#else
    (void)appdata;
    if (!xdg_data_home.empty()) return path_join(xdg_data_home, "Cemu");
    return home.empty() ? std::string() : path_join(home, ".local/share/Cemu");
#endif
}

std::string cemu_user_data_dir(const std::string& cemu_program) {
    return cemu_user_data_dir(cemu_program, user_home_dir(), env("XDG_DATA_HOME"), env("APPDATA"));
}

std::string downloaded_graphic_packs_dir(const std::string& user_data) {
    return path_join(path_join(user_data, "graphicPacks"), "downloadedGraphicPacks");
}

bool parse_graphic_packs_release(const std::string& json, GraphicPacksRelease& out, std::string* error) {
    const auto fail = [&](const char* why) {
        if (error != nullptr) *error = why;
        return false;
    };
    const nlohmann::json document = nlohmann::json::parse(json, nullptr, false);
    if (document.is_discarded() || !document.is_object()) return fail("GitHub's answer was not JSON");
    const auto name = document.find("name");
    if (name == document.end() || !name->is_string() || name->get<std::string>().empty()) {
        // GitHub's own refusal -- a rate limit, say -- says why in "message".
        const auto message = document.find("message");
        if (message != document.end() && message->is_string()) {
            if (error != nullptr) *error = "GitHub said: " + message->get<std::string>();
            return false;
        }
        return fail("the release has no name");
    }
    const auto assets = document.find("assets");
    if (assets == document.end() || !assets->is_array() || assets->empty() || !(*assets)[0].is_object()) {
        return fail("the release has no files");
    }
    const auto url = (*assets)[0].find("browser_download_url");
    if (url == (*assets)[0].end() || !url->is_string() || url->get<std::string>().empty()) {
        return fail("the release's file has no link");
    }
    out.name = name->get<std::string>();
    out.url = url->get<std::string>();
    return true;
}

std::string installed_graphic_packs(const std::string& user_data) {
    if (user_data.empty()) return std::string();
    std::vector<uint8_t> bytes;
    if (!read_file(path_join(downloaded_graphic_packs_dir(user_data), "version.txt"), bytes, nullptr)) {
        return std::string();
    }
    std::string text(bytes.begin(), bytes.end());
    // Its first line, as Cemu reads it.
    const std::size_t newline = text.find_first_of("\r\n");
    if (newline != std::string::npos) text.resize(newline);
    return trimmed(text);
}

std::string graphic_packs_version(const std::string& name) {
    const std::size_t colon = name.rfind(':');
    const std::string version = colon == std::string::npos ? trimmed(name) : trimmed(name.substr(colon + 1));
    return version.empty() ? trimmed(name) : version;
}

bool graphic_packs_differ(const std::string& installed, const std::string& latest) {
    return lower(trimmed(installed)) != lower(trimmed(latest));
}

bool install_graphic_packs(const std::string& zip, const std::string& user_data, const std::string& name,
                           std::string& error) {
    if (user_data.empty()) {
        error = "Cemu's user folder could not be found";
        return false;
    }
    const std::string packs = downloaded_graphic_packs_dir(user_data);
    // Outside graphicPacks, which Cemu searches for packs, so that a copy
    // half unpacked or left behind is never one it lists.
    const std::string unpacked = path_join(user_data, "downloadedGraphicPacks.new");
    const std::string old = path_join(user_data, "downloadedGraphicPacks.old");
    remove_tree(unpacked);
    remove_tree(old);
    std::string why;
    if (!extract_zip_all(zip, unpacked, "", nullptr, &why)) {
        remove_tree(unpacked);
        error = why.empty() ? std::string("the graphic packs would not unpack") : why;
        return false;
    }
    if (!write_file_atomically(path_join(unpacked, "version.txt"), name.data(), name.size(), &why)) {
        remove_tree(unpacked);
        error = why;
        return false;
    }
    if (!ensure_directory(path_join(user_data, "graphicPacks"))) {
        remove_tree(unpacked);
        error = "could not make " + path_join(user_data, "graphicPacks");
        return false;
    }
    // The old ones aside, the new ones in, the old ones gone -- or, if the
    // new ones would not go in, the old ones back.
    const bool had = directory_exists(packs);
    if (had && !move_tree(packs, old, error)) {
        remove_tree(unpacked);
        return false;
    }
    if (!move_tree(unpacked, packs, error)) {
        std::string ignored;
        if (had) move_tree(old, packs, ignored);
        remove_tree(unpacked);
        return false;
    }
    remove_tree(old);
    return true;
}

GraphicPacksDownloader::GraphicPacksDownloader(std::unique_ptr<Backend> backend, std::string user_data)
    : backend_(std::move(backend)), user_data_(std::move(user_data)) {
    status_.installed = installed_graphic_packs(user_data_);
}

GraphicPacksDownloader::~GraphicPacksDownloader() {
    cancel();
    wait();
}

void GraphicPacksDownloader::wait() {
    if (worker_.joinable()) worker_.join();
}

void GraphicPacksDownloader::set_user_data(const std::string& user_data) {
    if (busy() || user_data == user_data_) return;
    user_data_ = user_data;
    set([&](Status& status) {
        status.installed = installed_graphic_packs(user_data_);
        status.latest = GraphicPacksRelease();
        status.phase = Phase::Idle;
    });
}

GraphicPacksDownloader::Status GraphicPacksDownloader::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

void GraphicPacksDownloader::set(const std::function<void(Status&)>& change) {
    std::lock_guard<std::mutex> lock(mutex_);
    change(status_);
}

void GraphicPacksDownloader::start(std::function<void()> work) {
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

void GraphicPacksDownloader::check() {
    if (busy() || !backend_) return;
    set([&](Status& status) {
        status.phase = Phase::Checking;
        status.error.clear();
        // Looked at again: one downloaded in Cemu's own window counts too.
        status.installed = installed_graphic_packs(user_data_);
        ++status.round;
    });
    start([this] { run_check(); });
}

void GraphicPacksDownloader::run_check() {
    std::string body;
    std::string error;
    GraphicPacksRelease release;
    const bool found = backend_->fetch(kGraphicPacksLatestUrl, body, cancel_, error) &&
                       parse_graphic_packs_release(body, release, &error);
    if (!found) log_info("graphic packs: could not find the latest: " + error);
    set([&](Status& status) {
        if (found) {
            status.phase = Phase::Checked;
            status.latest = release;
        } else {
            status.phase = Phase::Failed;
            status.error = error;
        }
    });
}

void GraphicPacksDownloader::install() {
    if (busy() || !backend_) return;
    GraphicPacksRelease release;
    set([&](Status& status) {
        release = status.latest;
        if (release.url.empty()) return;
        status.phase = Phase::Downloading;
        status.progress = -1.0f;
        status.error.clear();
        ++status.round;
    });
    if (release.url.empty()) return;
    start([this, release] { run_install(release); });
}

void GraphicPacksDownloader::run_install(GraphicPacksRelease release) {
    const std::string staging = backend_->staging_dir();
    if (staging.empty() || !ensure_directory(staging)) {
        set([&](Status& status) {
            status.phase = Phase::Failed;
            status.error = "there is nowhere to download them to (" + staging + ")";
        });
        return;
    }
    const std::string file = path_join(staging, "graphicPacks.zip");
    std::remove(file.c_str());
    log_info("graphic packs: downloading " + release.name + " from " + release.url);
    std::string error;
    const bool downloaded = backend_->download(
        release.url, file, cancel_,
        [this](int64_t done, int64_t total) {
            set([&](Status& status) {
                status.progress = total > 0 ? static_cast<float>(static_cast<double>(done) / total) : -1.0f;
            });
        },
        error);
    if (!downloaded || cancel_) {
        std::remove(file.c_str());
        const bool cancelled = cancel_.load();
        log_info(cancelled ? "graphic packs: the download was cancelled"
                           : "graphic packs: did not download: " + error);
        set([&](Status& status) {
            if (cancelled) {
                status.phase = Phase::Checked;
            } else {
                status.phase = Phase::Failed;
                status.error = error;
            }
        });
        return;
    }
    set([&](Status& status) {
        status.phase = Phase::Installing;
        status.progress = -1.0f;
    });
    const bool installed = install_graphic_packs(file, user_data_, release.name, error);
    std::remove(file.c_str());
    if (!installed) {
        log_warn("graphic packs: " + release.name + " were not installed: " + error);
        set([&](Status& status) {
            status.phase = Phase::Failed;
            status.error = error;
        });
        return;
    }
    log_info("graphic packs: " + release.name + " are in " + downloaded_graphic_packs_dir(user_data_));
    set([&](Status& status) {
        status.phase = Phase::Installed;
        status.installed = release.name;
    });
}

}  // namespace ds::update
