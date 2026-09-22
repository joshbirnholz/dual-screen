#pragma once

#include <string>
#include <vector>

#include "input/input_manager.hpp"
#include "render/button_glyphs.hpp"
#include "render/quad_renderer.hpp"
#include "render/text_renderer.hpp"

namespace ds {

// One "<button> <action>" pair in the row of prompts along the bottom of a
// screen. The button is drawn as the console's own symbol rather than spelled
// out, so a prompt says the same thing whatever letters the pad in the player's
// hands happens to have printed on it.
struct GuidePrompt {
    ButtonGlyph glyph = ButtonGlyph::A;
    std::string action;
    // True when the thing this prompt names can simply be tapped: the row in
    // a list, the tile in a grid, the entry in a browser.
    //
    // With a pad it earns its place -- something has to say which button
    // opens the thing under the cursor. With a finger it is a second way to
    // do what tapping the thing already does, and the worse of the two, since
    // the thing itself is a bigger target and is where the eye already is.
    //
    // Not every confirm is one of these. A message with nothing in it but
    // words has no object to tap, and the two halves of a confirmation dialog
    // are tappable *because* the prompts label them -- take those away and
    // what is left is an invisible target.
    bool redundant_on_touch = false;
};

// Where one prompt ended up, so a finger can land on it. Filled in while the
// row is drawn and hit-tested on the next press: the row is laid out from the
// glyph artwork's own proportions, which only the drawing side knows.
struct GuideHit {
    ButtonGlyph glyph = ButtonGlyph::A;
    PixelRect rect;
};

// Whether the prompts are for a pad or for fingers.
//
// Module state rather than an argument threaded through every render call,
// the same shape as ui_theme(): it is one fact about the machine the app is
// running on, it is the same for every screen, and it changes only when a
// controller is plugged in or pulled out.
//
// A console glyph is the right thing to draw when there is a pad in the
// player's hands and exactly the wrong thing otherwise: a phone on its own
// has no B button, so "B Back" is an instruction that cannot be followed.
// The prompts stay in the same row, in the same place, at the same size --
// they just say what a finger can actually do.
void set_touch_hints(bool touch);
bool touch_hints();

// Which prompt a point is on, ButtonGlyph::Count when none. The prompts are
// laid out from the glyph artwork's own proportions, so only the row that
// drew itself knows where they went -- hence the hits.
ButtonGlyph guide_button_at(const std::vector<GuideHit>& hits, float x, float y);

// What tapping a prompt should do. False for a glyph no prompt outside a
// quick menu uses, whose meaning belongs to the menu and is decided there.
//
// Here rather than at the one call site so that it can be checked: a prompt
// whose glyph this does not know is a prompt that draws, invites a finger and
// then does nothing -- which on a device with no buttons is the difference
// between an app you can leave and one you cannot.
bool ui_action_for_glyph(ButtonGlyph glyph, UiAction& out);

// The prompts worth drawing, given how this device is being used. With a pad,
// all of them; with a finger, all but the ones whose thing can be tapped.
//
// Separate from the drawing so that what is shown can be checked without a
// GL context -- which prompts appear is a design decision, not a detail of
// how they are painted.
std::vector<GuidePrompt> prompts_worth_drawing(const std::vector<GuidePrompt>& prompts);

// Draws the prompts as one row and returns how tall it was. `x` is the row's
// centre when centred and its right edge when right-aligned. `out_hits`, when
// given, receives where each prompt landed.
float draw_control_guide(QuadRenderer& quads,
                         TextRenderer& text,
                         const ButtonGlyphSheet& glyphs,
                         const std::vector<GuidePrompt>& prompts,
                         float x,
                         float y,
                         float scale,
                         const Color& color,
                         TextAlign align = TextAlign::Center,
                         std::vector<GuideHit>* out_hits = nullptr);

// How tall that row will be, for a caller reserving space before drawing.
float control_guide_height(float scale);

}  // namespace ds
