#pragma once

#include <string>
#include <vector>

#include "input/controller_mapping.hpp"
#include "input/input_manager.hpp"
#include "layout/layout_registry.hpp"
#include "render/text_renderer.hpp"
#include "ui/ui_theme.hpp"

namespace ds {

struct LayoutPickerResult {
    bool close_requested = false;
    bool selection_changed = false;  // the layout changed; worth persisting
    bool open_settings = false;
    bool quit_requested = false;
};

// The layout menu: a grid of visual previews rather than a cycle-through
// button, so a player picks the arrangement they want directly.
class LayoutPicker {
public:
    explicit LayoutPicker(const LayoutRegistry& registry) : registry_(&registry) {}

    void open(const std::string& layout_id);
    bool is_open() const { return open_; }
    void close() { open_ = false; }

    LayoutPickerResult update(const std::vector<UiAction>& actions,
                              const std::vector<PointerEvent>& pointers,
                              float output_width,
                              float output_height);

    void render(QuadRenderer& quads,
                TextRenderer& text,
                float output_width,
                float output_height,
                const std::string& game_title,
                const MenuButtonLabels& labels);

    const std::string& selected_layout_id() const { return selected_layout_id_; }

    // Draws a layout as two coloured rectangles arranged per its placements.
    // Shared with any other screen that wants to show a layout at a glance.
    static void draw_preview(QuadRenderer& quads, const Layout& layout, const PixelRect& box);

private:
    // Footer buttons, in focus order after the layout tiles.
    enum class Footer { Settings, Resume, Quit, Count };

    struct Geometry {
        std::vector<PixelRect> tiles;
        std::vector<PixelRect> footer;
        PixelRect panel;
        int columns = 1;
        float scale = 1.0f;
    };

    Geometry compute_geometry(float output_width, float output_height) const;
    int tile_count() const { return static_cast<int>(registry_->size()); }
    int focus_count() const { return tile_count() + static_cast<int>(Footer::Count); }
    void move_focus(int delta_x, int delta_y, const Geometry& geometry);
    bool activate_focus(LayoutPickerResult& result);

    const LayoutRegistry* registry_;
    bool open_ = false;
    int focus_ = 0;
    std::string selected_layout_id_;
};

}  // namespace ds
