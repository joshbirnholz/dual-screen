#pragma once

#include <algorithm>
#include <array>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "config/melonds_tuning.hpp"
#include "config/paths.hpp"
#include "config/settings.hpp"
#include "core/system.hpp"
#include "input/input_manager.hpp"
#include "render/button_glyphs.hpp"
#include "render/text_renderer.hpp"
#include "ui/choice_popup.hpp"
#include "ui/menu_page.hpp"
#include "ui/popup.hpp"
#include "ui/text_entry.hpp"
#include "ui/control_guide.hpp"
#include "ui/list_touch.hpp"
#include "ui/sound_view.hpp"
#include "ui/ui_theme.hpp"

// Whether this device plays Wii U games: one that runs Cemu beside this app --
// a PC or a Steam Deck. A Switch and an Android device cannot, so the
// settings that only mean anything for a Wii U game are not offered there.
#if !defined(__SWITCH__) && !defined(__ANDROID__)
#define DS_PLAYS_WII_U 1
#else
#define DS_PLAYS_WII_U 0
#endif

// Whether other devices can start games on this one through its background
// service -- see launch/launch_host.hpp: a Linux PC or a Steam Deck, where it
// runs as a systemd user unit.
#if defined(__linux__) && !defined(__ANDROID__) && !defined(__SWITCH__)
#define DS_REMOTE_START_HOST 1
#else
#define DS_REMOTE_START_HOST 0
#endif

namespace ds {

// One BIOS/firmware file the core may want, and whether it is actually there.
struct BiosFileStatus {
    std::string filename;
    bool required = false;
    bool present = false;
    std::string note;
};

// Checks the melonDS DS BIOS/firmware files in `system_dir`. Nothing
// Nintendo-owned ships with this app — these have to be the player's own dumps,
// so the app's job is to say clearly which ones it can and cannot see.
std::vector<BiosFileStatus> check_bios_files(const std::string& system_dir);

struct SettingsScreenResult {
    bool close_requested = false;
    bool settings_changed = false;
    // Its Data Management row was chosen: the app opens it, and comes back
    // here when it is left.
    bool open_data_management = false;
    // Check for Updates was chosen: the app checks for a newer build of
    // Dualscreen and of every emulator it keeps -- see UpdatePrompt.
    bool open_updates = false;
    // A Download row under Updates was chosen: the app downloads the emulator
    // it keeps for that console -- Cemu, or a core -- see UpdatePrompt.
    std::optional<SystemId> open_download;
};

// The groups Settings files its rows in, listed down its left-hand side in
// this order. A group is the same on every device -- a row a device has no
// use for is left out of it, never moved to another -- and a group left with
// nothing in it is not listed.
enum class SettingsGroup { General, Consoles, WiiU, System, Count };
const char* settings_group_name(SettingsGroup group);

// The settings that can be changed without a keyboard, changed with a pad,
// laid out as Graphics Packs and a Switch's System Settings are: the groups
// listed on the left, and on the right the rows of the one highlighted there.
// Each console's group also says which emulator plays it and where it was
// found, with that emulator's own settings under it -- melonDS's on a Switch.
// Everything Dualscreen keeps up to date -- itself, and on a PC or a Steam
// Deck the emulators it downloads -- is under Updates in System, with one
// Check for Updates for all of them. Paths are edited in settings.json.
class SettingsScreen : public MenuPage {
public:
    // What a press here could have changed -- see UiSoundJudge.
    void describe_for_sound(UiSoundView& view) const;
    explicit SettingsScreen(const AppPaths& paths) : paths_(paths) {}

    // One entry per system, in the order they should be listed: the console's
    // name, which emulator plays it, and where that was resolved to (empty
    // when nothing was found).
    struct CoreStatus {
        std::string system_name;
        std::string resolved_path;
        SystemId system = SystemId::Ds;
        std::string emulator;
    };

    void open(const Settings& settings, const std::vector<CoreStatus>& cores);
    void close() { open_ = false; }
    // The settings it shows and hands back, replaced: after the app has
    // changed one itself -- the Wii U program, once Cemu is downloaded.
    void refresh_settings(const Settings& settings) { settings_ = settings; }
    // Where each emulator is, again: after Cemu has been downloaded, say.
    void set_cores(const std::vector<CoreStatus>& cores) { cores_ = cores; }

    // melonDS's settings, in the DS & 3DS group: how the Switch's melonDS
    // runs, kept in the melonds.ini at `path` -- see
    // config/melonds_tuning.hpp. `deko` is whether the core is RSDuck's, which
    // reads all of it; libretro's melonDS has only the CPU clock to set.
    void set_melonds_page(bool deko, std::string path);
    // Each emulator Dualscreen downloads and keeps -- Cemu, or a libretro
    // core -- listed under Updates in System, below Dualscreen itself: its
    // name, and what it says -- its build, Not installed, how far a download
    // has got. One not here yet, with `download`, is a row that downloads it;
    // the rest are kept up to date by Check for Updates.
    struct UpdateLine {
        std::string name;
        std::string value;
        std::optional<SystemId> download;
    };
    void set_update_lines(std::vector<UpdateLine> lines) { update_lines_ = std::move(lines); }
    // A Data Management row, in System, on a device that keeps saves of its
    // own.
    void set_offers_data_management(bool offers) { offers_data_management_ = offers; }
    // Updates, last in System, on a device that can update itself: Check for
    // Updates, then Dualscreen, saying what `value` does -- which build this
    // is, or what the updater is doing -- then each emulator it keeps. Over a
    // game too, for a game started from a shortcut, whose Settings is the
    // only one there is: the offer says what restarting does to the game.
    void set_offers_updates(bool offers) { offers_updates_ = offers; }
    void set_updates_value(std::string value) { updates_value_ = std::move(value); }
    // Back from Data Management: the cursor where it was, on its row.
    void return_from_data_management();
    // Straight to the row under Updates that downloads the emulator
    // `system` is played with -- from the alert a game not starting without
    // it puts up -- or to Check for Updates when there is none.
    void open_download_row(SystemId system);
    // Straight into a group's rows, the cursor on the first.
    void open_group(SettingsGroup group);
    // The group highlighted on the left, and whether the cursor is over in
    // its rows.
    SettingsGroup group() const;
    bool in_group() const { return in_group_; }
    bool is_open() const { return open_; }
    // A name being typed: the keyboard is to type into it -- see
    // InputManager::set_text_entry() -- and what it typed comes in through
    // type(), before update().
    bool wants_text() const { return text_entry_.is_open(); }
    void type(const std::string& typed, int backspaces) {
        typed_ = typed;
        backspaces_ = backspaces;
    }
    // A platform's own keyboard for typing a name -- a Switch's -- in place
    // of the popup: shown at once, true with what was typed, false if put
    // away.
    void set_text_editor(std::function<bool(const std::string& header, const std::string& initial,
                                            std::size_t max, std::string& out)>
                             editor) {
        text_editor_ = std::move(editor);
    }

    const Settings& settings() const { return settings_; }
    // Whether a paused game is behind it, to be dimmed rather than covered.
    void set_over_game(bool over_game) { over_game_ = over_game; }
    bool over_game() const { return over_game_; }

    SettingsScreenResult update(const std::vector<UiAction>& actions,
                                const std::vector<PointerEvent>& pointers,
                                float output_width,
                                float output_height);

    // MenuPage: the group's side scrolls under a finger, the mouse and the
    // wheel. Entries: a group in the list is its index times two, a row of
    // the group shown is its index times two plus one. A row's popup takes
    // its own taps.
    bool scroll(const std::vector<PointerEvent>& pointers, float output_width,
                float output_height) override;
    int entry_at(float x, float y, float output_width, float output_height) const override;
    bool off_popup(float, float, float, float) const override { return false; }
    // The group's side, in lines.
    std::vector<float> scroll_state(float output_width, float output_height) const override;
    void set_scroll_state(const std::vector<float>& state, float output_width, float output_height) override;

    void render(QuadRenderer& quads,
                TextRenderer& text,
                const ButtonGlyphSheet& glyphs,
                float output_width,
                float output_height);

public:
    // Which of the group's rows the cursor is on, counting rows alone, so the
    // shell can tell when it moved.
    int focus() const { return focus_; }

private:
    // The settings themselves. Which group each is in, and in what order, is
    // group_items()'s.
    //
    // A Switch has no button settings: its prompts are always its own, and
    // its menus confirm on A like every other Switch menu -- see
    // App::Impl::apply_button_style().
    enum class Row {
#ifndef __SWITCH__
        PadGlyphs,
        ConfirmButtonRow,
#endif
        VideoHold,
        RunAheadDs,
        DsNickname,
        DsFavoriteColor,
        DsBirthMonth,
        DsBirthDay,
        Vsync,
#if DS_PLAYS_WII_U
        WiiUScreenAlone,
        PauseWiiUInMenu,
        WiiUTvAudio,
        WiiRemoteRumble,
        // Desktops only, where Discord runs: see util/discord_presence.hpp.
        DiscordStatus,
#endif
#if DS_REMOTE_START_HOST
        // Enable or Disable Remote Start: installing the background service
        // that lets devices start games here, or removing it, each asked
        // first in an alert that says what it will do.
        RemoteStart,
#endif
        UiSounds,
        Count
    };
    std::string row_label(Row row) const;
    std::string row_value(Row row) const;

    // melonDS's rows, in order.
    enum class MelonRow {
        CpuClock, Pipeline
    };
    std::vector<MelonRow> melon_rows() const;

    // What a group's side holds, top to bottom. Only the rows take the
    // cursor -- a console's heading and where its emulator is do not -- and
    // focus_ counts those alone.
    struct Item {
        // Status: a name and what it says, under Updates, that the cursor
        // passes over.
        enum class Kind { Setting, Heading, Location, Status, Melon, Download, DataManagement, CheckUpdates };
        Kind kind = Kind::Setting;
        Row row = Row::Count;
        std::string text;
        std::string value;
        bool found = false;
        MelonRow melon = MelonRow::CpuClock;
        SystemId system = SystemId::Ds;
        bool focusable() const {
            return kind != Kind::Heading && kind != Kind::Location && kind != Kind::Status;
        }
    };
    std::vector<Item> group_items(SettingsGroup group) const;
    // The groups with anything in them, in order.
    std::vector<SettingsGroup> groups() const;
    // Built again whenever what they show may have changed: on moving to a
    // group, and before each update and each draw.
    void refresh_items() { items_ = group_items(group()); }
    // The item the cursor is on, or null; and the setting it is, or Count.
    const Item* focused_item() const;
    Row focused_row() const;
    // The group's rows.
    int row_count() const;

    struct Geometry {
        PixelRect panel;
        float scale = 1.0f;
        float line = 0.0f;
        // The window onto the page's content, under its heading.
        float view_top = 0.0f;
        float view_height = 0.0f;
        // The left-hand list, one rect per group, and the line beside it.
        std::vector<PixelRect> groups;
        float divider_x = 0.0f;
        // The right-hand side, and in it the group's rows, then its headings
        // and locations, where they go.
        PixelRect detail;
        std::vector<PixelRect> rows;
        struct Text {
            std::size_t item;
            float y;
        };
        std::vector<Text> texts;
        // Where the footnote under melonDS's rows starts.
        float footnote_top = 0.0f;
        // Everything the right-hand side holds, end to end, against the room
        // there is to show it in. The difference is how far it can scroll.
        float content_height = 0.0f;
        float max_scroll() const { return std::max(0.0f, content_height - view_height); }
    };

    Geometry compute_geometry(float output_width, float output_height) const;
    // Moves the view the least it can to bring the focused row fully into it.
    // Called after anything that moves the cursor, because a cursor you
    // cannot see is the same as no cursor.
    void scroll_focus_into_view(const Geometry& geometry);
    // A switch flipped, a page opened, or an alert put up: what a press on a
    // row without choices does.
    void adjust(SettingsScreenResult& result);
    // The cursor's row's choices, in order, and which is in effect, where it
    // has more to choose between than on and off -- chosen in a popup. Empty
    // for a switch, a way into a page, or the button row.
    std::vector<std::string> focused_choices(int& current) const;
    void choose(int index, SettingsScreenResult& result);
    // A press on the cursor's row: a popup of its choices, or what adjust()
    // does with it.
    void press_row(SettingsScreenResult& result, const Geometry& geometry);
    // Shows the `index`th group, the cursor still on the list.
    void show_group(int index);
    // A line said under the group's `index`th row, or null.
    const char* row_description(int index) const;
    float description_height(float scale) const;

    void adjust_melonds(MelonRow row);
    void save_melonds();
    static std::string melon_label(MelonRow row);
    std::string melon_value(MelonRow row) const;
    std::string footnote() const;

    AppPaths paths_;
    Settings settings_;
    std::vector<CoreStatus> cores_;
    // The group highlighted, by its place among groups(); whether the cursor
    // is over in its rows; and which of them.
    int group_ = 0;
    bool in_group_ = false;
    int focus_ = 0;
    // The cursor was put somewhere from outside, and the view goes to it at
    // the next update().
    bool reveal_focus_ = false;
    bool open_ = false;
    bool over_game_ = false;

    bool offers_data_management_ = false;
    bool offers_updates_ = false;
    std::string updates_value_;
    std::vector<UpdateLine> update_lines_;
    bool melonds_available_ = false;
    bool melonds_deko_ = false;
    std::string melonds_path_;
    MelonDsTuning tuning_;
    std::string melonds_error_;
    std::vector<Item> items_;
    // A row's choices, up beside it -- see focused_choices().
    ChoicePopup choice_;
    // The row the popup was opened beside, for the day's to open beside too.
    PixelRect choice_row_;
    // A button glyph's width for its height, from the sheet last drawn with,
    // so a popup of buttons can be sized before it is drawn.
    float glyph_aspect_ = 1.0f;
    // Remote Start, as the system has it: whether its unit is installed.
    bool remote_start_on_ = false;
    // The alert asking before Remote Start is enabled or disabled, up while
    // `asking_remote_start_`. `notice_` is an alert with only OK -- Remote
    // Start not turning on, say -- when it is not empty.
    bool asking_remote_start_ = false;
    std::string notice_;
    PopupCursor alert_cursor_;
    std::string remote_start_record() const { return paths_.config_dir + "/remote_start.json"; }
    // Enables or disables it, as the alert asked.
    void toggle_remote_start();
    std::vector<PopupButton> remote_start_buttons() const;
    PopupText remote_start_text() const;
    PopupGeometry remote_start_geometry(float output_width, float output_height) const;
    PopupGeometry notice_geometry(float output_width, float output_height) const;

    // The DS nickname being typed -- see type().
    TextEntryPopup text_entry_;
    std::string typed_;
    int backspaces_ = 0;
    std::function<bool(const std::string& header, const std::string& initial, std::size_t max,
                       std::string& out)>
        text_editor_;

    // How far down the group's side has been pushed, in pixels. Pixels rather
    // than rows, because a console's headings and locations are not rows and
    // would jump past in whole-row steps.
    float scroll_ = 0.0f;
    ListDrag drag_;
    ScrollEase scroll_ease_;

    // Where the prompts at the foot of the page were drawn -- see take_guide_taps().
    std::vector<GuideHit> guide_hits_;
};

}  // namespace ds
