#pragma once

#include <string>
#include <vector>

#include "input/input_manager.hpp"
#include "input/pad_routing.hpp"
#include "render/button_glyphs.hpp"
#include "render/quad_renderer.hpp"
#include "render/text_renderer.hpp"

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
// To tell which row is which, press the north face button: the controller on
// the focused row rumbles.
class ControllersScreen {
public:
    void open();
    void close() {
        open_ = false;
        holding_ = false;
    }
    bool is_open() const { return open_; }

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

    // `wii_u` is whether the game is one this applies to, which is said on
    // screen when it is not rather than hiding the screen.
    void render(QuadRenderer& quads,
                TextRenderer& text,
                const ButtonGlyphSheet& glyphs,
                float output_width,
                float output_height,
                const std::vector<KnownPad>& pads,
                bool gamepad_here,
                const PadOrder& order,
                bool wii_u);

    int focus() const { return focus_; }
    bool holding() const { return holding_; }

private:
    struct Geometry {
        std::vector<PixelRect> rows;
        PixelRect panel;
        float scale = 1.0f;
        float view_top = 0.0f;
        float view_bottom = 0.0f;
        float content_height = 0.0f;
    };

    Geometry compute_geometry(float output_width, float output_height, int row_count) const;
    void scroll_focus_into_view(float output_width, float output_height, int row_count);

    int focus_ = 0;
    bool open_ = false;
    // Whether a controller is picked up, and which: by identity, so it stays
    // in hand while the rows move around it.
    bool holding_ = false;
    std::string held_;
    float scroll_ = 0.0f;
};

}  // namespace ds
