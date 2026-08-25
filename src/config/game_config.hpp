#pragma once

#include <string>

#include "config/paths.hpp"
#include "config/settings.hpp"

namespace ds {

// Per-game settings, stored as ~/.config/dualscreen/games/<rom_sha1>.json.
//
// Keyed by content hash, not by path, so moving a ROM never loses its settings
// — and so a future networked session can agree on "same game" across two
// devices with different filesystems.
struct GameConfig {
    std::string rom_sha1;
    std::string display_name;
    std::string layout_id;
    // Smooth the screens when scaled up, or keep them pixel-crisp. Per game,
    // because it is a judgement about how one game looks.
    bool bilinear_filtering = false;
    // How many pixels the core renders per console pixel. Per game for the same
    // reason: how much a game can afford is a judgement about that game.
    int render_scale = 1;
    // Whether the current layout shows the gap between its screens, for
    // whichever layouts offer the choice. Per game like the two above, though
    // it only ever matters while one of those is the one chosen.
    bool screen_gap = true;
    // What the right stick is read as. Per game because which of the answers a
    // 3DS game wants is a fact about that game -- see RightStickMode -- and
    // there is nothing in a ROM that says which.
    RightStickMode right_stick_mode = RightStickMode::CStick;
    std::string last_played;  // ISO-8601 UTC

    // Reserved for the networked phase; unset in V1 but round-tripped so V1
    // files never need a migration. Any other unrecognised keys are preserved
    // on save too.
    std::string role_default;
    std::string companion_device_id;

    static std::string file_path(const AppPaths& paths, const std::string& rom_sha1);

    // Loads the config for this ROM hash, or synthesises defaults from
    // `settings` when no file exists yet.
    static GameConfig load_or_default(const AppPaths& paths,
                                      const Settings& settings,
                                      const std::string& rom_sha1,
                                      const std::string& display_name);

    bool save(const AppPaths& paths) const;

    // Fills last_played with the current UTC time.
    void touch_last_played();

private:
    // Verbatim copy of the file as loaded, so unknown/future keys survive a
    // save from an older build.
    std::string raw_document_;
};

std::string iso8601_utc_now();

}  // namespace ds
