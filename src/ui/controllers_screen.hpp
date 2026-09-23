#pragma once

#include <map>
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
    // A choice was made or undone, so where the pads go has changed.
    bool choices_changed = false;
};

// Which controller is the GamePad and which is each player, on a Wii U -- see
// pad_routing.hpp for how that is decided when nobody has said.
//
// One row per controller on either device, numbered in that device's own
// order (Steam Input's, under Steam) and never named: a name Steam gave a
// virtual pad when it appeared may no longer describe the controller behind
// it once pads have come, gone and been reordered. To tell which row is which,
// press something -- the row it belongs to lights up.
//
// Choices are the caller's map, keyed by pad_key(), and last for the session.
class ControllersScreen {
public:
    void open();
    void close() { open_ = false; }
    bool is_open() const { return open_; }

    // `pads` is every controller as it stands this frame, this device's first;
    // `gamepad_here` whether this device is the one holding the GamePad. Both
    // are asked for every frame because either can change while the screen
    // is up -- a pad plugged in, a companion joining.
    ControllersScreenResult update(const std::vector<UiAction>& actions,
                                   const std::vector<PointerEvent>& pointers,
                                   float output_width,
                                   float output_height,
                                   const std::vector<KnownPad>& pads,
                                   bool gamepad_here,
                                   std::map<std::string, std::string>& choices);

    // `wii_u` is whether the game is one these choices apply to, which is
    // said on screen when it is not rather than hiding the screen: the pads
    // are still worth seeing.
    void render(QuadRenderer& quads,
                TextRenderer& text,
                const ButtonGlyphSheet& glyphs,
                float output_width,
                float output_height,
                const std::vector<KnownPad>& pads,
                bool gamepad_here,
                const std::map<std::string, std::string>& choices,
                bool wii_u);

    int focus() const { return focus_; }

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
    // Steps the focused pad to the next slot along, or the one before.
    // Returns whether anything changed.
    bool adjust(int direction, const std::vector<KnownPad>& pads, bool gamepad_here,
                std::map<std::string, std::string>& choices, ControllersScreenResult& result);

    int focus_ = 0;
    bool open_ = false;
    float scroll_ = 0.0f;
};

// Whether a pad is being touched right now: a button held or a stick pushed
// well off centre. What lights its row.
bool pad_in_use(const DsInputState& held);

}  // namespace ds
