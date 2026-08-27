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
    settings.default_layout_id = "side_by_side";
    settings.default_bilinear_filtering = true;
    settings.default_render_scale = 2;
    settings.default_screen_gap = false;
    settings.default_right_stick_mode = RightStickMode::CirclePad;

    const std::string sha1 = "a9993e364706816aba3e25717850c26c9cd0d89d";

    // With no file on disk, the app-level defaults apply.
    GameConfig fresh = GameConfig::load_or_default(paths, settings, sha1, "Test Game");
    CHECK_STR_EQ(fresh.layout_id, "side_by_side");
    CHECK_EQ(fresh.bilinear_filtering, true);
    CHECK_EQ(fresh.render_scale, 2);
    CHECK_EQ(fresh.screen_gap, false);
    CHECK(fresh.right_stick_mode == RightStickMode::CirclePad);
    CHECK_STR_EQ(fresh.rom_sha1, sha1);

    fresh.layout_id = "vertical";
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
    CHECK_STR_EQ(reloaded.layout_id, "vertical");
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
    settings.default_layout_id = "touch_focus";
    const GameConfig config = GameConfig::load_or_default(paths, settings, sha1, "Broken");
    CHECK_STR_EQ(config.layout_id, "touch_focus");

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
    CHECK_STR_EQ(defaults.default_layout_id, "standard");
    CHECK_STR_EQ(defaults.system_dir, paths.default_system_dir);

    defaults.default_layout_id = "side_by_side";
    defaults.last_browse_dir = "/tmp/roms";
    defaults.vsync = false;
    defaults.stick_deadzone = 0.5f;
    defaults.default_bilinear_filtering = true;
    defaults.default_render_scale = 2;
    defaults.default_screen_gap = false;
    defaults.default_right_stick_mode = RightStickMode::DPad;
    defaults.audio_target = AudioTarget::BottomScreen;
    defaults.cursor_mode = CursorMode::Hidden;
    defaults.screen_codec = video::ScreenCodec::H264;
    CHECK(defaults.save(paths));

    const Settings reloaded = Settings::load(paths);
    CHECK_STR_EQ(reloaded.default_layout_id, "side_by_side");
    CHECK_STR_EQ(reloaded.last_browse_dir, "/tmp/roms");
    CHECK_EQ(reloaded.vsync, false);
    CHECK_NEAR(reloaded.stick_deadzone, 0.5f);
    CHECK_EQ(reloaded.default_bilinear_filtering, true);
    CHECK_EQ(reloaded.default_render_scale, 2);
    CHECK(reloaded.default_right_stick_mode == RightStickMode::DPad);
    CHECK_EQ(reloaded.default_screen_gap, false);
    CHECK(reloaded.audio_target == AudioTarget::BottomScreen);
    CHECK(reloaded.cursor_mode == CursorMode::Hidden);
    CHECK(reloaded.screen_codec == video::ScreenCodec::H264);

    // Malformed settings fall back to defaults rather than refusing to start.
    {
        std::ofstream file(paths.settings_file);
        file << "not json at all";
    }
    const Settings recovered = Settings::load(paths);
    CHECK_STR_EQ(recovered.default_layout_id, "standard");
    CHECK_EQ(recovered.default_bilinear_filtering, false);
    CHECK_EQ(recovered.default_render_scale, 1);
    CHECK_EQ(recovered.default_screen_gap, true);
    CHECK(recovered.audio_target == AudioTarget::Both);
    CHECK(recovered.cursor_mode == CursorMode::Shown);
    CHECK(recovered.screen_codec == video::ScreenCodec::Auto);

    // Text nobody recognises falls back rather than becoming a fourth state.
    CHECK(audio_target_from_key("sideways") == AudioTarget::Both);
    CHECK(cursor_mode_from_key("") == CursorMode::Shown);
    // "auto" is what the old three-way version called the one that survived.
    CHECK(cursor_mode_from_key("auto") == CursorMode::Shown);
    CHECK(next_cursor_mode(CursorMode::Shown) == CursorMode::Hidden);
    CHECK(next_cursor_mode(CursorMode::Hidden) == CursorMode::Shown);

    // A codec nobody recognises is the automatic one rather than an error: a
    // typo in a config file should leave the app working as it would have.
    CHECK(video::screen_codec_from_key("mpeg2") == video::ScreenCodec::Auto);
    CHECK(video::screen_codec_from_key("") == video::ScreenCodec::Auto);
    CHECK(video::screen_codec_from_key("tiles") == video::ScreenCodec::Tiles);
    CHECK(video::next_screen_codec(video::ScreenCodec::Auto) == video::ScreenCodec::Tiles);
    CHECK(video::next_screen_codec(video::ScreenCodec::Tiles) == video::ScreenCodec::H264);
    CHECK(video::next_screen_codec(video::ScreenCodec::H264) == video::ScreenCodec::Auto);

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
    test_sha1_known_vectors();
    test_sha1_file();
    test_paths_resolve_under_xdg();
    test_path_helpers();
    test_game_config_round_trip();
    test_game_config_preserves_unknown_keys();
    test_game_config_survives_a_corrupt_file();
    test_settings_round_trip();
    test_command_line();
    return test::finish("config");
}
