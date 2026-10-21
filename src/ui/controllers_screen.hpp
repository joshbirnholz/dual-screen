#pragma once

#include <string>
#include <vector>

#include "input/input_manager.hpp"
#include "input/pad_routing.hpp"
#include "net/protocol.hpp"
#include "render/button_glyphs.hpp"
#include "render/quad_renderer.hpp"
#include "render/text_renderer.hpp"
#include "render/texture.hpp"
#include "ui/control_guide.hpp"
#include "ui/list_touch.hpp"
#include "ui/menu_page.hpp"
#include "ui/sound_view.hpp"

namespace ds {

struct ControllersScreenResult {
    bool close_requested = false;
    // A pad moved, or the line went back to the defaults.
    bool order_changed = false;
    // A pad to buzz, so the player can tell which one it is: an index into the
    // pads handed in, or -1.
    int identify = -1;
};

// Which controller is the GamePad and which is each player, on a Wii U -- the
// line pad_routing.hpp describes, rearranged the way SteamOS's own Controller
// Order screen rearranges Steam's: pick a controller up with the confirm
// button, move it up and down, put it down again. The first place is the
// GamePad.
//
// To tell which row is which, press either upper face button -- X or Y, which
// sit the other way round on a Nintendo pad from an Xbox one -- and the
// controller on the focused row gives a short buzz.
class ControllersScreen : public MenuPage {
public:
    // Lets go of its icons before the GL context they live in does; they are
    // loaded again the next time they are drawn.
    void release_textures() {
        icons_.destroy();
        icons_tried_ = false;
    }
    // What a press here could have changed -- see UiSoundJudge.
    void describe_for_sound(UiSoundView& view) const;
    void open();
    void close() {
        open_ = false;
        holding_ = false;
    }
    // Where its prompts were last drawn -- see take_guide_taps(). For a
    // device showing another's Controller Order, whose taps go over there.
    const std::vector<GuideHit>& guide_hits() const { return guide_hits_; }
    bool is_open() const { return open_; }
    // Over a paused game, which shows through dimmed; otherwise on the app's
    // own background. See draw_page_background().
    void set_over_game(bool over_game) { over_game_ = over_game; }

    // `pads` is every controller as it stands this frame; `gamepad_here`
    // whether this device is the one holding the GamePad. Both are asked for
    // every frame because either can change while the screen is up -- a pad
    // plugged in, a companion joining. `order` is the caller's, kept for the
    // session.
    ControllersScreenResult update(const std::vector<UiAction>& actions,
                                   const std::vector<PointerEvent>& pointers,
                                   float output_width,
                                   float output_height,
                                   const std::vector<KnownPad>& pads,
                                   bool gamepad_here,
                                   PadOrder& order);

    // `this_device` and `other_device` are what each row says its pad is
    // plugged into.
    void render(QuadRenderer& quads,
                TextRenderer& text,
                const ButtonGlyphSheet& glyphs,
                float output_width,
                float output_height,
                const std::vector<KnownPad>& pads,
                bool gamepad_here,
                const PadOrder& order,
                const std::string& this_device,
                const std::string& other_device);

    int focus() const { return focus_; }
    bool holding() const { return holding_; }

    // The other device's screen, drawn from what it said -- see
    // ControllersViewMessage: the cursor where it has it, scrolled to show it
    // here, and nothing run here. Opens the screen if it is not.
    void show_mirrored(int focus, bool holding, float output_width, float output_height,
                       const std::vector<KnownPad>& pads, bool gamepad_here,
                       const PadOrder& order);

    // MenuPage: the list scrolls under a finger, the mouse and the wheel --
    // on the device drawing the other's screen too, as its own view of it --
    // and an entry is a row, by its index.
    bool scroll(const std::vector<PointerEvent>& pointers, float output_width,
                float output_height) override;
    int entry_at(float x, float y, float output_width, float output_height) const override;
    bool off_popup(float, float, float, float) const override { return false; }
    // The list, in lines.
    std::vector<float> scroll_state(float output_width, float output_height) const override;
    void set_scroll_state(const std::vector<float>& state, float output_width, float output_height) override;

private:
    struct Geometry {
        std::vector<PixelRect> rows;
        PixelRect panel;
        float scale = 1.0f;
        float view_top = 0.0f;
        float view_bottom = 0.0f;
        float content_height = 0.0f;
    };

    // `gap_before` is the first row of Not used, which is set a little apart
    // from the controllers in play; -1 for none.
    Geometry compute_geometry(float output_width, float output_height, int row_count,
                              int gap_before) const;
    void scroll_focus_into_view(float output_width, float output_height, int row_count,
                                int gap_before);

    int focus_ = 0;
    bool open_ = false;
    bool over_game_ = false;
    // Whether a controller is picked up, and which: by identity, so it stays
    // in hand while the rows move around it.
    // The GamePad's icon, from the menus' icon strip -- see ui/ui_icons.hpp.
    Texture icons_;
    bool icons_tried_ = false;
    bool holding_ = false;
    std::string held_;
    float scroll_ = 0.0f;
    ListDrag drag_;
    ScrollEase scroll_ease_;
    // How many rows the list had when last laid out, and the first of Not
    // used -- what a point is on, and how far the list scrolls, follow them.
    int row_count_ = 0;
    int gap_before_ = -1;
    // The other device's cursor as last drawn here: the view follows it only
    // when it moves, and is otherwise where a finger here left it.
    int mirrored_focus_ = -1;
    bool mirrored_holding_ = false;
    // Where the prompts at the foot of the page were drawn -- see take_guide_taps().
    std::vector<GuideHit> guide_hits_;
};

// The Controller Order screen as a message for the other device, and back.
net::ControllersViewMessage controllers_view_of(const ControllersScreen& screen,
                                                const std::vector<KnownPad>& pads,
                                                bool gamepad_here, const PadOrder& order,
                                                const std::string& this_device,
                                                const std::string& other_device);
void unpack_controllers_view(const net::ControllersViewMessage& view,
                             std::vector<KnownPad>& pads, PadOrder& order);

// A whole frame of the other device's Controller Order screen, on this
// device's own background -- what a companion shows while the host has it
// up. `screen` is a ControllersScreen kept for this alone.
// `over_game`: drawn over a game the caller has already put on screen,
// darkened, the way the host draws it -- rather than on the app's background.
void render_mirrored_controllers(ControllersScreen& screen, QuadRenderer& quads,
                                 TextRenderer& text, const ButtonGlyphSheet& glyphs,
                                 float output_width, float output_height,
                                 const net::ControllersViewMessage& view, bool over_game = false);

}  // namespace ds
