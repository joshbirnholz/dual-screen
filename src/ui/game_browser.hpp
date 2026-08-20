#pragma once

#include <string>
#include <vector>

#include "config/paths.hpp"
#include "input/controller_mapping.hpp"
#include "input/input_manager.hpp"
#include "render/text_renderer.hpp"
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
    // Back to where the browser was opened from, rather than up a folder.
    bool back_requested = false;
    bool quit_requested = false;
};

// A file browser for picking a ROM, used when the app is launched with no
// --game. Navigable with a controller and by touch, including drag scrolling,
// because on a Deck in Game Mode both are the only inputs there are.
class GameBrowser {
public:
    void open(const std::string& directory);
    void close() { open_ = false; }
    bool is_open() const { return open_; }

    const std::string& current_directory() const { return directory_; }

    GameBrowserResult update(const InputManager& input,
                             const std::vector<UiAction>& actions,
                             const std::vector<PointerEvent>& pointers,
                             float output_width,
                             float output_height);

    // `labels` only decides which letters the prompts print.
    void render(QuadRenderer& quads,
                TextRenderer& text,
                float output_width,
                float output_height,
                const MenuButtonLabels& labels);

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
        PixelRect back_button;
        PixelRect quit_button;
        float row_height = 0.0f;
        float scale = 1.0f;
        int visible_rows = 1;
    };

    Geometry compute_geometry(float output_width, float output_height) const;
    // By value on purpose: see the definition. Returns whether the browser
    // actually moved, so a refused directory is not reported as a change.
    bool navigate_to(std::string directory);
    void activate(int index, GameBrowserResult& result);
    void scroll_focus_into_view(int visible_rows);
    void clamp_scroll(int visible_rows);

    std::vector<Row> rows_;
    std::string directory_;
    std::string error_;
    int focus_ = 0;
    int scroll_ = 0;
    bool open_ = false;

    // Drag-scroll state: a press that moves more than a few pixels scrolls the
    // list instead of counting as a tap on whatever was under the finger.
    bool dragging_ = false;
    bool drag_moved_ = false;
    float drag_start_y_ = 0.0f;
    int drag_start_scroll_ = 0;
};

}  // namespace ds
