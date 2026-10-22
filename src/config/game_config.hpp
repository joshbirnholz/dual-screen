#pragma once

#include <map>
#include <set>
#include <string>

#include "config/paths.hpp"
#include "config/settings.hpp"
#include "input/button_mapping.hpp"
#include "input/wii_u_mapping.hpp"
#include "layout/screen_id.hpp"

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
    // A DS game's screens with an LCD's pixel grid over them -- see
    // Compositor::set_lcd_grid(). Per game like the ones above.
    bool lcd_filter = false;
    // Which screen Separate Screens shows on this device when it is on its
    // own -- and, with a second device connected, which of the two this one
    // holds. Swapped from the quick menu or with L3, and kept per game, since
    // which screen a game is played on is a fact about that game.
    ScreenId separate_screen = ScreenId::Top;
    // What the right stick is read as. Per game because which of the answers a
    // 3DS game wants is a fact about that game -- see RightStickMode -- and
    // there is nothing in a ROM that says which.
    RightStickMode right_stick_mode = RightStickMode::CStick;
    // What each of the pad's buttons does in this game -- see ButtonMapping.
    // Only the buttons changed from the standard mapping are written.
    ButtonMapping button_mapping;
    // A Rumble Pak in the DS's GBA slot -- out unless asked for, since a game
    // that finds one may say so, and one that has no use for it gains nothing.
    bool rumble_pak = false;
    // A 3DS game drawn with the texture pack put in its folder of Azahar's
    // (see LibretroCoreScreenSource::offers_custom_textures()) in place of its
    // own textures. Off unless asked for, since a pack is something a player
    // goes looking for, and loading one costs memory and start-up time.
    bool custom_textures = false;
    // Start the game from where it was when the app last closed, rather than
    // from its title screen -- see App::Impl::write_resume_point(). Off unless
    // asked for, and only offered where save states are.
    bool resume_from_save_state = false;
    // A Wii U game played alone shows the other screen from the one
    // Settings::wii_u_screen_alone says -- Swap Screens in the quick menu, or
    // F3 -- until swapped back. Per game, since which screen a game is best
    // played on is a fact about that game.
    bool screens_swapped = false;
    // A Wii U game's choices among Cemu's own options that Cemu keeps for
    // every game alike -- see ExternalOption::default_choice -- by the
    // option's id: set again each time this game starts.
    std::map<std::string, int> wii_u_options;
    // A Wii U game's mapping for the GamePad and for each player as each
    // controller, by wii_u_mapping_key(). One never changed is not kept.
    std::map<std::string, WiiUMapping> wii_u_mappings;
    // The GamePad and players, by wii_u_slot_key(), whose motion or rumble
    // this game has off; the rest have it on.
    std::set<std::string> wii_u_motion_off;
    std::set<std::string> wii_u_rumble_off;
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
