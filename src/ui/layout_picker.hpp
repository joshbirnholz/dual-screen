#pragma once

#include <string>
#include <vector>

#include "input/controller_mapping.hpp"
#include "input/cursor_visibility.hpp"
#include "input/input_manager.hpp"
#include "layout/layout_registry.hpp"
#include "render/text_renderer.hpp"
#include "ui/ui_theme.hpp"

namespace ds {

struct LayoutPickerResult {
    bool close_requested = false;
    bool selection_changed = false;  // the layout changed; worth persisting

    bool quit_requested = false;
    // Ends the two-device session. Offered on both devices, since either end
    // should be able to stop it.
    bool disconnect_requested = false;
    // The filtering toggle was used; the caller applies and persists it.
    bool filtering_toggled = false;
    // The sound target was cycled; same deal.
    bool audio_target_changed = false;
    // The pointer mode was cycled. Unlike the two above it belongs to this
    // device alone -- the other one has its own pointer, or none.
    bool cursor_mode_changed = false;
};

// Everything the menu needs to know about the state it is opening onto. A
// struct rather than a row of positional flags, because there are now enough of
// them that a call site would be unreadable.
struct LayoutPickerState {
    std::string layout_id;
    // Two devices are connected: adds Disconnect, and a sound target worth
    // choosing.
    bool session_active = false;
    // This device is the companion. It has no game of its own to quit out of,
    // so leaving the session is the only exit that means anything.
    bool companion = false;
    bool bilinear_filtering = false;
    AudioTarget audio_target = AudioTarget::Both;
    CursorMode cursor_mode = CursorMode::Shown;
};

// The layout menu: a grid of visual previews rather than a cycle-through
// button, so a player picks the arrangement they want directly.
class LayoutPicker {
public:
    explicit LayoutPicker(const LayoutRegistry& registry) : registry_(&registry) {}

    void open(const LayoutPickerState& state);
    // Kept in step while the menu is open: R3 can change filtering from under
    // us, and on a companion both of these are owned by the host and arrive
    // whenever it says so.
    void set_filtering(bool bilinear) { state_.bilinear_filtering = bilinear; }
    void set_audio_target(AudioTarget target) { state_.audio_target = target; }
    void set_cursor_mode(CursorMode mode) { state_.cursor_mode = mode; }
    // The host can change the layout under a companion's open menu.
    void set_selected_layout(const std::string& layout_id) { selected_layout_id_ = layout_id; }
    bool filtering() const { return state_.bilinear_filtering; }
    AudioTarget audio_target() const { return state_.audio_target; }
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
    // Footer buttons, in focus order after the layout tiles. Which of them are
    // present depends on the session, so they are built per-open rather than
    // being a fixed enum.
    enum class Footer { Filtering, Sound, Cursor, Resume, Disconnect, Quit };

    struct Geometry {
        std::vector<PixelRect> tiles;
        std::vector<PixelRect> footer;
        PixelRect panel;
        int columns = 1;
        // The footer wraps onto a second row rather than squeezing six buttons
        // into one: a label nobody can read is not a shortcut.
        int footer_columns = 1;
        float scale = 1.0f;
    };

    Geometry compute_geometry(float output_width, float output_height) const;
    int tile_count() const { return static_cast<int>(registry_->size()); }
    int footer_count() const { return static_cast<int>(footer_.size()); }
    int focus_count() const { return tile_count() + footer_count(); }
    void move_focus(int delta_x, int delta_y, const Geometry& geometry);
    bool activate_focus(LayoutPickerResult& result);

    std::string footer_label(Footer kind) const;

    const LayoutRegistry* registry_;
    bool open_ = false;
    int focus_ = 0;
    std::string selected_layout_id_;
    std::vector<Footer> footer_;
    LayoutPickerState state_;
};

}  // namespace ds
