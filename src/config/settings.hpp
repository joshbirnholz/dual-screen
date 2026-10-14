#pragma once

#include <string>
#include <vector>

#include "config/paths.hpp"
#include "core/system.hpp"
#include "input/controller_mapping.hpp"
#include "render/button_glyphs.hpp"
#include "input/cursor_visibility.hpp"
#include "layout/layout.hpp"
#include "video/video_codec.hpp"

namespace ds {

// What a device playing a Wii U on its own is: the television, the GamePad,
// or whichever its display says it is being used as.
//
// Auto is the default, and the reason is a Steam Deck: in the hand it is a
// GamePad, and docked to a television it is the television. Its own panel is
// 1280x800 and nothing it is docked to is, so the display's size is the one
// thing that tells the two apart without being asked. Asked when a game
// starts: a Deck's display does not change size under a running app, docked
// or not, until the app is started again.
enum class WiiUScreenAlone { Auto, Television, GamePad };

inline WiiUScreenAlone next_wii_u_screen_alone(WiiUScreenAlone screen) {
    switch (screen) {
        case WiiUScreenAlone::Auto: return WiiUScreenAlone::Television;
        case WiiUScreenAlone::Television: return WiiUScreenAlone::GamePad;
        case WiiUScreenAlone::GamePad: break;
    }
    return WiiUScreenAlone::Auto;
}

inline const char* to_string(WiiUScreenAlone screen) {
    switch (screen) {
        case WiiUScreenAlone::Auto: return "Auto";
        case WiiUScreenAlone::Television: return "TV";
        case WiiUScreenAlone::GamePad: return "GamePad";
    }
    return "Auto";
}

// The display a Steam Deck has built in, which Auto reads as "held in two
// hands". Either way up, since a panel mounted sideways is reported both ways
// depending on who is asking.
inline bool display_is_a_steam_decks_own(int width, int height) {
    return (width == 1280 && height == 800) || (width == 800 && height == 1280);
}

// Which screen that means, on a display of this size. A size of zero -- no
// display to ask yet -- is not a Deck's panel, so Auto starts as the
// television until it knows better.
// Settings::wii_u_tv_audio's choices, as Settings and Cemu's option name them,
// and that option's id over the link.
inline constexpr const char* kWiiUTvAudioChoices[3] = {"Mono", "Stereo", "Surround"};
inline constexpr const char* kWiiUTvAudioOption = "tv_channels";

inline ScreenId resolve_wii_u_screen_alone(WiiUScreenAlone screen, int display_width,
                                           int display_height) {
    switch (screen) {
        case WiiUScreenAlone::Television: return ScreenId::Top;
        case WiiUScreenAlone::GamePad: return ScreenId::Bottom;
        case WiiUScreenAlone::Auto: break;
    }
    return display_is_a_steam_decks_own(display_width, display_height) ? ScreenId::Bottom
                                                                        : ScreenId::Top;
}

// The layout a game starts in the first time it is played: its two screens
// one above the other. Not a setting -- a game remembers whatever it was last
// played in, and this is only where that starts. A console with no shape for
// it (a Wii U) falls back to its own; see SystemProfile::default_layout_id.
inline constexpr const char* kNewGameLayoutId = "vertical";

// App-level defaults, stored in ~/.config/dualscreen/settings.json.
struct Settings {

    // How this device identifies itself to the other one. Defaults to the
    // machine's hostname, which is what a player will recognise in a list.
    std::string device_name;

    // Directory the in-app game browser opens in. Updated as the player
    // navigates, so launching with no ROM picks up where they left off.
    std::string last_browse_dir;
    // And the one the Wii U quick menu's amiibo browser opens in.
    std::string amiibo_dir;
    // SteamGridDB's API key, asked for the first time a forwarder's icon is
    // searched for there, and kept.
    std::string steamgriddb_api_key;

    // Directory handed to the core as its libretro "system" directory; this is
    // where the user's own BIOS/firmware dumps live. Nothing Nintendo-owned is
    // bundled with this app.
    std::string system_dir;
    // Path to the DS libretro core (.so). Empty means "search the usual places"
    // — see core_search_paths().
    std::string core_path;
    // And the same for 3DS games, which need a different core.
    std::string core_path_3ds;

    // The path this system's core should be loaded from, or empty to search.
    const std::string& core_path_for(SystemId system) const {
        switch (system) {
            case SystemId::Ds: return core_path;
            case SystemId::ThreeDs: return core_path_3ds;
            // Played in a program of its own rather than a core this app loads.
            case SystemId::WiiU: break;
        }
        // A console whose frames do not come from a core this app loads has no
        // path to hold. Reached only if one is added without answering here,
        // which the switch above will not let happen quietly.
        static const std::string none;
        return none;
    }

    // The program that plays a console this app does not run itself, and the
    // arguments to start it with. Nothing here knows what that program's own
    // flags are called, so they are written out in full and two placeholders
    // are filled in: {socket} becomes the path it should hand its screens over
    // on, {game} the file the player picked.
    //
    // Empty means the player starts it themselves, which works on a desktop
    // and does not work in Game Mode -- where there is nothing to start it
    // from. See docs/cemu-integration.md.
    std::string wii_u_program;
    // The fork's own flags, so that setting the program is enough on its own.
    // See docs/cemu-integration.md.
    std::vector<std::string> wii_u_arguments{"--dualscreen-socket", "{socket}", "--game", "{game}"};
    // Cemu's mlc01 folder, where it keeps Wii U saves, for Data Management.
    // Empty means wherever Cemu says it is -- see find_cemu_mlc().
    std::string wii_u_mlc_path;
    // Whether a Wii U game pauses while the quick menu is up, as it does for
    // the console's HOME Menu. Off, it plays on underneath, darkened.
    bool pause_wii_u_in_menu = true;
    // What a Wii U game's TV sound comes out as -- Cemu's own TV channels
    // setting, the same for every game: 0 mono, 1 stereo, 2 surround. Read
    // by Cemu as a game starts. See kWiiUTvAudioChoices.
    int wii_u_tv_audio = 1;

    const std::string& external_program_for(SystemId system) const {
        switch (system) {
            case SystemId::WiiU: return wii_u_program;
            // Played by a core this app loads, so there is nothing to start.
            case SystemId::Ds:
            case SystemId::ThreeDs: break;
        }
        static const std::string none;
        return none;
    }

    const std::vector<std::string>& external_arguments_for(SystemId system) const {
        switch (system) {
            case SystemId::WiiU: return wii_u_arguments;
            case SystemId::Ds:
            case SystemId::ThreeDs: break;
        }
        static const std::vector<std::string> none;
        return none;
    }


    // Starting point for new games; each game remembers its own afterwards.
    bool default_bilinear_filtering = false;

    // And the same for the internal render resolution. 1 means the core draws
    // at the console's own resolution; a system whose core cannot upscale
    // ignores it.
    int default_render_scale = 1;

    // Starting point for whether a layout shows the gap between its screens,
    // for whichever layouts offer the choice; each game remembers its own
    // choice afterwards.
    bool default_screen_gap = true;

    // And for what the right stick is read as, on a console that has sticks of
    // its own. Which a 3DS game wants is the game's business, so each one
    // remembers its own; this is only where a new one starts.
    RightStickMode default_right_stick_mode = RightStickMode::CStick;

    // What clicking the left stick does on a console that has a microphone --
    // blow into it, or swap the two focus layouts the way it does everywhere
    // else. Defaults to the microphone, because the swap is also a press away
    // in the quick menu while blowing has no other way in at all; a player who
    // would rather keep the shortcut says so here. See LeftStickPress.
    LeftStickPress left_stick_press = LeftStickPress::Microphone;

    // Whose buttons the on-screen prompts are drawn as. Nintendo by default,
    // which is the artwork this app started with and the console most of what
    // it streams came from.
    //
    // This changes nothing about what a button does. The positions are fixed
    // -- see ButtonGlyph -- and this decides only what is printed on them.
    PadGlyphStyle pad_glyphs = PadGlyphStyle::Nintendo;

    // Which of the two lower face buttons means yes. East by default, which a
    // Nintendo pad prints A on and which is what this app has always done.
    ConfirmButton confirm_button = ConfirmButton::East;

    // How long a companion may hold a decoded frame back to even out an
    // uneven link -- see VideoHold. Defaults to what this app did before the
    // setting existed; the other two ends are there because the right answer
    // depends on wifi nobody here can measure.
    video::VideoHold video_hold = video::VideoHold::Balanced;

    // How many frames a DS game is run ahead of itself, to hide its own input
    // lag -- see LibretroCore::set_run_ahead(). Off by default: it costs a
    // whole frame of emulation per frame hidden, and whether this machine
    // has that to spare, and whether the core's save states are quick
    // enough, is only known by trying it. Two at the most from the menu.
    unsigned run_ahead_ds = 0;

    // Which device plays the sound while two are connected. Chosen from the
    // layout menu rather than here, since it is the kind of thing that changes
    // with the room rather than once at setup.
    AudioTarget audio_target = AudioTarget::Both;

    // Whether this device has a mouse pointer at all.
    CursorMode cursor_mode = CursorMode::Shown;

    // Which layouts are flipped for a left hand, by id. Chosen layout by
    // layout -- the stylus button under the layouts flips only the one in
    // use -- so each keeps whichever way round it was left. Every layout not
    // named here is drawn right-handed, as it is defined.
    std::vector<std::string> left_handed_layouts;
    // An older settings.json said "handedness": "left", for every layout at
    // once, and nothing yet says which layouts: the app turns that into every
    // layout that has a hand, which is what the player was seeing.
    bool legacy_left_handed = false;

    float stick_deadzone = 0.35f;   // fraction of full stick travel
    float trigger_deadzone = 0.30f;
    // Off by default: this loop reads input and sends it to the other device
    // in the same iteration it swaps the window (see the main loop's own
    // comment on this), so vsync does not just pace drawing here, it paces
    // input right along with it -- up to a whole display refresh added to
    // every button press and touch before it even leaves this device. Without
    // it, the loop is still correctly paced to the emulator's own frame rate
    // rather than spinning uselessly (same comment), so this is not trading
    // smooth video for a busy-loop -- only for the tear a player who wants it
    // back can still opt into from Settings. On, a game played alone runs one
    // frame to each refresh (see display_locked in the main loop), which
    // takes away the frame shown twice every few seconds that pacing by the
    // game's own clock leaves; on a Switch, without costing a refresh of input.
    bool vsync = false;

    // Which of a Wii U's two screens this device is, when it is the only one
    // there is.
    //
    // A Wii U's screens are not two halves of a picture, they are a
    // television and a thing in your hands, and a device on its own has to be
    // one or the other. Being the GamePad means Cemu shows the GamePad screen
    // and this device's own controller -- gyroscope included -- is what the
    // game reads as the GamePad, which is Off-TV play on a single machine.
    // Being the television means Cemu shows the television and the GamePad is
    // whatever the player has configured in Cemu itself, which is how to play
    // the many games that never ask for the pad with a controller already set
    // up for them.
    //
    // Only consulted while nothing else is connected. With a companion there
    // is no question to answer: the companion says which screen it is holding
    // and this device is the other one.
    //
    // Auto unless somebody chose otherwise -- see WiiUScreenAlone.
    WiiUScreenAlone wii_u_screen_alone = WiiUScreenAlone::Auto;

    // The menus' own sounds -- a cursor moving, something chosen, the quick
    // menu opening -- on this device. See UiSoundPlayer.
    bool ui_sounds = true;

    // The DS's owner, as its own settings have them: the name games greet
    // the player by, and the favorite color -- an index into
    // kDsFavoriteColors (config/ds_profile.hpp), or -1 for none chosen. Told
    // to melonDS DS as a game starts. The color is the menus' accent too.
    std::string ds_nickname;
    int ds_favorite_color = -1;

    static Settings load(const AppPaths& paths);
    bool save(const AppPaths& paths) const;
};

// Places a melonDS DS core may be installed, most-preferred first. The
// settings' core_path, then $DUALSCREEN_CORE, take priority over these.
std::vector<std::string> core_search_paths(SystemId system);

}  // namespace ds
