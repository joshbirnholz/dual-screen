#pragma once

#include <string>

#include "render/quad_renderer.hpp"
#include "render/texture.hpp"

namespace ds {

// The controller buttons a control prompt names, drawn as the console's own
// symbols. Always the Nintendo positions -- the A that means "confirm" is the
// east button whatever the pad in the player's hands calls it -- because the
// prompt is naming the thing the game names, not the hardware.
// What a prompt is asking for, named by where the button is rather than by
// what any one pad prints on it. The names are the Nintendo letters because
// those were the first ones drawn, and they mean:
//
//     A  east      B  south      X  north      Y  west
//
// which is the whole of why a style can be chosen at all: the position is
// fixed and only the printing changes.
// L and R are the shoulder buttons, which every pad has in the same place and
// prints its own way: L and R, L1 and R1, LB and RB.
enum class ButtonGlyph { A, B, X, Y, LeftStick, RightStick, Plus, Minus, L, R, Count };

// Whose buttons the prompts are drawn as. The choice is the player's, not the
// pad's: somebody who has used a PlayStation for twenty years reads a circle
// faster than they read the letter that happens to be under their thumb, and
// somebody streaming to a phone has no pad under their thumb at all.
//
// Positional draws no letters at all: four dots with the one to press filled
// in, as Nintendo draws them where it will not say which letter is meant. For
// a pad whose printing disagrees with the prompt's -- a Switch pad read by
// position, a Steam Input layout that swapped things round -- the position is
// the one thing that is always right.
enum class PadGlyphStyle { Nintendo, PlayStation, Xbox, Positional, Count };

const char* to_string(PadGlyphStyle style);
bool pad_glyph_style_from_string(const std::string& text, PadGlyphStyle& out);

// The cells in assets/button_icons.png, in the order they sit in the strip.
// The first eight are the original hand-drawn artwork; the rest are generated
// by assets/make_button_icons.py to the geometry measured off them.
enum class GlyphCell {
    LetterA,
    LetterB,
    LetterX,
    LetterY,
    LeftStick,
    RightStick,
    Plus,
    Minus,
    Cross,
    Circle,
    Triangle,
    Square,
    // Three horizontal lines: an Xbox Menu button and a PlayStation Options
    // button are the same icon, so they are the same cell.
    Bars,
    View,
    Create,
    // Four dots, the one to press filled in -- see PadGlyphStyle::Positional.
    DotsEast,
    DotsSouth,
    DotsNorth,
    DotsWest,
    // The shoulders, from Kenney's Input Prompts: a Switch's, a Steam
    // Controller's L1 and R1 -- which the PlayStation and positional styles
    // share, plain enough to read as either -- and an Xbox pad's.
    SwitchL,
    SwitchR,
    SteamL1,
    SteamR1,
    XboxLB,
    XboxRB,
    Count
};

// Which cell a prompt is drawn from, in a given style. Pure arithmetic over
// the table above, so it can be checked without a GL context.
GlyphCell glyph_cell(PadGlyphStyle style, ButtonGlyph glyph);

// The strip of button artwork, drawn a cell at a time. Every glyph is a black
// disc with its letter knocked out of it, so the letter picks up whatever is
// behind the prompt and one image reads correctly on a dark backdrop and on a
// white dialog alike.
class ButtonGlyphSheet {
public:
    bool initialize(std::string* error);
    void shutdown() {
        texture_.destroy();
        stylus_hand_.destroy();
    }
    bool valid() const { return texture_.valid(); }

    // How wide a glyph is when drawn `height` tall, so a caller can lay out a
    // line of prompts before drawing any of it.
    float width_for(float height) const;

    // Where the button itself sits inside the box a glyph is drawn into, as a
    // fraction of that box's height. The strip packs every button square and
    // bottom-aligned, so that whatever stands above one -- the arrow over a
    // stick click -- takes the headroom and the buttons stay on one line. Text
    // beside a glyph lines up with this rather than with the box, which would
    // leave it sitting above the circle it is labelling.
    float button_centre() const;

    // Drawn in `color`: the artwork is a disc with its letter knocked out of
    // it, so the colour is the disc and whatever is behind shows through the
    // letter. Prompts pass their own text colour, which keeps a glyph and the
    // words beside it looking like one thing.
    void draw(QuadRenderer& quads,
              ButtonGlyph glyph,
              float x,
              float y,
              float height,
              const Color& color) const;

    // Which pad's buttons these are drawn as. Held on the sheet rather than
    // passed to every draw: it is one choice for the whole app, and every
    // caller would otherwise have to carry it through to say anything.
    void set_style(PadGlyphStyle style) { style_ = style; }
    PadGlyphStyle style() const { return style_; }
    // Whether the pad in use is read by letter (InputManager::reads_by_letter):
    // its A is pressed where the prompts' A is drawn, so an Xbox or
    // PlayStation prompt for that place shows the letter or symbol Steam
    // gave it -- A, or a cross -- rather than the one printed there on a pad
    // read by place.
    void set_by_letter(bool by_letter) { by_letter_ = by_letter; }

    // The one other symbol the menus draw: the Wii U's hand holding a stylus,
    // filling `rect` in `color` -- its back parts fainter, as the artwork has
    // them -- the way round `left_handed` says. Kept here with the buttons
    // because it is tinted the same way and handed to the same places.
    void draw_stylus_hand(QuadRenderer& quads, const PixelRect& rect, const Color& color,
                          bool left_handed) const;
    // And Separate Screens' swap button: an arrow up beside an arrow down.
    void draw_swap_screens(QuadRenderer& quads, const PixelRect& rect, const Color& color) const;

private:
    Texture texture_;
    Texture stylus_hand_;
    Texture swap_screens_;
    PadGlyphStyle style_ = PadGlyphStyle::Nintendo;
    bool by_letter_ = false;
    int cell_width_ = 0;
    int cell_height_ = 0;
};

}  // namespace ds
