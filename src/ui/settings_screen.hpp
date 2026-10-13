#pragma once

#include <algorithm>
#include <array>
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
    // Its Updates row was chosen: the app checks for a newer build -- see
    // UpdatePrompt.
    bool open_updates = false;
    // One of the Emulators page's download rows was chosen: the app
    // downloads or updates the emulator it keeps for that console -- Cemu, or
    // a core -- see UpdatePrompt.
    std::optional<SystemId> open_download;
};

// The settings that can be changed without a keyboard, changed with a pad, and
// an Emulators page: one section for each console this device plays, saying
// which emulator plays it and where it was found, with that emulator's own
// settings under it -- melonDS's on a Switch, and on a PC or a Steam Deck the
// Cemu that Dualscreen downloads and keeps up to date. Paths are edited in
// settings.json.
class SettingsScreen {
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

    // melonDS's settings, in the Emulators page's DS section: how the
    // Switch's melonDS runs, kept in the melonds.ini at `path` -- see
    // config/melonds_tuning.hpp. `deko` is whether the core is RSDuck's, which
    // reads all of it; libretro's melonDS has only the CPU clock to set.
    void set_melonds_page(bool deko, std::string path);
    // A row in a console's section for the emulator Dualscreen downloads and
    // keeps for it -- Cemu, or a libretro core: `label`, and what it says --
    // its build, Not installed, how far a download has got. Chosen, the app
    // downloads or updates it.
    void set_download_row(SystemId system, std::string label, std::string value) {
        download_rows_[system] = {std::move(label), std::move(value)};
    }
    // A Data Management row, first, on a device that keeps saves of its own.
    void set_offers_data_management(bool offers) { offers_data_management_ = offers; }
    // An Updates row, last, on a device that can update itself, saying what
    // `value` does -- which build this is, or what the updater is doing. Not
    // over a game: an update ends with the app starting again.
    void set_offers_updates(bool offers) { offers_updates_ = offers; }
    void set_updates_value(std::string value) { updates_value_ = std::move(value); }
    // Back from Data Management: the cursor where it was, on its row.
    void return_from_data_management();
    bool on_emulators_page() const { return page_ == Page::Emulators; }
    // Straight to the Emulators page, as its row does.
    void open_emulators_page();
    bool is_open() const { return open_; }

    const Settings& settings() const { return settings_; }
    // Whether a paused game is behind it, to be dimmed rather than covered.
    void set_over_game(bool over_game) { over_game_ = over_game; }

    SettingsScreenResult update(const std::vector<UiAction>& actions,
                                const std::vector<PointerEvent>& pointers,
                                float output_width,
                                float output_height);

    void render(QuadRenderer& quads,
                TextRenderer& text,
                const ButtonGlyphSheet& glyphs,
                float output_width,
                float output_height);

public:
    // Where the cursor is, so the shell can tell when it moved.
    int focus() const { return focus_; }

private:
    // The cores are not in here: they are read-only, one per console, and a
    // focusable row that does nothing when pressed is worse than a plain list.
    //
    // A Switch has no button settings: its prompts are always its own, and
    // its menus confirm on A like every other Switch menu -- see
    // App::Impl::apply_button_style().
    enum class Row {
        // Every row is a value -- but the ways into Data Management and
        // melonDS's page, which come before and after these -- and leaving
        // is the cancel button's: a row that only did that would be a second
        // way to do something already on the controller.
#ifndef __SWITCH__
        PadGlyphs,
        ConfirmButtonRow,
#endif
        VideoHold,
        RunAheadDs,
        Vsync,
#if DS_PLAYS_WII_U
        WiiUScreenAlone,
        PauseWiiUInMenu,
#endif
        UiSounds,
        ShowBattery,
        Count
    };
    static constexpr int kRowCount = static_cast<int>(Row::Count);

    struct Geometry {
        std::vector<PixelRect> rows;
        PixelRect panel;
        // The Emulators page's headings and locations, where they go.
        struct Text {
            std::size_t entry;
            float y;
        };
        std::vector<Text> texts;
        float scale = 1.0f;
        // Everything the panel holds, laid out end to end, against the room
        // there is to show it in. The difference is how far this can scroll.
        float content_height = 0.0f;
        // The window onto it: the page's content area.
        float view_top = 0.0f;
        float view_height = 0.0f;
        // Where the footnote under the BIOS files starts.
        float footnote_top = 0.0f;
        float max_scroll() const { return std::max(0.0f, content_height - view_height); }
    };

    Geometry compute_geometry(float output_width, float output_height) const;
    // Moves the view the least it can to bring the focused row fully into it.
    // Called after anything that moves the cursor, because a cursor you
    // cannot see is the same as no cursor.
    void scroll_focus_into_view(const Geometry& geometry);
    void adjust(int direction, SettingsScreenResult& result);
    // A line said under a row, or null. Only Vsync has one.
    const char* row_description(int index) const;
    float description_height(float scale) const;

    // melonDS's rows, in order.
    enum class MelonRow {
        CpuClock, Pipeline
    };
    enum class Page { Main, Emulators };
    std::vector<MelonRow> melon_rows() const;

    // What the Emulators page holds, top to bottom: for each console a
    // heading and where its emulator is, then that emulator's rows. Only the
    // rows take the cursor; focus_ counts those alone.
    struct Entry {
        enum class Kind { Heading, Location, Melon, Download } kind = Kind::Heading;
        std::string text;
        std::string value;
        bool found = false;
        MelonRow melon = MelonRow::CpuClock;
        SystemId system = SystemId::Ds;
        bool focusable() const { return kind == Kind::Melon || kind == Kind::Download; }
    };
    std::vector<Entry> emulator_entries() const;
    // Built again whenever what they show may have changed: on opening the
    // page, and before each update and each draw.
    void refresh_entries() { entries_ = emulator_entries(); }
    // The entry the cursor is on, on the Emulators page, or null.
    const Entry* focused_entry() const;

    // Rows on the page showing: Data Management's first when there is one,
    // then the main page's, then Emulators and Updates when there are those.
    int row_count() const;
    int data_management_row() const { return offers_data_management_ ? 0 : -1; }
    // Where the rows of Row start: after Data Management's.
    int first_setting_row() const { return offers_data_management_ ? 1 : 0; }
    bool has_emulators_page() const { return !cores_.empty() || melonds_available_; }
    int emulators_row() const { return has_emulators_page() ? first_setting_row() + kRowCount : -1; }
    int updates_row() const {
        return offers_updates_ && !over_game_
                   ? first_setting_row() + kRowCount + (has_emulators_page() ? 1 : 0)
                   : -1;
    }
    void adjust_melonds(MelonRow row, int direction);
    static std::string melon_label(MelonRow row);
    std::string melon_value(MelonRow row) const;
    std::string footnote() const;

    AppPaths paths_;
    Settings settings_;
    std::vector<CoreStatus> cores_;
    int focus_ = 0;
    bool open_ = false;
    bool over_game_ = false;

    bool offers_data_management_ = false;
    bool offers_updates_ = false;
    std::string updates_value_;
    std::map<SystemId, std::array<std::string, 2>> download_rows_;
    bool melonds_available_ = false;
    bool melonds_deko_ = false;
    std::string melonds_path_;
    MelonDsTuning tuning_;
    std::string melonds_error_;
    Page page_ = Page::Main;
    std::vector<Entry> entries_;

    // How far down the list has been pushed, in pixels. Pixels rather than
    // rows, because what is below the rows -- the cores, then the BIOS files
    // -- is not made of rows and would jump past in whole-row steps.
    float scroll_ = 0.0f;
    ListDrag drag_;
    // Dragged by a finger on a device with a touch screen -- a Deck, a Switch
    // -- the same as every list.

    // Where the prompts at the foot of the page were drawn -- see take_guide_taps().
    std::vector<GuideHit> guide_hits_;
};

}  // namespace ds
