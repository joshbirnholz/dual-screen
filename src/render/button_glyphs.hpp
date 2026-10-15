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
// LeftStick and RightStick are the sticks clicked in; LeftStickMove and
// RightStickMove the sticks themselves. Plus and Minus are start and select,
// L and R the shoulder buttons and ZL and ZR the triggers -- every pad has
// them in the same places and prints them its own way. Every button a pad
// has is here, so any of them can be named.
enum class ButtonGlyph {
    A,
    B,
    X,
    Y,
    LeftStick,
    RightStick,
    Plus,
    Minus,
    L,
    R,
    ZL,
    ZR,
    DpadUp,
    DpadDown,
    DpadLeft,
    DpadRight,
    Dpad,
    LeftStickMove,
    RightStickMove,
    Home,
    Capture,
    Count
};

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

// The cells in assets/button_icons.png, in the order they sit in the sheet --
// see assets/make_button_icons.py, which draws it from Kenney's Input Prompts.
// Each pad's whole set, in ButtonGlyph order.
enum class GlyphCell {
    // A Switch's.
    LetterA,
    LetterB,
    LetterX,
    LetterY,
    LeftStick,
    RightStick,
    Plus,
    Minus,
    SwitchL,
    SwitchR,
    SwitchZL,
    SwitchZR,
    SwitchDpadUp,
    SwitchDpadDown,
    SwitchDpadLeft,
    SwitchDpadRight,
    SwitchDpad,
    SwitchStickL,
    SwitchStickR,
    SwitchHome,
    SwitchCapture,
    // A PlayStation's.
    Circle,
    Cross,
    Triangle,
    Square,
    PlayStationL3,
    PlayStationR3,
    PlayStationOptions,
    PlayStationCreate,
    PlayStationL1,
    PlayStationR1,
    PlayStationL2,
    PlayStationR2,
    PlayStationDpadUp,
    PlayStationDpadDown,
    PlayStationDpadLeft,
    PlayStationDpadRight,
    PlayStationDpad,
    PlayStationStickL,
    PlayStationStickR,
    PlayStationMute,
    // An Xbox pad's.
    XboxA,
    XboxB,
    XboxX,
    XboxY,
    XboxLS,
    XboxRS,
    XboxMenu,
    XboxView,
    XboxLB,
    XboxRB,
    XboxLT,
    XboxRT,
    XboxDpadUp,
    XboxDpadDown,
    XboxDpadLeft,
    XboxDpadRight,
    XboxDpad,
    XboxStickL,
    XboxStickR,
    XboxGuide,
    XboxShare,
    // Four dots, the one to press filled in -- see PadGlyphStyle::Positional
    // -- and a Steam Controller's shoulders and triggers, the plainest.
    DotsEast,
    DotsSouth,
    DotsNorth,
    DotsWest,
    SteamL1,
    SteamR1,
    SteamL2,
    SteamR2,
    Count
};

// How many cells a row of the sheet holds.
inline constexpr int kGlyphSheetColumns = 16;

// Which cell a prompt is drawn from, in a given style. Pure arithmetic over
// the table above, so it can be checked without a GL context.
GlyphCell glyph_cell(PadGlyphStyle style, ButtonGlyph glyph);

// The sheet of button artwork, drawn a cell at a time. Every glyph is a
// shape with its letter knocked out of it, so the letter picks up whatever is
// behind the prompt and one image reads correctly on a dark backdrop and on a
// white dialog alike.
class ButtonGlyphSheet {
public:
    bool initialize(std::string* error);
    // Every texture it has, so initialize() makes them all again: one left
    // behind keeps a name from the old context, and the next context hands
    // that name to something else -- the Swap Screens button drew the whole
    // button sheet, and the stylus hand the swap arrows.
    void shutdown() {
        texture_.destroy();
        stylus_hand_.destroy();
        swap_screens_.destroy();
    }
    bool valid() const { return texture_.valid(); }

    // How wide a glyph is when drawn `height` tall, so a caller can lay out a
    // line of prompts before drawing any of it.
    float width_for(float height) const;

    // Where the button itself sits inside the box a glyph is drawn into, as a
    // fraction of that box's height: the middle, every cell being a square
    // with its button centred in it. Text beside a glyph lines up with this.
    float button_centre() const;

    // Drawn in `color`: the artwork is a shape with its letter knocked out of
    // it, so the colour is the shape and whatever is behind shows through the
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
    int cell_width_ = 0;
    int cell_height_ = 0;
    int sheet_width_ = 0;
    int sheet_height_ = 0;
};

}  // namespace ds
