#if defined(__APPLE__)
// TARGET_OS_IPHONE and TARGET_OS_TV are used below to check that this build
// answers for the platform it is. Undefined they are zero, which would quietly
// assert the macOS answer on an iPhone -- so the header is included rather than
// left to arrive through SDL, which is where it happens to come from today.
#include <TargetConditionals.h>
#endif

#include "test_support.hpp"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <unistd.h>

#include "app.hpp"
#include "config/game_config.hpp"
#include "config/paths.hpp"
#include "config/settings.hpp"
#include "config/sha1.hpp"

using namespace ds;

namespace {

std::string make_sandbox() {
    char pattern[] = "/tmp/dualscreen-test-XXXXXX";
    const char* directory = mkdtemp(pattern);
    return directory != nullptr ? std::string(directory) : std::string();
}

std::string read_file(const std::string& path) {
    std::ifstream file(path);
    std::stringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

void test_sha1_known_vectors() {
    // The canonical FIPS-180 vectors, plus the empty string.
    CHECK_STR_EQ(Sha1::hash_bytes("", 0), "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    CHECK_STR_EQ(Sha1::hash_bytes("abc", 3), "a9993e364706816aba3e25717850c26c9cd0d89d");
    CHECK_STR_EQ(Sha1::hash_bytes("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56),
                 "84983e441c3bd26ebaae4aa1f95129e5e54670f1");

    // Exactly one block, and one byte past a block, exercise the padding paths.
    const std::string sixty_four(64, 'a');
    CHECK_STR_EQ(Sha1::hash_bytes(sixty_four.data(), sixty_four.size()),
                 "0098ba824b5c16427bd7a1122a5a442a25ec644d");
    const std::string million(1000000, 'a');
    CHECK_STR_EQ(Sha1::hash_bytes(million.data(), million.size()),
                 "34aa973cd4c4daa4f61eeb2bdbad27316534016f");

    // A reused hasher must give the same answer as a fresh one.
    Sha1 hasher;
    hasher.update("abc", 3);
    CHECK_STR_EQ(hasher.finalize(), "a9993e364706816aba3e25717850c26c9cd0d89d");
    hasher.update("abc", 3);
    CHECK_STR_EQ(hasher.finalize(), "a9993e364706816aba3e25717850c26c9cd0d89d");

    // Streaming in pieces matches hashing in one go.
    Sha1 streamed;
    streamed.update("abcdbcdecdefdefgefghfghi", 24);
    streamed.update("ghijhijkijkljklmklmnlmnomnopnopq", 32);
    CHECK_STR_EQ(streamed.finalize(), "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
}

void test_sha1_file() {
    const std::string sandbox = make_sandbox();
    CHECK(!sandbox.empty());
    const std::string path = path_join(sandbox, "rom.nds");
    {
        std::ofstream file(path, std::ios::binary);
        file << "abc";
    }
    CHECK_STR_EQ(Sha1::hash_file(path), "a9993e364706816aba3e25717850c26c9cd0d89d");
    // A missing file hashes to nothing rather than to the empty-input digest,
    // so "no ROM" can never collide with a real game's key.
    CHECK(Sha1::hash_file(path_join(sandbox, "absent.nds")).empty());
}

void test_paths_resolve_under_xdg() {
    const std::string sandbox = make_sandbox();
    setenv("XDG_CONFIG_HOME", path_join(sandbox, "config").c_str(), 1);
    setenv("XDG_DATA_HOME", path_join(sandbox, "data").c_str(), 1);

    const AppPaths paths = AppPaths::resolve();
    CHECK_STR_EQ(paths.config_dir, path_join(sandbox, "config/dualscreen"));
    CHECK_STR_EQ(paths.games_dir, path_join(sandbox, "config/dualscreen/games"));
    CHECK_STR_EQ(paths.settings_file, path_join(sandbox, "config/dualscreen/settings.json"));

    CHECK(ensure_directory(paths.games_dir));
    CHECK(directory_exists(paths.games_dir));
    // ensure_directory is idempotent.
    CHECK(ensure_directory(paths.games_dir));
}

// Where a Mac puts things, checked from a machine that is not one.
//
// This is the point of PathConvention being a value rather than a set of
// conditionals. A directory convention is not something to get wrong quietly
// on somebody else's computer, and a build for a platform this test does not
// run on cannot otherwise be checked at all -- the ifdef would simply be
// taken on trust until a person on a Mac noticed their settings in a folder
// no Mac application uses.
void test_each_platform_puts_things_where_that_platform_keeps_them() {
    // Nothing in the environment, which is Apple's case: macOS has no XDG
    // convention, and honouring one would only give the same file a second
    // place to hide in.
    const auto nothing_set = [](const char*) { return std::string(); };

    const PathConvention mac = path_convention(PathStyle::AppleLibrary, "/Users/someone",
                                               nothing_set);
    const AppPaths mac_paths = AppPaths::under(mac);
    CHECK_STR_EQ(mac_paths.config_dir, "/Users/someone/Library/Application Support/dualscreen");
    CHECK_STR_EQ(mac_paths.settings_file,
                 "/Users/someone/Library/Application Support/dualscreen/settings.json");
    CHECK_STR_EQ(mac_paths.saves_dir,
                 "/Users/someone/Library/Application Support/dualscreen/saves");
    // Logs are the one thing macOS keeps somewhere else, and a Mac user
    // looking for a log looks in exactly one place.
    CHECK_STR_EQ(mac_paths.log_file, "/Users/someone/Library/Logs/dualscreen/dualscreen.log");
    CHECK_MSG(mac_paths.config_dir.find("/.config") == std::string::npos,
              "a Mac build was going to write to a dot directory nothing on a Mac reads");

    // An Apple build must ignore XDG even where something has set it, or a
    // Mac that once ran something Linuxish gets its settings in two places.
    const auto xdg_set = [](const char* name) {
        return std::string(name) == "XDG_CONFIG_HOME" ? std::string("/elsewhere")
                                                      : std::string();
    };
    CHECK_STR_EQ(path_convention(PathStyle::AppleLibrary, "/Users/someone", xdg_set).config_root,
                 "/Users/someone/Library/Application Support");

    // And the other way round: the XDG style is the one that reads them, and
    // falls back to the standard defaults when it finds nothing.
    const PathConvention plain = path_convention(PathStyle::Xdg, "/home/someone", nothing_set);
    CHECK_STR_EQ(plain.config_root, "/home/someone/.config");
    CHECK_STR_EQ(plain.data_root, "/home/someone/.local/share");
    CHECK_STR_EQ(AppPaths::under(plain).log_file,
                 "/home/someone/.local/share/dualscreen/dualscreen.log");
    CHECK_STR_EQ(path_convention(PathStyle::Xdg, "/home/someone", xdg_set).config_root,
                 "/elsewhere");

    // An iPhone or iPad is the same shape: the home directory is the app's own
    // sandbox container rather than a person's, but Application Support means
    // what it means and the app never shows a path to anybody anyway.
    //
    // An Apple TV is not. There is no Application Support and no Documents on
    // tvOS at all -- Library/Caches is the only place an app may write a file,
    // and the system may empty it whenever it wants the space back. Writing to
    // a directory that does not exist there is how a companion would come up
    // once and never save a setting again, silently.
    const PathConvention tv = path_convention(PathStyle::AppleCaches, "/var/mobile/Containers/app",
                                              nothing_set);
    const AppPaths tv_paths = AppPaths::under(tv);
    CHECK_STR_EQ(tv_paths.config_dir, "/var/mobile/Containers/app/Library/Caches/dualscreen");
    CHECK_MSG(tv_paths.config_dir.find("Application Support") == std::string::npos,
              "an Apple TV build was going to write where tvOS has no such directory");
    // Including the log, which on tvOS has nowhere else to go: Library/Logs is
    // not writable and there is no Console to read it with.
    CHECK_STR_EQ(tv_paths.log_file,
                 "/var/mobile/Containers/app/Library/Caches/dualscreen/dualscreen.log");
    CHECK_STR_EQ(path_convention(PathStyle::AppleCaches, "/var/mobile/Containers/app", xdg_set)
                     .config_root,
                 "/var/mobile/Containers/app/Library/Caches");

    // This build's own answer is the one resolve() uses, and on anything that
    // is not Apple it is the freedesktop one.
#if defined(__APPLE__)
#if TARGET_OS_TV
    CHECK(native_path_style() == PathStyle::AppleCaches);
#else
    CHECK(native_path_style() == PathStyle::AppleLibrary);
#endif
#else
    CHECK(native_path_style() == PathStyle::Xdg);
#endif
}

void test_path_helpers() {
    CHECK_STR_EQ(path_join("/a", "b"), "/a/b");
    CHECK_STR_EQ(path_join("/a/", "b"), "/a/b");
    CHECK_STR_EQ(path_join("/a", "/b"), "/b");
    CHECK_STR_EQ(path_join("", "b"), "b");
    CHECK_STR_EQ(path_basename("/a/b/c.nds"), "c.nds");
    CHECK_STR_EQ(path_stem("/a/b/Pokemon Platinum.nds"), "Pokemon Platinum");
    CHECK_STR_EQ(path_stem("noextension"), "noextension");
    CHECK_STR_EQ(path_dirname("/a/b/c.nds"), "/a/b");
    setenv("HOME", "/home/tester", 1);
    CHECK_STR_EQ(expand_user_path("~/roms"), "/home/tester/roms");
    CHECK_STR_EQ(expand_user_path("/absolute"), "/absolute");
    CHECK_STR_EQ(expand_user_path("~notauser/x"), "~notauser/x");

    // absolute_path is the one that guarantees a path with a parent. "." is
    // the case that matters: path_dirname maps it to itself, so a browser that
    // ever stored a "." could never climb out of it.
    CHECK_STR_EQ(absolute_path("~/roms"), "/home/tester/roms");
    CHECK_STR_EQ(absolute_path("/a/./b/../c"), "/a/c");
    CHECK_STR_EQ(absolute_path("/a/b/"), "/a/b");
    CHECK_STR_EQ(absolute_path("/"), "/");
    CHECK_STR_EQ(absolute_path("/.."), "/");
    CHECK_STR_EQ(absolute_path(""), "");

    // Relative inputs resolve against the working directory, and never come
    // back as something path_dirname would treat as its own parent.
    char buffer[4096];
    const std::string working = ::getcwd(buffer, sizeof(buffer));
    CHECK_STR_EQ(absolute_path("."), working);
    CHECK_STR_EQ(absolute_path("./tests"), working + "/tests");
    CHECK_STR_EQ(absolute_path("CMakeFiles"), working + "/CMakeFiles");
    for (const char* relative : {".", "./tests", "CMakeFiles", ".."}) {
        const std::string resolved = absolute_path(relative);
        CHECK_MSG(!resolved.empty() && resolved.front() == '/',
                  std::string(relative) + " did not resolve to an absolute path");
        CHECK_MSG(path_dirname(resolved) != resolved,
                  std::string(relative) + " resolved to a directory that is its own parent");
    }
}

void test_game_config_round_trip() {
    const std::string sandbox = make_sandbox();
    setenv("XDG_CONFIG_HOME", path_join(sandbox, "config").c_str(), 1);
    setenv("XDG_DATA_HOME", path_join(sandbox, "data").c_str(), 1);
    const AppPaths paths = AppPaths::resolve();

    Settings settings;
    settings.default_bilinear_filtering = true;
    settings.default_render_scale = 2;
    settings.default_screen_gap = false;
    settings.default_right_stick_mode = RightStickMode::CirclePad;

    const std::string sha1 = "a9993e364706816aba3e25717850c26c9cd0d89d";

    // With no file on disk, the app-level defaults apply.
    GameConfig fresh = GameConfig::load_or_default(paths, settings, sha1, "Test Game");
    // Every game starts with its screens one above the other.
    CHECK_STR_EQ(fresh.layout_id, "vertical");
    CHECK_EQ(fresh.bilinear_filtering, true);
    CHECK_EQ(fresh.render_scale, 2);
    CHECK_EQ(fresh.screen_gap, false);
    CHECK(fresh.right_stick_mode == RightStickMode::CirclePad);
    CHECK_STR_EQ(fresh.rom_sha1, sha1);

    fresh.layout_id = "side_by_side";
    // Deliberately the opposite of the default, so a reload that quietly fell
    // back to the default would be visible rather than looking like a pass.
    fresh.bilinear_filtering = false;
    fresh.render_scale = 3;
    fresh.screen_gap = true;
    fresh.right_stick_mode = RightStickMode::Off;
    fresh.touch_last_played();
    CHECK(fresh.save(paths));

    // Reloading gives back what was saved, not the defaults.
    const GameConfig reloaded = GameConfig::load_or_default(paths, settings, sha1, "Test Game");
    CHECK_STR_EQ(reloaded.layout_id, "side_by_side");
    CHECK_EQ(reloaded.bilinear_filtering, false);
    CHECK_EQ(reloaded.render_scale, 3);
    CHECK_EQ(reloaded.screen_gap, true);
    CHECK(reloaded.right_stick_mode == RightStickMode::Off);
    CHECK(!reloaded.last_played.empty());

    // The file is keyed by hash and carries the forward-compatible keys.
    const std::string path = GameConfig::file_path(paths, sha1);
    CHECK(file_exists(path));
    CHECK(path.find(sha1) != std::string::npos);
    const std::string contents = read_file(path);
    CHECK(contents.find("\"rom_sha1\"") != std::string::npos);
    CHECK(contents.find("\"layout_id\"") != std::string::npos);
    CHECK(contents.find("\"screen_gap\"") != std::string::npos);
    CHECK(contents.find("\"right_stick_mode\"") != std::string::npos);
    CHECK(contents.find("\"last_played\"") != std::string::npos);
    CHECK(contents.find("\"role_default\"") != std::string::npos);
    CHECK(contents.find("\"companion_device_id\"") != std::string::npos);
}

void test_game_config_preserves_unknown_keys() {
    const std::string sandbox = make_sandbox();
    setenv("XDG_CONFIG_HOME", path_join(sandbox, "config").c_str(), 1);
    const AppPaths paths = AppPaths::resolve();
    ensure_directory(paths.games_dir);

    const std::string sha1 = "0000000000000000000000000000000000000001";
    const std::string path = GameConfig::file_path(paths, sha1);
    {
        std::ofstream file(path);
        file << R"({"rom_sha1":")" << sha1 << R"(","layout_id":"touch_focus",)"
             << R"("role_default":"host","companion_device_id":"deck-1",)"
             << R"("a_key_from_the_future":42})";
    }

    Settings settings;
    GameConfig config = GameConfig::load_or_default(paths, settings, sha1, "Future Game");
    CHECK_STR_EQ(config.layout_id, "touch_focus");
    // Keys reserved for the networked phase round-trip untouched.
    CHECK_STR_EQ(config.role_default, "host");
    CHECK_STR_EQ(config.companion_device_id, "deck-1");

    config.layout_id = "side_by_side";
    CHECK(config.save(paths));

    const std::string contents = read_file(path);
    CHECK(contents.find("a_key_from_the_future") != std::string::npos);
    CHECK(contents.find("\"host\"") != std::string::npos);
    CHECK(contents.find("deck-1") != std::string::npos);
    CHECK(contents.find("side_by_side") != std::string::npos);
}

void test_game_config_survives_a_corrupt_file() {
    const std::string sandbox = make_sandbox();
    setenv("XDG_CONFIG_HOME", path_join(sandbox, "config").c_str(), 1);
    const AppPaths paths = AppPaths::resolve();
    ensure_directory(paths.games_dir);

    const std::string sha1 = "0000000000000000000000000000000000000002";
    {
        std::ofstream file(GameConfig::file_path(paths, sha1));
        file << "{ this is not json";
    }
    Settings settings;
    const GameConfig config = GameConfig::load_or_default(paths, settings, sha1, "Broken");
    CHECK_STR_EQ(config.layout_id, "vertical");

    // A config with no hash has nothing to key a file on and must refuse to save.
    GameConfig unkeyed;
    CHECK(!unkeyed.save(paths));
}

void test_settings_round_trip() {
    const std::string sandbox = make_sandbox();
    setenv("XDG_CONFIG_HOME", path_join(sandbox, "config").c_str(), 1);
    setenv("XDG_DATA_HOME", path_join(sandbox, "data").c_str(), 1);
    const AppPaths paths = AppPaths::resolve();

    Settings defaults = Settings::load(paths);
    CHECK_STR_EQ(defaults.system_dir, paths.default_system_dir);

    // A Wii U needs a program to start; the arguments it is started with are
    // the fork's own, so setting the program is enough on its own. Consoles
    // this app plays itself start nothing.
    CHECK(defaults.external_program_for(SystemId::WiiU).empty());
    const std::vector<std::string> expected_arguments = {"--dualscreen-socket", "{socket}",
                                                         "--game", "{game}"};
    CHECK(defaults.external_arguments_for(SystemId::WiiU) == expected_arguments);
    CHECK(defaults.external_arguments_for(SystemId::Ds).empty());
    CHECK(defaults.external_arguments_for(SystemId::ThreeDs).empty());

    defaults.last_browse_dir = "/tmp/roms";
    defaults.vsync = false;
    defaults.stick_deadzone = 0.5f;
    defaults.default_bilinear_filtering = true;
    defaults.default_render_scale = 2;
    defaults.default_screen_gap = false;
    defaults.default_right_stick_mode = RightStickMode::DPad;
    defaults.left_stick_press = LeftStickPress::SwapLayout;
    defaults.video_hold = video::VideoHold::Responsive;
    defaults.run_ahead_ds = 2;
    defaults.audio_target = AudioTarget::BottomScreen;
    defaults.cursor_mode = CursorMode::Hidden;
    defaults.left_handed_layouts = {"upper_focus", "side_by_side"};
    // Away from Auto, the default, so the round trip is checking storage.
    defaults.wii_u_screen_alone = WiiUScreenAlone::GamePad;
    const WiiUScreenAlone chosen_alone = defaults.wii_u_screen_alone;
    CHECK(defaults.save(paths));

    const Settings reloaded = Settings::load(paths);
    CHECK(reloaded.left_handed_layouts ==
          std::vector<std::string>({"upper_focus", "side_by_side"}));
    CHECK(!reloaded.legacy_left_handed);
    CHECK_STR_EQ(reloaded.last_browse_dir, "/tmp/roms");
    CHECK_EQ(reloaded.vsync, false);
    CHECK_NEAR(reloaded.stick_deadzone, 0.5f);
    CHECK_EQ(reloaded.default_bilinear_filtering, true);
    CHECK_EQ(reloaded.default_render_scale, 2);
    CHECK(reloaded.default_right_stick_mode == RightStickMode::DPad);
    CHECK_MSG(reloaded.left_stick_press == LeftStickPress::SwapLayout,
              "what L3 does did not survive being written down");
    CHECK_MSG(reloaded.video_hold == video::VideoHold::Responsive,
              "how long a frame may be held back did not survive a save and load");
    CHECK_EQ(reloaded.run_ahead_ds, 2u);
    CHECK_EQ(reloaded.default_screen_gap, false);
    CHECK(reloaded.audio_target == AudioTarget::BottomScreen);
    CHECK(reloaded.cursor_mode == CursorMode::Hidden);
    CHECK_MSG(reloaded.wii_u_screen_alone == chosen_alone,
              "which screen this device is on its own did not survive being written down");

    // A config file that names arguments replaces them rather than adding to
    // them: a fork with different flags must not inherit these.
    {
        std::ofstream file(paths.settings_file);
        file << "{\"wii_u_arguments\": [\"--only\"]}";
    }
    const Settings overridden = Settings::load(paths);
    CHECK_EQ(overridden.external_arguments_for(SystemId::WiiU).size(), std::size_t{1});
    CHECK_STR_EQ(overridden.external_arguments_for(SystemId::WiiU).front(), "--only");

    // Malformed settings fall back to defaults rather than refusing to start.
    {
        std::ofstream file(paths.settings_file);
        file << "not json at all";
    }
    const Settings recovered = Settings::load(paths);
    CHECK_EQ(recovered.default_bilinear_filtering, false);
    CHECK_EQ(recovered.default_render_scale, 1);
    CHECK_EQ(recovered.default_screen_gap, true);
    CHECK(recovered.audio_target == AudioTarget::Both);
    CHECK(recovered.cursor_mode == CursorMode::Shown);

    // Text nobody recognises falls back rather than becoming a fourth state.
    CHECK(audio_target_from_key("sideways") == AudioTarget::Both);
    CHECK(audio_target_from_key("off") == AudioTarget::Off);
    CHECK_STR_EQ(audio_target_key(AudioTarget::Off), "off");
    CHECK(cursor_mode_from_key("") == CursorMode::Shown);
    // "auto" is what the old three-way version called the one that survived.
    CHECK(cursor_mode_from_key("auto") == CursorMode::Shown);
    CHECK(next_cursor_mode(CursorMode::Shown) == CursorMode::Hidden);
    CHECK(next_cursor_mode(CursorMode::Hidden) == CursorMode::Shown);

    // Cycling visits all three and comes back round.
    CHECK(next_audio_target(AudioTarget::Both) == AudioTarget::TopScreen);
    CHECK(next_audio_target(AudioTarget::TopScreen) == AudioTarget::BottomScreen);
    CHECK(next_audio_target(AudioTarget::BottomScreen) == AudioTarget::Both);

    // Who hears what, which is the whole point of the setting.
    CHECK(audio_reaches(AudioTarget::Both, ScreenId::Top));
    CHECK(audio_reaches(AudioTarget::Both, ScreenId::Bottom));
    CHECK(audio_reaches(AudioTarget::TopScreen, ScreenId::Top));
    CHECK(!audio_reaches(AudioTarget::TopScreen, ScreenId::Bottom));
    CHECK(!audio_reaches(AudioTarget::BottomScreen, ScreenId::Top));
    CHECK(audio_reaches(AudioTarget::BottomScreen, ScreenId::Bottom));
}

// Alone, a Wii U's device is the GamePad on a Steam Deck's own panel and the
// television anywhere else, unless somebody chose -- so a Deck in the hand is
// the GamePad and the same Deck on a dock is the television.
// A settings.json from before hands were chosen layout by layout said one hand
// for all of them. "left" is kept as a request for the app to flip every
// layout that has a hand; "right", or nothing, is simply none flipped.
void test_one_left_hand_for_every_layout_is_carried_over() {
    const std::string sandbox = make_sandbox();
    setenv("XDG_CONFIG_HOME", path_join(sandbox, "config").c_str(), 1);
    setenv("XDG_DATA_HOME", path_join(sandbox, "data").c_str(), 1);
    const AppPaths paths = AppPaths::resolve();
    CHECK(Settings::load(paths).save(paths));

    for (const char* hand : {"left", "right"}) {
        std::ofstream(paths.settings_file) << "{ \"handedness\": \"" << hand << "\" }";
        const Settings old = Settings::load(paths);
        CHECK_EQ(old.legacy_left_handed, std::string(hand) == "left");
        CHECK(old.left_handed_layouts.empty());
    }
}

void test_a_lone_device_is_the_gamepad_in_the_hand_and_the_television_on_a_dock() {
    CHECK(Settings().wii_u_screen_alone == WiiUScreenAlone::Auto);

    // In the hand, either way up.
    CHECK(resolve_wii_u_screen_alone(WiiUScreenAlone::Auto, 1280, 800) == ScreenId::Bottom);
    CHECK(resolve_wii_u_screen_alone(WiiUScreenAlone::Auto, 800, 1280) == ScreenId::Bottom);
    // Docked, whatever it is docked to.
    CHECK(resolve_wii_u_screen_alone(WiiUScreenAlone::Auto, 1920, 1080) == ScreenId::Top);
    CHECK(resolve_wii_u_screen_alone(WiiUScreenAlone::Auto, 3840, 2160) == ScreenId::Top);
    CHECK(resolve_wii_u_screen_alone(WiiUScreenAlone::Auto, 1280, 720) == ScreenId::Top);
    // Nothing to ask yet is not a Deck.
    CHECK(resolve_wii_u_screen_alone(WiiUScreenAlone::Auto, 0, 0) == ScreenId::Top);
    // A choice is a choice, whatever the display.
    CHECK(resolve_wii_u_screen_alone(WiiUScreenAlone::GamePad, 1920, 1080) == ScreenId::Bottom);
    CHECK(resolve_wii_u_screen_alone(WiiUScreenAlone::Television, 1280, 800) == ScreenId::Top);

    // The row goes round all three.
    CHECK(next_wii_u_screen_alone(WiiUScreenAlone::Auto) == WiiUScreenAlone::Television);
    CHECK(next_wii_u_screen_alone(WiiUScreenAlone::Television) == WiiUScreenAlone::GamePad);
    CHECK(next_wii_u_screen_alone(WiiUScreenAlone::GamePad) == WiiUScreenAlone::Auto);

    const std::string sandbox = make_sandbox();
    setenv("XDG_CONFIG_HOME", path_join(sandbox, "config").c_str(), 1);
    setenv("XDG_DATA_HOME", path_join(sandbox, "data").c_str(), 1);
    const AppPaths paths = AppPaths::resolve();
    // A file with nothing to say about it, or something unreadable, is Auto.
    {
        std::ofstream file(paths.settings_file);
        file << "{}";
    }
    CHECK(Settings::load(paths).wii_u_screen_alone == WiiUScreenAlone::Auto);
    {
        std::ofstream file(paths.settings_file);
        file << "{\"wii_u_alone\": \"sideways\"}";
    }
    CHECK(Settings::load(paths).wii_u_screen_alone == WiiUScreenAlone::Auto);
    // And one from before Auto existed is not read: what it holds was the old
    // version's guess from the machine's shape, written down whether or not
    // anybody chose it, and it would keep a Deck the GamePad on the dock.
    {
        std::ofstream file(paths.settings_file);
        file << "{\"wii_u_screen_alone\": \"gamepad\"}";
    }
    CHECK(Settings::load(paths).wii_u_screen_alone == WiiUScreenAlone::Auto);
}

void test_command_line() {
    const char* argv[] = {"dualscreen", "--game", "/roms/a.nds", "--layout", "side_by_side",
                          "--windowed"};
    const AppOptions options = parse_command_line(6, const_cast<char**>(argv));
    CHECK(options.parse_error.empty());
    CHECK_STR_EQ(options.game_path, "/roms/a.nds");
    CHECK_STR_EQ(options.layout_id, "side_by_side");
    CHECK_EQ(options.windowed, true);

    // A bare path works too, so a shortcut can just pass the ROM.
    const char* bare[] = {"dualscreen", "/roms/b.nds"};
    const AppOptions bare_options = parse_command_line(2, const_cast<char**>(bare));
    CHECK(bare_options.parse_error.empty());
    CHECK_STR_EQ(bare_options.game_path, "/roms/b.nds");

    // A flag with no value is reported, not silently swallowed.
    const char* truncated[] = {"dualscreen", "--game"};
    CHECK(!parse_command_line(2, const_cast<char**>(truncated)).parse_error.empty());
    const char* unknown[] = {"dualscreen", "--nope"};
    CHECK(!parse_command_line(2, const_cast<char**>(unknown)).parse_error.empty());

    const char* help[] = {"dualscreen", "--help"};
    CHECK(parse_command_line(2, const_cast<char**>(help)).show_help);

    // The codec override names one of the three the setting can hold, and
    // anything else is refused rather than quietly meaning one of them.
    const char* codec[] = {"dualscreen", "--video-codec", "tiles"};
    const AppOptions chosen = parse_command_line(3, const_cast<char**>(codec));
    CHECK(chosen.parse_error.empty());
    CHECK_STR_EQ(chosen.video_codec_override, "tiles");
    const char* nonsense[] = {"dualscreen", "--video-codec", "mpeg2"};
    CHECK(!parse_command_line(3, const_cast<char**>(nonsense)).parse_error.empty());
}

}  // namespace

int main() {
    test_one_left_hand_for_every_layout_is_carried_over();
    test_sha1_known_vectors();
    test_sha1_file();
    test_paths_resolve_under_xdg();
    test_each_platform_puts_things_where_that_platform_keeps_them();
    test_path_helpers();
    test_game_config_round_trip();
    test_game_config_preserves_unknown_keys();
    test_game_config_survives_a_corrupt_file();
    test_settings_round_trip();
    test_a_lone_device_is_the_gamepad_in_the_hand_and_the_television_on_a_dock();
    test_command_line();
    return test::finish("config");
}
