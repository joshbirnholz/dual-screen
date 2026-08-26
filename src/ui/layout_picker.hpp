#pragma once

#include <string>
#include <vector>

#include "input/controller_mapping.hpp"
#include "input/cursor_visibility.hpp"
#include "input/input_manager.hpp"
#include "layout/layout_registry.hpp"
#include "render/button_glyphs.hpp"
#include "render/compositor.hpp"
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
    // Which hand holds the stylus was toggled, mirroring the handed layouts.
    bool handedness_changed = false;
    // The render scale was cycled. Like the pointer, it belongs to this device
    // alone -- it is a setting on the core, and only one device runs one.
    bool render_scale_changed = false;
    // Reset was chosen. Unlike the settings above there is no new value to
    // adopt -- the caller just restarts the game.
    bool reset_requested = false;
    // The gap toggle was used; the caller applies and persists it, the same
    // as filtering.
    bool screen_gap_toggled = false;
    // The right stick's mapping was cycled; same deal again.
    bool right_stick_mode_changed = false;
};

// Everything the menu needs to know about the state it is opening onto. A
// struct rather than a row of positional flags, because there are now enough of
// them that a call site would be unreadable.
struct LayoutPickerState {
    std::string layout_id;
    // Two devices are connected: adds Disconnect, and a sound target worth
    // choosing.
    bool session_active = false;
    bool bilinear_filtering = false;
    AudioTarget audio_target = AudioTarget::Both;
    CursorMode cursor_mode = CursorMode::Shown;
    // Whether this device has a pointer to show or hide at all.
    //
    // False on a companion client running on hardware with no mouse or
    // trackpad -- the Switch one, and whatever comes after it. It is the one
    // entry on this menu that belongs to the device rather than to the host,
    // which is exactly why it has nothing to say there, and an entry that
    // cannot do anything is worse than an absent one. Same rule as the render
    // scale and the right stick: offered where there is something to choose.
    bool cursor_choosable = true;
    Handedness handedness = Handedness::Right;
    // How many pixels the core renders per console pixel, and the largest this
    // system offers. A maximum of 1 means there is nothing to choose and the
    // button does not appear.
    int render_scale = 1;
    int max_render_scale = 1;
    // Whether the current layout shows the gap between its screens. Only
    // meaningful -- and only offered -- while a layout that has one is
    // selected, which the picker reads off the registry for itself.
    bool screen_gap = true;
    // What the right stick is read as, and whether that is a question at all:
    // only a console with a stick of its own has more than one answer, so a DS
    // does not get the button. See RightStickMode.
    RightStickMode right_stick_mode = RightStickMode::CStick;
    bool right_stick_choosable = false;
};

// The layout menu: a grid of visual previews rather than a cycle-through
// button, so a player picks the arrangement they want directly.
class LayoutPicker {
public:
    explicit LayoutPicker(const LayoutRegistry& registry) : registry_(&registry) {}

    void open(const LayoutPickerState& state);
    // Whether a confirmation is up, which the app needs to know: while one is,
    // the menu is not what the player is answering.
    bool awaiting_confirmation() const { return confirm_ != Confirm::None; }
    // Kept in step while the menu is open: R3 can change filtering from under
    // us, and on a companion both of these are owned by the host and arrive
    // whenever it says so.
    void set_filtering(bool bilinear) { state_.bilinear_filtering = bilinear; }
    void set_audio_target(AudioTarget target) { state_.audio_target = target; }
    void set_cursor_mode(CursorMode mode) { state_.cursor_mode = mode; }
    void set_handedness(Handedness handedness) { state_.handedness = handedness; }
    void set_render_scale(int scale) { state_.render_scale = scale; }
    void set_screen_gap(bool include_gap) { state_.screen_gap = include_gap; }
    void set_right_stick_mode(RightStickMode mode) { state_.right_stick_mode = mode; }
    // The console artwork, drawn inside the Standard layout's tile.
    void set_bezel_art(const Texture* art) { bezel_art_ = art; }
    void set_smoothing(bool smooth) { smoothing_ = smooth; }
    // The display the previews are judged against, when that is not the size
    // being drawn at -- see Compositor::set_display_size(). A tile has to show
    // the margin the real render would have, which is decided by the panel.
    // `fill` travels with the size because it answers the same question --
    // how much of the display the screens should take -- and a tile that
    // disagreed with the render behind it would be showing the wrong thing.
    void set_display_size(float width, float height, bool fill = false) {
        display_width_ = width;
        display_height_ = height;
        fill_display_ = fill;
    }
    // The console being played, which decides the screens' shapes and what the
    // Standard tile draws.
    void set_screens(const SystemScreens& screens) { screens_ = screens; }
    Handedness handedness() const { return state_.handedness; }
    // The host can change the layout under a companion's open menu.
    void set_selected_layout(const std::string& layout_id);
    bool filtering() const { return state_.bilinear_filtering; }
    AudioTarget audio_target() const { return state_.audio_target; }
    bool is_open() const { return open_; }
    void close() { open_ = false; }

    LayoutPickerResult update(const std::vector<UiAction>& actions,
                              const std::vector<PointerEvent>& pointers,
                              float output_width,
                              float output_height);

    // `screens` are the frames the game is paused on, drawn into the previews so
    // a tile shows the actual game in that arrangement. Null falls back to the
    // coloured blocks.
    void render(QuadRenderer& quads,
                TextRenderer& text,
                const ButtonGlyphSheet& glyphs,
                float output_width,
                float output_height,
                const std::string& game_title,
                const ScreenTextures* textures = nullptr);

    const std::string& selected_layout_id() const { return selected_layout_id_; }

    // The layout a tile actually draws: `layout` with this device's handedness
    // and the current gap choice applied, the same as the game itself would
    // be drawn with. Exposed so a preview -- and a test -- do not have to
    // reimplement the combination; render() uses this for every tile, which is
    // what makes toggling Gap or Hands update the previews immediately.
    Layout resolved_layout_for_preview(const Layout& layout) const;

    // Draws a layout at a glance: the game's own screens where there are frames
    // to draw, two coloured rectangles where there are not. `display_width`/
    // `display_height` are the real output size the menu itself is drawn at --
    // not `box`, which is only this one tile -- so a preview shows the margin
    // (or lack of it) the real layout would actually have.
    static void draw_preview(QuadRenderer& quads,
                             const Layout& layout,
                             const PixelRect& box,
                             float display_width,
                             float display_height,
                             const SystemScreens& geometry,
                             const ScreenTextures* textures,
                             const Texture* bezel_art,
                             bool smoothing);

private:
    // Footer buttons, in focus order after the layout tiles. Which of them are
    // present depends on the session, so they are built per-open rather than
    // being a fixed enum.
    enum class Footer {
        RenderScale,
        Hands,
        Sound,
        Cursor,
        RightStick,
        ScreenGap,
    };

    // What a face button asked for and has not yet been allowed to do. Both of
    // these throw away something the player cannot get back -- unsaved progress,
    // or the session -- so both are asked about first.
    enum class Confirm { None, Reset, Quit };

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
    // Where the confirmation sits, and the two prompts inside it a finger can
    // land on. Only meaningful while one is up.
    struct ConfirmGeometry {
        PixelRect dialog;
        PixelRect accept;
        PixelRect dismiss;
    };
    ConfirmGeometry compute_confirm_geometry(float output_width, float output_height) const;
    bool confirming() const { return confirm_ != Confirm::None; }
    int tile_count() const { return static_cast<int>(registry_->size()); }
    int footer_count() const { return static_cast<int>(footer_.size()); }
    int focus_count() const { return tile_count() + footer_count(); }
    void move_focus(int delta_x, int delta_y, const Geometry& geometry);
    bool activate_focus(LayoutPickerResult& result);

    std::string footer_label(Footer kind) const;
    // Which buttons the footer carries depends on the session, so it is built
    // per-open rather than fixed.
    void rebuild_footer();

    const LayoutRegistry* registry_;
    bool open_ = false;
    int focus_ = 0;
    std::string selected_layout_id_;
    std::vector<Footer> footer_;
    LayoutPickerState state_;
    const Texture* bezel_art_ = nullptr;
    bool smoothing_ = true;
    Confirm confirm_ = Confirm::None;
    float display_width_ = 0.0f;
    float display_height_ = 0.0f;
    bool fill_display_ = false;
    SystemScreens screens_;
};

}  // namespace ds
