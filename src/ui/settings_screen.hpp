#pragma once

#include <algorithm>

#include <string>
#include <vector>

#include "config/melonds_tuning.hpp"
#include "config/paths.hpp"
#include "config/settings.hpp"
#include "input/input_manager.hpp"
#include "render/button_glyphs.hpp"
#include "render/text_renderer.hpp"
#include "ui/list_touch.hpp"
#include "ui/ui_theme.hpp"

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
};

// Shows where the app is looking for the core and the BIOS files, and lets the
// few settings that can be changed without a keyboard be changed with a pad.
// Paths are edited in settings.json; the BIOS folder is the default one unless
// that file names another.
class SettingsScreen {
public:
    explicit SettingsScreen(const AppPaths& paths) : paths_(paths) {}

    // One entry per system, in the order they should be listed: the console's
    // name and where its core was resolved to (empty when nothing was found).
    struct CoreStatus {
        std::string system_name;
        std::string resolved_path;
    };

    void open(const Settings& settings, const std::vector<CoreStatus>& cores);
    void close() { open_ = false; }

    // A melonDS row, opening a page of how the Switch's melonDS runs, kept in
    // the melonds.ini at `path` -- see config/melonds_tuning.hpp. `deko` is
    // whether the core is RSDuck's, which reads all of it; libretro's melonDS
    // has only the CPU clock and the app's own drawing to set.
    void set_melonds_page(bool deko, std::string path);
    // A Data Management row, first, on a device that keeps saves of its own.
    void set_offers_data_management(bool offers) { offers_data_management_ = offers; }
    // Back from Data Management: the cursor where it was, on its row.
    void return_from_data_management();
    bool on_melonds_page() const { return page_ == Page::MelonDs; }
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
        WiiUScreenAlone,
        Count
    };
    static constexpr int kRowCount = static_cast<int>(Row::Count);

    struct Geometry {
        std::vector<PixelRect> rows;
        PixelRect panel;
        PixelRect core_panel;
        PixelRect bios_panel;
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
    void refresh_bios();

    // The melonDS page's rows, in order.
    enum class MelonRow {
        CpuClock, Jit, FastMemory, Pipeline, GpuAlone, TextureCache, Shaders, VramCheck, GpuCache, L2Flush,
        Settle, Renderer
    };
    enum class Page { Main, MelonDs };
    std::vector<MelonRow> melon_rows() const;
    // Rows on the page showing: Data Management's first when there is one,
    // then the main page's, and the melonDS row after them when there is one.
    int row_count() const;
    int data_management_row() const { return offers_data_management_ ? 0 : -1; }
    // Where the rows of Row start: after Data Management's.
    int first_setting_row() const { return offers_data_management_ ? 1 : 0; }
    int melonds_row() const { return first_setting_row() + kRowCount; }
    void open_melonds_page();
    void adjust_melonds(MelonRow row, int direction);
    static std::string melon_label(MelonRow row);
    std::string melon_value(MelonRow row) const;
    std::string footnote() const;

    AppPaths paths_;
    Settings settings_;
    std::vector<CoreStatus> cores_;
    std::vector<BiosFileStatus> bios_;
    int focus_ = 0;
    bool open_ = false;
    bool over_game_ = false;

    bool offers_data_management_ = false;
    bool melonds_available_ = false;
    bool melonds_deko_ = false;
    std::string melonds_path_;
    MelonDsTuning tuning_;
    std::string melonds_error_;
    Page page_ = Page::Main;

    // How far down the list has been pushed, in pixels. Pixels rather than
    // rows, because what is below the rows -- the cores, then the BIOS files
    // -- is not made of rows and would jump past in whole-row steps.
    float scroll_ = 0.0f;
    ListDrag drag_;
    // Dragged by a finger on a device with a touch screen -- a Deck, a Switch
    // -- the same as every list.
};

}  // namespace ds
