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
// Why a prompt is on screen, which is what decides whether a device that has
// the same action covered some other way still needs it drawn.
enum class PromptRole {
    // Names a button that does something nothing else on the screen does:
    // "+ Quit", "X Reset", or the one "A Continue" on a screen that is
    // nothing but words. Always drawn. The default, because a prompt that
    // nobody has thought about is a prompt that has to stay.
    OwnControl,
    // Activates whatever is highlighted -- the row in a list, the tile in a
    // grid, the entry in a browser.
    //
    // With a pad it earns its place: something has to say which button opens
    // the thing under the cursor. With a finger, or with a remote whose click
    // acts on what is already focused, it is a second way to do what the thing
    // itself already does, and the worse of the two, since the thing is a
    // bigger target and is where the eye already is.
    ActsOnSelection,
    // Leaves the screen: Back, Resume, up a level.
    //
    // A phone has no button for that, so the prompt is the only way out and
    // has to stay. A Siri Remote has one with a back arrow on it, and nobody
    // needs telling what it does.
    LeavesScreen,
};

struct GuidePrompt {
    ButtonGlyph glyph = ButtonGlyph::A;
    std::string action;
    PromptRole role = PromptRole::OwnControl;
    // A second button that does the same the other way, drawn after a slash:
    // "[L]/[R] Change Device". Count for none. A finger on the prompt means
    // the first.
    ButtonGlyph pair = ButtonGlyph::Count;
};

// Where one prompt ended up, so a finger can land on it. Filled in while the
// row is drawn and hit-tested on the next press: the row is laid out from the
// glyph artwork's own proportions, which only the drawing side knows.
struct GuideHit {
    ButtonGlyph glyph = ButtonGlyph::A;
    PixelRect rect;
};

// What is in the player's hands, as far as the prompts are concerned.
enum class PromptStyle {
    // A controller with a face-button cluster. Every prompt, drawn as the
    // console's own symbol.
    Pad,
    // A touchscreen and nothing else. Capsule chips with words in them,
    // because a phone has no B button and "B Back" is an instruction that
    // cannot be followed.
    Touch,
    // A Siri Remote on an Apple TV. Glyphs, because it does have buttons --
    // but not for select or for back, which are the two it is most obviously
    // going to do.
    Remote,
};

// Which prompts to draw, and how.
//
// Module state rather than an argument threaded through every render call,
// the same shape as ui_theme(): it is one fact about the machine the app is
// running on, it is the same for every screen, and it changes only when a
// controller is plugged in or pulled out. The prompts stay in the same row,
// in the same place, at the same size -- they just say what the thing in the
// player's hands can actually do.
void set_prompt_style(PromptStyle style);
PromptStyle prompt_style();
bool touch_hints();

// Which style suits a machine with this attached. Pure, and separate from the
// state above so that it can be checked without a window:
//
//   television      this device is an Apple TV, so there is nothing to tap and
//                   a remote in the box whatever else is paired
//   controllers     how many SDL is reporting at all
//   full_gamepads   how many of those have a full face-button cluster
//
// A Siri Remote is a controller as far as SDL is concerned, so counting
// controllers cannot tell it from a pad; what separates them is SDL's name for
// it, which is "Remote" -- see controller_is_a_remote(), and the measurement
// in its comment that replaced a capability test that did not work. Pair a pad
// with the Apple TV and the glyphs come back, which is the point: the hints
// are about what you are holding.
PromptStyle prompt_style_for(bool television, int controllers, int full_gamepads);

// And which button means yes, given that.
//
// A Siri Remote has one button on its face and its click arrives as SDL's
// south, so east -- this app's default, and every Nintendo pad's convention --
// names a button the remote does not have. Pure and here beside the style it
// follows from, rather than a ternary at the one call site, because getting it
// wrong is silent: the menu simply stops answering.
ConfirmButton confirm_button_for_style(PromptStyle style);

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

// Which prompt to draw for yes and for no, given which button was chosen to
// mean yes. Here rather than beside the buttons themselves: what a button
// does is input's business, what is drawn for it is this layer's.
ButtonGlyph confirm_glyph_for(ConfirmButton choice);
ButtonGlyph cancel_glyph_for(ConfirmButton choice);

// Which button means yes, for everything drawn from here on. Module state
// like the hint style above, and for the same reason: one choice for the
// whole app, and every prompt in it would otherwise have to carry it.
//
// A prompt says ButtonGlyph::A when it means "confirm", because that is the
// east button and east confirms by default. Choosing south swaps what is
// *drawn* for the pair and nothing else -- the prompt still means confirm,
// so a finger landing on it still confirms.
void set_confirm_button(ConfirmButton choice);
ConfirmButton confirm_button();

// What a prompt is drawn as, once that choice is applied. The identity for
// everything that is not one of the two.
ButtonGlyph glyph_as_drawn(ButtonGlyph glyph);

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
