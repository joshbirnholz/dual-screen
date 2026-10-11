// Checking for and installing nightly builds -- see update/updater.hpp -- with
// a backend that downloads nothing and records what it was asked to do.

#include "test_support.hpp"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "config/paths.hpp"
#include "ui/update_prompt.hpp"
#include "update/updater.hpp"

namespace {

using namespace ds::update;

std::string make_temp_dir() {
    char pattern[] = "/tmp/dualscreen-updater-XXXXXX";
    const char* directory = mkdtemp(pattern);
    return directory != nullptr ? directory : "";
}

struct FakeBackend : Backend {
    std::map<std::string, std::string> pages;
    std::string staging;
    bool fail_download = false;
    bool fail_install = false;
    bool restarts = true;
    std::vector<std::string> downloaded;
    std::vector<std::string> installed;

    bool fetch(const std::string& url, std::string& body, const std::atomic<bool>&, std::string& error) override {
        const auto page = pages.find(url);
        if (page == pages.end()) {
            error = "not found";
            return false;
        }
        body = page->second;
        return true;
    }
    bool download(const std::string& url, const std::string& path, const std::atomic<bool>& cancel,
                  const std::function<void(int64_t, int64_t)>& progress, std::string& error) override {
        if (cancel) return false;
        if (fail_download) {
            error = "the network went away";
            return false;
        }
        progress(5, 10);
        FILE* file = std::fopen(path.c_str(), "wb");
        if (file == nullptr) {
            error = "could not write";
            return false;
        }
        std::fputs(url.c_str(), file);
        std::fclose(file);
        progress(10, 10);
        downloaded.push_back(url);
        return true;
    }
    std::string staging_dir() override { return staging; }
    bool install(const ComponentSpec& component, const ReleaseInfo& release, const std::string& file,
                 std::string& error) override {
        if (fail_install) {
            error = "the folder is read-only";
            return false;
        }
        if (!ds::file_exists(file)) {
            error = "nothing was downloaded";
            return false;
        }
        installed.push_back(component.name + " " + std::to_string(release.build));
        return true;
    }
    bool restarts_after_install() const override { return restarts; }
};

const std::string kDualscreenVersion = release_url(kDualscreenRepository, "version.json");
const std::string kCemuVersion = release_url(kCemuRepository, "version.json");

void test_reads_version_json() {
    ReleaseInfo info;
    std::string error;
    CHECK(parse_release_info(
        R"({"build": 42, "commit": "abc123", "branch": "main", "built": "2026-10-11T04:00:00Z"})", info, &error));
    CHECK_EQ(info.build, 42);
    CHECK_STR_EQ(info.commit, std::string("abc123"));
    CHECK_STR_EQ(info.built, std::string("2026-10-11T04:00:00Z"));

    // A captive portal's page, a release with no number, and nonsense.
    CHECK(!parse_release_info("<html>Sign in to the Wi-Fi</html>", info, &error));
    CHECK(!error.empty());
    CHECK(!parse_release_info(R"({"commit": "abc"})", info, &error));
    CHECK(!parse_release_info(R"({"build": "42"})", info, &error));
    CHECK(!parse_release_info(R"({"build": 0})", info, &error));
    CHECK(!parse_release_info(R"([1, 2])", info, &error));
}

void test_release_urls_are_the_rolling_nightly() {
    CHECK_STR_EQ(release_url("joshbirnholz/dual-screen", "Dualscreen.nro"),
                 std::string("https://github.com/joshbirnholz/dual-screen/releases/download/nightly/Dualscreen.nro"));
}

// A build from CI is offered anything newer, and nothing else. A local build,
// or a component not installed, only when the player asked.
void test_what_is_worth_offering() {
    const ReleaseInfo newest{15, "", ""};
    ComponentSpec from_ci{Component::Dualscreen, "Dualscreen", kDualscreenRepository, "x", 12};
    CHECK(worth_offering(from_ci, newest, false));
    from_ci.installed = 15;
    CHECK(!worth_offering(from_ci, newest, false));
    CHECK(!worth_offering(from_ci, newest, true));
    from_ci.installed = 16;
    CHECK(!worth_offering(from_ci, newest, true));

    ComponentSpec local{Component::Dualscreen, "Dualscreen", kDualscreenRepository, "x", kLocalBuild};
    CHECK(!worth_offering(local, newest, false));
    CHECK(worth_offering(local, newest, true));

    ComponentSpec missing{Component::Cemu, "Cemu", kCemuRepository, kCemuAsset, kNotInstalled};
    CHECK(!worth_offering(missing, newest, false));
    CHECK(worth_offering(missing, newest, true));
}

void test_a_check_finds_a_newer_build_and_installs_it() {
    auto backend = std::make_unique<FakeBackend>();
    FakeBackend* fake = backend.get();
    fake->staging = make_temp_dir();
    fake->pages[kDualscreenVersion] = R"({"build": 15})";
    Updater updater(std::move(backend), components_for("dualscreen-steamos.tar.gz", 12, false, kNotInstalled));

    updater.check(false);
    updater.wait();
    Status status = updater.status();
    CHECK(status.phase == Phase::Checked);
    CHECK_EQ(status.newest_build, 15);
    CHECK_EQ(static_cast<int>(status.offers.size()), 1);
    const int round = status.round;

    updater.install();
    updater.wait();
    status = updater.status();
    CHECK(status.phase == Phase::Installed);
    CHECK(status.restart);
    CHECK(!status.handed_to_system);
    CHECK(status.round > round);
    CHECK_EQ(static_cast<int>(fake->installed.size()), 1);
    CHECK_STR_EQ(fake->installed.front(), std::string("Dualscreen 15"));
    CHECK_STR_EQ(fake->downloaded.front(), release_url(kDualscreenRepository, "dualscreen-steamos.tar.gz"));
    CHECK_EQ(updater.components().front().installed, 15);
}

void test_up_to_date_offers_nothing() {
    auto backend = std::make_unique<FakeBackend>();
    backend->pages[kDualscreenVersion] = R"({"build": 15})";
    Updater updater(std::move(backend), components_for("Dualscreen.nro", 15, false, kNotInstalled));
    updater.check(true);
    updater.wait();
    const Status status = updater.status();
    CHECK(status.phase == Phase::Checked);
    CHECK(status.offers.empty());
    // And installing nothing does nothing.
    updater.install();
    updater.wait();
    CHECK(updater.status().phase == Phase::Checked);
}

// Without a network the check fails and says why; nothing is offered.
void test_a_check_that_cannot_reach_github_fails() {
    auto backend = std::make_unique<FakeBackend>();
    Updater updater(std::move(backend), components_for("Dualscreen.nro", 12, false, kNotInstalled));
    updater.check(false);
    updater.wait();
    const Status status = updater.status();
    CHECK(status.phase == Phase::Failed);
    CHECK(!status.error.empty());
    CHECK(status.offers.empty());
}

// Cemu, kept by Dualscreen, is checked beside it -- and one that cannot be
// checked does not stop Dualscreen's own update.
void test_cemu_is_offered_beside_dualscreen() {
    auto backend = std::make_unique<FakeBackend>();
    FakeBackend* fake = backend.get();
    fake->staging = make_temp_dir();
    fake->pages[kDualscreenVersion] = R"({"build": 15})";
    fake->pages[kCemuVersion] = R"({"build": 4})";
    Updater updater(std::move(backend), components_for("dualscreen-steamos.tar.gz", 12, true, 3));
    updater.check(false);
    updater.wait();
    CHECK_EQ(static_cast<int>(updater.status().offers.size()), 2);
    updater.install();
    updater.wait();
    CHECK(updater.status().phase == Phase::Installed);
    CHECK_EQ(static_cast<int>(fake->installed.size()), 2);

    auto unreachable = std::make_unique<FakeBackend>();
    unreachable->pages[kDualscreenVersion] = R"({"build": 15})";
    Updater partly(std::move(unreachable), components_for("dualscreen-steamos.tar.gz", 12, true, 3));
    partly.check(false);
    partly.wait();
    CHECK(partly.status().phase == Phase::Checked);
    CHECK_EQ(static_cast<int>(partly.status().offers.size()), 1);

    // Cemu alone being newer is no reason to restart Dualscreen.
    auto cemu_only = std::make_unique<FakeBackend>();
    cemu_only->staging = make_temp_dir();
    cemu_only->pages[kDualscreenVersion] = R"({"build": 12})";
    cemu_only->pages[kCemuVersion] = R"({"build": 4})";
    Updater cemu(std::move(cemu_only), components_for("dualscreen-steamos.tar.gz", 12, true, 3));
    cemu.check(false);
    cemu.wait();
    cemu.install();
    cemu.wait();
    CHECK(cemu.status().phase == Phase::Installed);
    CHECK(!cemu.status().restart);
}

// A download that fails installs nothing, and a failed install says why.
void test_failures_leave_what_is_installed() {
    auto backend = std::make_unique<FakeBackend>();
    FakeBackend* fake = backend.get();
    fake->staging = make_temp_dir();
    fake->pages[kDualscreenVersion] = R"({"build": 15})";
    fake->fail_download = true;
    Updater updater(std::move(backend), components_for("Dualscreen.nro", 12, false, kNotInstalled));
    updater.check(false);
    updater.wait();
    updater.install();
    updater.wait();
    CHECK(updater.status().phase == Phase::Failed);
    CHECK_STR_EQ(updater.status().error, std::string("the network went away"));
    CHECK(fake->installed.empty());
    CHECK_EQ(updater.components().front().installed, 12);

    auto read_only = std::make_unique<FakeBackend>();
    read_only->staging = make_temp_dir();
    read_only->pages[kDualscreenVersion] = R"({"build": 15})";
    read_only->fail_install = true;
    Updater stuck(std::move(read_only), components_for("Dualscreen.nro", 12, false, kNotInstalled));
    stuck.check(false);
    stuck.wait();
    stuck.install();
    stuck.wait();
    CHECK(stuck.status().phase == Phase::Failed);
    CHECK_STR_EQ(stuck.status().error, std::string("the folder is read-only"));
}

// Android's installer takes over from the app rather than it restarting.
void test_a_system_installer_is_not_a_restart() {
    auto backend = std::make_unique<FakeBackend>();
    backend->staging = make_temp_dir();
    backend->restarts = false;
    backend->pages[kDualscreenVersion] = R"({"build": 15})";
    Updater updater(std::move(backend), components_for("dualscreen-android.apk", 12, false, kNotInstalled));
    updater.check(false);
    updater.wait();
    updater.install();
    updater.wait();
    CHECK(updater.status().phase == Phase::Installed);
    CHECK(!updater.status().restart);
    CHECK(updater.status().handed_to_system);
}

void test_managed_cemu_build() {
    const std::string data = make_temp_dir();
    CHECK_EQ(managed_cemu_build(data), kNotInstalled);
    CHECK(ds::ensure_directory(ds::path_dirname(managed_cemu_path(data))));
    FILE* file = std::fopen(managed_cemu_path(data).c_str(), "wb");
    CHECK(file != nullptr);
    if (file != nullptr) std::fclose(file);
    // There, but nobody wrote down which.
    CHECK_EQ(managed_cemu_build(data), kLocalBuild);
    file = std::fopen(managed_cemu_build_file(data).c_str(), "wb");
    CHECK(file != nullptr);
    if (file != nullptr) {
        std::fputs("7\n", file);
        std::fclose(file);
    }
    CHECK_EQ(managed_cemu_build(data), 7);
}

// Polls until the updater has finished what it is doing.
ds::UpdatePromptResult settle(ds::UpdatePrompt& prompt, Updater& updater, bool may_offer) {
    updater.wait();
    return prompt.poll(may_offer);
}

// The prompt the shells put up: offered at the main menu once a check at
// start finds something, and installing on Update.
void test_the_prompt_offers_and_installs() {
    auto backend = std::make_unique<FakeBackend>();
    backend->staging = make_temp_dir();
    backend->pages[kDualscreenVersion] = R"({"build": 15})";
    Updater updater(std::move(backend), components_for("dualscreen-steamos.tar.gz", 12, false, kNotInstalled));
    ds::UpdatePrompt prompt;
    prompt.set_updater(&updater);
    prompt.check_at_start();
    // Not over a game: the answer waits for the main menu.
    settle(prompt, updater, false);
    CHECK(!prompt.open());
    settle(prompt, updater, true);
    CHECK(prompt.open());
    CHECK_STR_EQ(prompt.row_value(), std::string("Build 15 available"));

    prompt.update({ds::UiAction::Confirm}, {}, 1280.0f, 800.0f);
    const ds::UpdatePromptResult result = settle(prompt, updater, true);
    CHECK(result.restart);
    CHECK(prompt.open());  // saying it is restarting, while the app closes
    // Said once.
    CHECK(!prompt.poll(true).restart);
}

// "Not Now" on the start-up offer is remembered, and the same build is not
// offered at the next start -- but Settings still offers it, and a newer one
// is offered as usual.
void test_not_now_is_remembered() {
    const std::string declined = ds::path_join(make_temp_dir(), "declined");
    const std::string staging = make_temp_dir();
    auto run_start = [&](int newest, bool decline) {
        auto backend = std::make_unique<FakeBackend>();
        backend->staging = staging;
        backend->pages[kDualscreenVersion] = "{\"build\": " + std::to_string(newest) + "}";
        Updater updater(std::move(backend), components_for("Dualscreen.nro", 12, false, kNotInstalled));
        ds::UpdatePrompt prompt;
        prompt.set_updater(&updater);
        prompt.set_declined_file(declined);
        prompt.check_at_start();
        settle(prompt, updater, true);
        const bool offered = prompt.open();
        if (offered && decline) prompt.update({ds::UiAction::Cancel}, {}, 1280.0f, 800.0f);
        CHECK(!prompt.open() || !decline);
        if (!offered) {
            // Settings asks regardless.
            prompt.choose();
            settle(prompt, updater, false);
            CHECK(prompt.open());
        }
        return offered;
    };
    CHECK(run_start(15, true));
    CHECK(!run_start(15, false));
    CHECK(run_start(16, false));
}

// Asked from Settings with nothing newer: said so.
void test_up_to_date_is_said_when_asked() {
    auto backend = std::make_unique<FakeBackend>();
    backend->pages[kDualscreenVersion] = R"({"build": 15})";
    Updater updater(std::move(backend), components_for("Dualscreen.nro", 15, false, kNotInstalled));
    ds::UpdatePrompt prompt;
    prompt.set_updater(&updater);
    prompt.choose();
    settle(prompt, updater, false);
    CHECK(prompt.open());
    CHECK_STR_EQ(prompt.row_value(), std::string("Build 15"));
    prompt.update({ds::UiAction::Confirm}, {}, 1280.0f, 800.0f);
    CHECK(!prompt.open());
}

}  // namespace

int main() {
    test_reads_version_json();
    test_release_urls_are_the_rolling_nightly();
    test_what_is_worth_offering();
    test_a_check_finds_a_newer_build_and_installs_it();
    test_up_to_date_offers_nothing();
    test_a_check_that_cannot_reach_github_fails();
    test_cemu_is_offered_beside_dualscreen();
    test_failures_leave_what_is_installed();
    test_a_system_installer_is_not_a_restart();
    test_managed_cemu_build();
    test_the_prompt_offers_and_installs();
    test_not_now_is_remembered();
    test_up_to_date_is_said_when_asked();
    return test::finish("updater");
}
