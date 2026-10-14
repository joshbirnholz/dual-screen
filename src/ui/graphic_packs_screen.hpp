#pragma once

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "core/external_frame_link.hpp"
#include "net/protocol.hpp"
#include "input/input_manager.hpp"
#include "render/button_glyphs.hpp"
#include "render/quad_renderer.hpp"
#include "render/text_renderer.hpp"
#include "ui/choice_popup.hpp"
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

// The left-hand list, top to bottom: a heading for each group of packs (the
// folders Cemu's own Graphic Packs window files them in, less the game's
// name), and each pack under it. Only the packs take the cursor.
struct GraphicPackLine {
    enum class Kind { Heading, Pack } kind = Kind::Heading;
    std::string text;
    std::size_t pack = 0;
    bool focusable() const { return kind == Kind::Pack; }
};
std::vector<GraphicPackLine> graphic_pack_lines(const std::vector<ExternalGraphicPack>& packs);

// The running Wii U game's graphics packs, from the quick menu, laid out as
// both that menu and Cemu's own Graphic Packs window are: the packs listed on
// the left, and on the right the one highlighted there -- on or off, its
// presets, what it says about itself. Moving between the two goes as it does
// in the quick menu: Right or A into the pack, Left or B back to the list.
//
// The packs are Cemu's; this only shows what Cemu last said and asks it for
// changes, showing each as made rather than waiting to hear it back.
class GraphicPacksScreen {
public:
    // Up, and waiting for Cemu's list.
    void open();
    void close() { open_ = false; }
    bool is_open() const { return open_; }
    // Cemu's list, as it is now: the cursor stays on the pack, and the row in
    // it, that it was on.
    void set_packs(std::vector<ExternalGraphicPack> packs);
    // Whether a paused game is behind it, to be dimmed rather than covered.
    void set_over_game(bool over_game) { over_game_ = over_game; }

    GraphicPacksScreenResult update(const std::vector<UiAction>& actions,
                                    const std::vector<PointerEvent>& pointers, float output_width,
                                    float output_height);
    void render(QuadRenderer& quads, TextRenderer& text, const ButtonGlyphSheet& glyphs,
                float output_width, float output_height);
    void describe_for_sound(UiSoundView& view) const;

    // Which pack is highlighted in the list, counting packs only.
    int focus() const { return focus_; }
    // Whether the cursor is over on the right, in that pack, and on which of
    // its rows: 0 turns it on and off, and each preset choice follows.
    bool in_pack() const { return in_pack_; }
    int item() const { return item_; }
    const std::vector<ExternalGraphicPack>& packs() const { return packs_; }
    // Counts up whenever the packs, or anything about them, change: what the
    // other device is sent is only made again when it has.
    uint64_t packs_revision() const { return packs_revision_; }
    // Whether a change was made to a pack that only takes effect once the
    // game starts again -- which the page then says.
    bool restart_pending() const { return restart_pending_; }
    bool waiting() const { return waiting_; }
    // How far each side is scrolled, in pixels.
    float list_scroll() const { return scroll_; }
    float detail_scroll() const { return detail_scroll_; }
    // A preset's choices, up beside its row, and the one the cursor is on.
    bool choosing() const { return choice_.is_open(); }
    int choice_focus() const { return choice_.focus(); }
    // Where the prompts at the foot of the page were drawn -- see
    // take_guide_taps() -- for a device drawing the other's page.
    const std::vector<GuideHit>& guide_hits() const { return guide_hits_; }

    // The other device's page, drawn from what it said -- see
    // GraphicPacksViewMessage: its packs, its cursor, scrolled to show the
    // cursor here, and nothing changed here. Opens the page if it is not.
    void show_mirrored(const net::GraphicPacksViewMessage& view, float output_width,
                       float output_height);
    // A finger dragging either side of the page, which scrolls it -- on
    // the device drawing the other's page too, where a finger has nothing
    // else here to do. See update().
    // True when this pass's release ended a drag rather than tapped. A
    // preset's popup, drawn from the other device's, scrolls the same way.
    bool drag(const std::vector<PointerEvent>& pointers, float output_width, float output_height);

    // For a device drawing the other's page: what is under a point, as an
    // entry to tell the other device -- see press_entry() -- or -1.
    int entry_at(float x, float y, float output_width, float output_height) const;
    // Whether a point is off an open preset popup altogether.
    bool off_choice_popup(float x, float y, float output_width, float output_height) const;
    // A tap the other device resolved to an entry, answered as a tap on that
    // entry here would be: a first tap highlights, a second chooses; a
    // popup's choice is chosen at once.
    GraphicPacksScreenResult press_entry(int entry);
    // Entries: a pack in the list is its index times two, a row of the pack
    // shown is its index times two plus one, and a popup's choice is this
    // plus its index.
    static constexpr int kChoiceEntryBase = 0x8000;

private:
    struct Geometry {
        PixelRect panel;
        float scale = 1.0f;
        float line = 0.0f;
        float view_top = 0.0f;
        float view_height = 0.0f;
        float divider_x = 0.0f;
        // The left-hand list: one rect per line, headings included, and how
        // tall it all is against the room it has.
        PixelRect list;
        std::vector<PixelRect> lines;
        float content_height = 0.0f;
        // The right-hand side: the pack's name, its rows, and under them
        // what it says about itself.
        PixelRect detail;
        float title_y = 0.0f;
        std::vector<PixelRect> items;
        float description_y = 0.0f;
        // How tall the pack's side is, title to the end of what it says.
        float detail_height = 0.0f;
        float max_scroll() const { return std::max(0.0f, content_height - view_height); }
        float detail_max_scroll() const { return std::max(0.0f, detail_height - view_height); }
    };
    Geometry compute_geometry(float output_width, float output_height) const;
    void rebuild_lines() { lines_ = graphic_pack_lines(packs_); }
    int pack_count() const;
    // The index in lines_ of the `focus`th pack, and the pack it is.
    int line_of_focus(int focus) const;
    ExternalGraphicPack* focused_pack();
    const ExternalGraphicPack* focused_pack() const;
    // Rows on the right for the focused pack: on/off, then one per choice.
    int item_count() const;
    void scroll_focus_into_view(const Geometry& geometry);
    // The pack's side brought round the row the cursor is on.
    void scroll_item_into_view(const Geometry& geometry);
    // Turns the focused pack on or off, or steps its focused preset on.
    void adjust(GraphicPacksScreenResult& result);
    // The focused preset's choices up beside its row.
    void open_choice(const Geometry& geometry);
    // The focused preset set to its `index`th choice.
    void choose(int index, GraphicPacksScreenResult& result);
    std::string item_label(int item) const;
    std::string item_value(int item) const;

    bool open_ = false;
    bool waiting_ = true;
    bool over_game_ = false;
    bool restart_pending_ = false;
    // The packs changed since the page opened. Cemu only knows a pack needs
    // the game started again once it has read the pack, which a pack that
    // was off it has not, so its answer can say so after the change.
    std::set<std::string> changed_;
    std::vector<ExternalGraphicPack> packs_;
    uint64_t packs_revision_ = 0;
    std::vector<GraphicPackLine> lines_;
    // How tall the highlighted pack's description is, wrapped: wrapping is
    // measuring every word, and the layout is asked for many times a frame.
    struct DescriptionHeight {
        std::string text;
        float width = -1.0f;
        float scale = -1.0f;
        float height = 0.0f;
    };
    mutable DescriptionHeight description_height_;
    float description_height(const std::string& description, float width, float scale) const;
    // Whether the other device's packs are the ones already here.
    bool same_packs(const std::vector<net::GraphicPacksViewPack>& wire) const;
    int focus_ = 0;
    bool in_pack_ = false;
    int item_ = 0;
    float scroll_ = 0.0f;
    ListDrag drag_;
    // The pack's side, scrolled, when it says more than fits.
    float detail_scroll_ = 0.0f;
    ListDrag detail_drag_;
    // What the other device's page last said about where its cursor is: a
    // page drawn from it follows that cursor only when it moves, and is
    // otherwise where a finger here left it.
    int mirrored_focus_ = -1;
    bool mirrored_in_pack_ = false;
    int mirrored_item_ = -1;
    ChoicePopup choice_;
    float last_width_ = 1280.0f;
    float last_height_ = 800.0f;
    std::vector<GuideHit> guide_hits_;
};

// The other device's page, drawn here, under a finger, the mouse or the
// wheel: scrolled here, as this device's own view of it, and each tap turned
// into the entry it landed on -- see GraphicPacksScreen::press_entry() -- for
// the other device to answer, or kMirroredTapCancel for a tap off an open
// popup, which puts it away.
inline constexpr int kMirroredTapCancel = -2;
std::vector<int> mirrored_graphic_pack_taps(GraphicPacksScreen& page,
                                            const std::vector<PointerEvent>& pointers,
                                            float output_width, float output_height);

// The Graphics Packs page as a message for the other device.
net::GraphicPacksViewMessage graphic_packs_view_of(const GraphicPacksScreen& screen);

// A whole frame of the other device's Graphics Packs page -- what a companion
// shows while the host has it up. `screen` is one kept for this alone.
// `over_game`: drawn over a game the caller has already put on screen,
// darkened, the way the host draws it, rather than on the app's background.
void render_mirrored_graphic_packs(GraphicPacksScreen& screen, QuadRenderer& quads,
                                   TextRenderer& text, const ButtonGlyphSheet& glyphs,
                                   float output_width, float output_height,
                                   const net::GraphicPacksViewMessage& view, bool over_game = false);

}  // namespace ds
