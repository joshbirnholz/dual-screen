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
#include "update/zip_extract.hpp"

#include <zlib.h>

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

ComponentSpec spec(Component id, const std::string& name, const std::string& repository,
                   const std::string& asset, int installed) {
    ComponentSpec component;
    component.id = id;
    component.name = name;
    component.repository = repository;
    component.asset = asset;
    component.installed = installed;
    return component;
}

ReleaseInfo release_numbered(int build) {
    ReleaseInfo release;
    release.build = build;
    return release;
}

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
    const ReleaseInfo newest = release_numbered(15);
    ComponentSpec from_ci = spec(Component::Dualscreen, "Dualscreen", kDualscreenRepository, "x", 12);
    CHECK(worth_offering(from_ci, newest, false));
    from_ci.installed = 15;
    CHECK(!worth_offering(from_ci, newest, false));
    CHECK(!worth_offering(from_ci, newest, true));
    from_ci.installed = 16;
    CHECK(!worth_offering(from_ci, newest, true));

    ComponentSpec local = spec(Component::Dualscreen, "Dualscreen", kDualscreenRepository, "x", kLocalBuild);
    CHECK(!worth_offering(local, newest, false));
    CHECK(worth_offering(local, newest, true));

    ComponentSpec missing = spec(Component::Cemu, "Cemu", kCemuRepository, kCemuAsset, kNotInstalled);
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

// The buildbot's index: the first of a core's files it lists, by its date
// and hash.
void test_reads_the_buildbot_index() {
    const std::string index =
        "2026-10-09 1a2b3c4d citra_libretro.so.zip\r\n"
        "2026-10-10 5e6f7a8b melondsds_libretro.so.zip\n"
        "2026-10-08 99999999 melonds_libretro.so.zip\n";
    ReleaseInfo release;
    CHECK(parse_buildbot_index(index, {"melondsds_libretro.so.zip"}, release));
    CHECK_STR_EQ(release.version, std::string("2026-10-10 5e6f7a8b"));
    CHECK_STR_EQ(release.built, std::string("2026-10-10"));
    CHECK_STR_EQ(release.asset, std::string("melondsds_libretro.so.zip"));
    // Azahar wanted, Citra there: Citra.
    CHECK(parse_buildbot_index(index, {"azahar_libretro.so.zip", "citra_libretro.so.zip"}, release));
    CHECK_STR_EQ(release.asset, std::string("citra_libretro.so.zip"));
    CHECK_STR_EQ(release.version, std::string("2026-10-09 1a2b3c4d"));
    // A name that only ends the same is not the core.
    CHECK(!parse_buildbot_index("2026-10-10 5e6f7a8b xmelondsds_libretro.so.zip\n", {"melondsds_libretro.so.zip"},
                                release));
    CHECK_STR_EQ(download_url(core_component(Component::DsCore, "/nowhere", false), release),
                 std::string(kBuildbotLinux) + "citra_libretro.so.zip");
}

// A core is offered when it is a different build from the one here, and --
// not installed, or not the one in use -- only when asked for by name.
void test_cores_are_offered_by_version() {
    ReleaseInfo release;
    release.version = "2026-10-10 5e6f7a8b";
    ComponentSpec core = core_component(Component::DsCore, "/nowhere", true);
    CHECK_EQ(core.installed, kNotInstalled);
    CHECK(!worth_offering(core, release, false));
    CHECK(worth_offering(core, release, true));
    core.installed = kLocalBuild;
    core.installed_version = "2026-10-01 00000000";
    CHECK(worth_offering(core, release, false));
    core.installed_version = release.version;
    CHECK(!worth_offering(core, release, true));
    core.installed_version = "2026-10-01 00000000";
    core.automatic = false;
    CHECK(!worth_offering(core, release, false));
    CHECK(worth_offering(core, release, true));
}

// Asked for by name on the Emulators page: downloaded from the buildbot,
// Citra standing in where it has no Azahar, and nothing else looked at.
void test_a_core_is_downloaded_when_asked() {
    auto backend = std::make_unique<FakeBackend>();
    FakeBackend* fake = backend.get();
    fake->staging = make_temp_dir();
    fake->pages[buildbot_index_url(kBuildbotLinux)] = "2026-10-09 1a2b3c4d citra_libretro.so.zip\n";
    std::vector<ComponentSpec> components = components_for("dualscreen-steamos.tar.gz", 12, true, kNotInstalled, false);
    components.push_back(core_component(Component::ThreeDsCore, make_temp_dir(), false));
    Updater updater(std::move(backend), components);
    // Not automatic: Settings' Updates row does not look at it.
    updater.check(true);
    updater.wait();
    CHECK(updater.status().phase == Phase::Failed);  // no Dualscreen page in the fake
    updater.check(true, Component::ThreeDsCore);
    updater.wait();
    Status status = updater.status();
    CHECK(status.phase == Phase::Checked);
    CHECK_EQ(static_cast<int>(status.offers.size()), 1);
    if (status.offers.size() == 1) {
        CHECK_STR_EQ(status.offers.front().component.name, std::string("Citra"));
        CHECK_STR_EQ(describe_release(status.offers.front().component, status.offers.front().release),
                     std::string("built 2026-10-09"));
    }
    updater.install();
    updater.wait();
    status = updater.status();
    CHECK(status.phase == Phase::Installed);
    CHECK(!status.restart);
    CHECK_STR_EQ(fake->downloaded.front(), std::string(kBuildbotLinux) + "citra_libretro.so.zip");
    for (const ComponentSpec& component : updater.components()) {
        if (component.id != Component::ThreeDsCore) continue;
        CHECK_STR_EQ(component.installed_version, std::string("2026-10-09 1a2b3c4d"));
        CHECK_STR_EQ(component.asset, std::string("citra_libretro.so.zip"));
    }
}

// What core_component() finds in the cores folder.
void test_finds_the_cores_here() {
    const std::string cores = make_temp_dir();
    auto touch = [](const std::string& path, const std::string& text) {
        FILE* file = std::fopen(path.c_str(), "wb");
        if (file == nullptr) return;
        std::fputs(text.c_str(), file);
        std::fclose(file);
    };
    touch(ds::path_join(cores, "citra_libretro.so"), "x");
    ComponentSpec three = core_component(Component::ThreeDsCore, cores, true);
    CHECK_EQ(three.installed, kLocalBuild);
    CHECK_STR_EQ(three.name, std::string("Citra"));
    CHECK_STR_EQ(three.installed_version, std::string("unknown"));
    touch(core_version_file(cores, "citra_libretro.so"), "2026-10-09 1a2b3c4d\n");
    three = core_component(Component::ThreeDsCore, cores, true);
    CHECK_STR_EQ(three.installed_version, std::string("2026-10-09 1a2b3c4d"));
    // Azahar, once it is there, ahead of Citra.
    touch(ds::path_join(cores, "azahar_libretro.so"), "x");
    CHECK_STR_EQ(core_component(Component::ThreeDsCore, cores, true).name, std::string("Azahar"));
    CHECK_STR_EQ(core_library_name("melondsds_libretro.so.zip"), std::string("melondsds_libretro.so"));
}

// A zip of two files, one stored and one deflated, the way a zip tool writes
// them.
std::vector<uint8_t> make_zip(const std::vector<std::pair<std::string, std::string>>& files) {
    std::vector<uint8_t> out;
    std::vector<uint8_t> directory;
    auto put16 = [](std::vector<uint8_t>& to, uint32_t v) {
        to.push_back(v & 0xFF);
        to.push_back((v >> 8) & 0xFF);
    };
    auto put32 = [&](std::vector<uint8_t>& to, uint32_t v) {
        put16(to, v & 0xFFFF);
        put16(to, v >> 16);
    };
    for (std::size_t index = 0; index < files.size(); ++index) {
        const std::string& name = files[index].first;
        const std::string& body = files[index].second;
        const bool deflated = index % 2 == 1;
        std::vector<uint8_t> packed(body.begin(), body.end());
        if (deflated) {
            packed.assign(body.size() + 64, 0);
            z_stream stream{};
            deflateInit2(&stream, 9, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
            stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(body.data()));
            stream.avail_in = static_cast<uInt>(body.size());
            stream.next_out = packed.data();
            stream.avail_out = static_cast<uInt>(packed.size());
            deflate(&stream, Z_FINISH);
            packed.resize(stream.total_out);
            deflateEnd(&stream);
        }
        const uint32_t crc = static_cast<uint32_t>(
            crc32(crc32(0L, Z_NULL, 0), reinterpret_cast<const Bytef*>(body.data()), static_cast<uInt>(body.size())));
        const uint32_t offset = static_cast<uint32_t>(out.size());
        put32(out, 0x04034b50); put16(out, 20); put16(out, 0); put16(out, deflated ? 8 : 0);
        put16(out, 0); put16(out, 0); put32(out, crc);
        put32(out, static_cast<uint32_t>(packed.size())); put32(out, static_cast<uint32_t>(body.size()));
        put16(out, static_cast<uint32_t>(name.size())); put16(out, 0);
        out.insert(out.end(), name.begin(), name.end());
        out.insert(out.end(), packed.begin(), packed.end());
        put32(directory, 0x02014b50); put16(directory, 20); put16(directory, 20); put16(directory, 0);
        put16(directory, deflated ? 8 : 0); put16(directory, 0); put16(directory, 0); put32(directory, crc);
        put32(directory, static_cast<uint32_t>(packed.size())); put32(directory, static_cast<uint32_t>(body.size()));
        put16(directory, static_cast<uint32_t>(name.size())); put16(directory, 0); put16(directory, 0);
        put16(directory, 0); put16(directory, 0); put32(directory, 0); put32(directory, offset);
        directory.insert(directory.end(), name.begin(), name.end());
    }
    const uint32_t directory_offset = static_cast<uint32_t>(out.size());
    out.insert(out.end(), directory.begin(), directory.end());
    put32(out, 0x06054b50); put16(out, 0); put16(out, 0);
    put16(out, static_cast<uint32_t>(files.size())); put16(out, static_cast<uint32_t>(files.size()));
    put32(out, static_cast<uint32_t>(directory.size())); put32(out, directory_offset); put16(out, 0);
    return out;
}

void test_reads_a_core_out_of_its_zip() {
    const std::string folder = make_temp_dir();
    const std::string zip = ds::path_join(folder, "core.zip");
    std::string big(100000, 'a');
    for (std::size_t i = 0; i < big.size(); i += 7) big[i] = static_cast<char>('a' + i % 26);
    const std::vector<uint8_t> bytes = make_zip({{"readme.txt", "hello"}, {"melondsds_libretro.so", big}});
    CHECK(ds::write_file_atomically(zip, bytes.data(), bytes.size(), nullptr));
    std::vector<std::string> names;
    CHECK(list_zip(zip, names, nullptr));
    CHECK_EQ(static_cast<int>(names.size()), 2);
    std::string error;
    const std::string out = ds::path_join(folder, "melondsds_libretro.so");
    CHECK(extract_zip_entry(zip, "melondsds_libretro.so", out, &error));
    std::vector<uint8_t> got;
    CHECK(ds::read_file(out, got, nullptr));
    CHECK(std::string(got.begin(), got.end()) == big);
    CHECK(extract_zip_entry(zip, "readme.txt", ds::path_join(folder, "readme.txt"), &error));
    CHECK(!extract_zip_entry(zip, "missing.so", ds::path_join(folder, "missing.so"), &error));
    CHECK(!error.empty());
    // Damaged: a byte of the deflated data changed fails its CRC or inflate.
    std::vector<uint8_t> broken = bytes;
    broken[200] ^= 0xFF;  // inside the deflated entry, which starts at 96
    CHECK(ds::write_file_atomically(zip, broken.data(), broken.size(), nullptr));
    CHECK(!extract_zip_entry(zip, "melondsds_libretro.so", out + ".2", &error));
    // Not a zip at all.
    CHECK(ds::write_file_atomically(zip, "<html>", 6, nullptr));
    CHECK(!list_zip(zip, names, &error));
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
    test_reads_the_buildbot_index();
    test_cores_are_offered_by_version();
    test_a_core_is_downloaded_when_asked();
    test_finds_the_cores_here();
    test_reads_a_core_out_of_its_zip();
    return test::finish("updater");
}
