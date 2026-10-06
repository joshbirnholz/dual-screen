#pragma once

#include <string>
#include <vector>

#include "config/paths.hpp"
#include "input/controller_mapping.hpp"
#include "input/input_manager.hpp"
#include "render/button_glyphs.hpp"
#include "render/text_renderer.hpp"
#include "ui/control_guide.hpp"
#include "ui/list_touch.hpp"
#include "ui/sound_view.hpp"
#include "ui/ui_theme.hpp"

namespace ds {

// True if this filename looks like something the DS core can load.
bool is_supported_rom(const std::string& filename);

// Directories worth opening in when there is no remembered one: the usual
// EmuDeck/RetroDECK ROM folders, then removable media, then home. Only
// directories that exist are returned, most likely first.
std::vector<std::string> default_browse_dirs();

struct GameBrowserResult {
    bool game_chosen = false;
    std::string chosen_path;
    // The browsed directory changed, so the remembered one is worth updating.
    bool directory_changed = false;
    // Back to where the browser was opened from, rather than up a folder:
    // Main Menu, on plus. There is no Quit here -- the main menu has one.
    bool back_requested = false;
    // X on a ROM, where forwarders can be made: Forwarder Creation for
    // `chosen_path`.
    bool forwarder_requested = false;
};

// A file browser for picking a ROM, used when the app is launched with no
// --game. Navigable with a controller and by touch, including drag scrolling,
// because on a Deck in Game Mode both are the only inputs there are.
class GameBrowser {
public:
    // What a press here could have changed -- see UiSoundJudge.
    void describe_for_sound(UiSoundView& view) const;
    void open(const std::string& directory);
    void close() { open_ = false; }
    bool is_open() const { return open_; }

    const std::string& current_directory() const { return directory_; }
    // Whether X makes a forwarder for the ROM under the cursor: only where
    // one can be installed -- a Switch.
    void set_offers_forwarders(bool offers) { offers_forwarders_ = offers; }

    GameBrowserResult update(const InputManager& input,
                             const std::vector<UiAction>& actions,
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
    struct Row {
        std::string label;
        std::string path;
        bool is_directory = false;
        bool is_parent = false;
    };

    struct Geometry {
        PixelRect panel;
        PixelRect list;
        float row_height = 0.0f;
        float scale = 1.0f;
        int visible_rows = 1;
    };

    Geometry compute_geometry(float output_width, float output_height) const;
    // By value on purpose: see the definition. Returns whether the browser
    // actually moved, so a refused directory is not reported as a change.
    bool navigate_to(std::string directory);
    void activate(int index, GameBrowserResult& result);
    // The ROM under the cursor, when there is one a forwarder can be made for.
    bool forwarder_target(std::string& path) const;
    void scroll_focus_into_view(const Geometry& geometry);
    float max_scroll(const Geometry& geometry) const;

    std::vector<Row> rows_;
    std::string directory_;
    std::string error_;
    int focus_ = 0;
    // How far down the list is scrolled, in pixels: a finger moves it by the
    // pixel, not a row at a time.
    float scroll_ = 0.0f;
    bool open_ = false;
    bool offers_forwarders_ = false;

    // A finger dragging the list, and the glide after it lets go.
    ListDrag drag_;

    // Where the prompts were drawn, so tapping one does what pressing it does.
    std::vector<GuideHit> guide_hits_;
};

}  // namespace ds
