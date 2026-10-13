#pragma once

#include <optional>
#include <set>
#include <string>
#include <vector>

#include "core/external_frame_link.hpp"
#include "input/input_manager.hpp"
#include "render/button_glyphs.hpp"
#include "render/quad_renderer.hpp"
#include "render/text_renderer.hpp"
#include "ui/control_guide.hpp"
#include "ui/list_touch.hpp"
#include "ui/sound_view.hpp"

namespace ds {

struct GraphicPacksScreenResult {
    bool close_requested = false;
    // A pack turned on or off, or a preset chosen: for Cemu, which answers
    // with the whole list again -- see set_packs().
    std::optional<ExternalGraphicPackChange> change;
};

// What the page lists, top to bottom: a heading for each group of packs (the
// folders Cemu's own Graphic Packs window shows them in, less the game's
// name), each pack under it, and under a pack that is on, its presets. Only
// the packs and presets take the cursor.
struct GraphicPackLine {
    enum class Kind { Heading, Pack, Choice } kind = Kind::Heading;
    std::string text;
    std::size_t pack = 0;
    std::size_t choice = 0;
    bool focusable() const { return kind != Kind::Heading; }
};
std::vector<GraphicPackLine> graphic_pack_lines(const std::vector<ExternalGraphicPack>& packs);

// The running Wii U game's graphics packs, from the quick menu: each turned on
// and off, and its presets chosen, with a controller -- what Cemu's Graphic
// Packs window does, which in Game Mode is out of reach. The packs are Cemu's;
// this only shows what Cemu last said and asks it for changes, showing each
// as made rather than waiting to hear it back.
class GraphicPacksScreen {
public:
    // Up, and waiting for Cemu's list.
    void open();
    void close() { open_ = false; }
    bool is_open() const { return open_; }
    // Cemu's list, as it is now: the cursor stays on the pack or preset it
    // was on.
    void set_packs(std::vector<ExternalGraphicPack> packs);
    // Whether a paused game is behind it, to be dimmed rather than covered.
    void set_over_game(bool over_game) { over_game_ = over_game; }

    GraphicPacksScreenResult update(const std::vector<UiAction>& actions,
                                    const std::vector<PointerEvent>& pointers, float output_width,
                                    float output_height);
    void render(QuadRenderer& quads, TextRenderer& text, const ButtonGlyphSheet& glyphs,
                float output_width, float output_height);
    void describe_for_sound(UiSoundView& view) const;

    int focus() const { return focus_; }
    const std::vector<ExternalGraphicPack>& packs() const { return packs_; }
    // Whether a change was made to a pack that only takes effect once the
    // game starts again -- which the page then says.
    bool restart_pending() const { return restart_pending_; }

private:
    struct Geometry {
        PixelRect panel;
        float scale = 1.0f;
        float view_top = 0.0f;
        float view_height = 0.0f;
        float content_height = 0.0f;
        // One per line, headings included.
        std::vector<PixelRect> lines;
        float max_scroll() const { return std::max(0.0f, content_height - view_height); }
    };
    Geometry compute_geometry(float output_width, float output_height) const;
    void rebuild_lines();
    // The line the cursor is on, or null.
    const GraphicPackLine* focused_line() const;
    int focusable_count() const;
    // The index in lines_ of the focusable line `focus`.
    int line_of_focus(int focus) const;
    void scroll_focus_into_view(const Geometry& geometry);
    // Turns the focused pack on or off, or steps its focused preset
    // `direction` places.
    void adjust(int direction, GraphicPacksScreenResult& result);
    std::string line_value(const GraphicPackLine& line) const;

    bool open_ = false;
    bool waiting_ = true;
    bool over_game_ = false;
    bool restart_pending_ = false;
    // The packs changed since the page opened. Cemu only knows a pack needs
    // the game started again once it has read the pack, which a pack that
    // was off it has not, so its answer can say so after the change.
    std::set<std::string> changed_;
    std::vector<ExternalGraphicPack> packs_;
    std::vector<GraphicPackLine> lines_;
    int focus_ = 0;
    float scroll_ = 0.0f;
    ListDrag drag_;
    std::vector<GuideHit> guide_hits_;
};

}  // namespace ds
