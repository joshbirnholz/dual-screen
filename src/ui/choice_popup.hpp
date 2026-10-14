#pragma once

#include <string>
#include <utility>
#include <vector>

#include "input/input_manager.hpp"
#include "render/button_glyphs.hpp"
#include "render/quad_renderer.hpp"
#include "render/text_renderer.hpp"
#include "ui/list_touch.hpp"

namespace ds {

struct ChoicePopupResult {
    // B, or a tap outside it: put away, nothing changed.
    bool closed = false;
    // The menu's own button: put away, and whatever it was opened over too.
    bool close_everything = false;
    // The choice made, by its index, or -1.
    int chosen = -1;
};

// A value with several choices, chosen the way a Switch's settings choose
// one: a small, well-rounded box beside its row, each choice with a radio
// circle and the one in effect filled, the cursor on that one to begin with.
// Up and Down move, stopping at the ends; A chooses; B puts it away. A long
// list scrolls, with a bar, under a finger or the mouse wheel as well. A tap
// on a choice chooses it; a tap outside puts it away, changing nothing.
//
// Where it opens it stays: only what is inside it scrolls.
//
// Its choices are words, or -- see set_glyph_pairs() and open_colors() -- a
// pair of buttons, or a grid of colors.
class ChoicePopup {
public:
    // Up over `row`, the row whose value it is, with `current` filled.
    void open(std::vector<std::string> choices, int current, const PixelRect& row);
    // A grid of colored circles, `columns` across, named under the grid as the
    // cursor moves; Left and Right move along a line of them too.
    void open_colors(std::vector<Color> colors, std::vector<std::string> names, int columns,
                     int current, const PixelRect& row);
    // After open(): each choice drawn as two buttons, "first / second", in
    // place of its words -- `aspect` is a glyph's width for its height.
    void set_glyph_pairs(std::vector<std::pair<ButtonGlyph, ButtonGlyph>> pairs, float aspect);
    void close() { open_ = false; }
    bool is_open() const { return open_; }
    const std::vector<std::string>& choices() const { return choices_; }
    int current() const { return current_; }
    int focus() const { return focus_; }
    // From the other device, drawing the same popup.
    void set_focus(int focus);
    // The row it was opened over, moved -- the page laid out after it opened,
    // say. Only until it is first drawn: from then on it stays where it is.
    void set_row(const PixelRect& row) {
        if (!placed_) row_ = row;
    }

    ChoicePopupResult update(const std::vector<UiAction>& actions,
                             const std::vector<PointerEvent>& pointers, float output_width,
                             float output_height);
    // `glyphs` draws a choice that is a pair of buttons.
    void render(QuadRenderer& quads, TextRenderer& text, float output_width, float output_height,
                const ButtonGlyphSheet* glyphs = nullptr);

    // Which choice is under a point, or -1: for a device drawing the
    // other's popup, to send it as a focus.
    int choice_at(float x, float y, float output_width, float output_height) const;
    // Whether a point is on the popup at all: a tap anywhere else puts it
    // away, changing nothing.
    bool contains(float x, float y, float output_width, float output_height) const;

private:
    struct Geometry {
        PixelRect box;
        PixelRect list;
        std::vector<PixelRect> rows;  // every choice's, scrolled; a grid's cells
        float scale = 1.0f;
        float row_height = 0.0f;
        int visible = 0;
        // A grid's: where the focused color's name goes.
        float name_y = 0.0f;
    };
    Geometry geometry(float output_width, float output_height) const;
    void keep_focus_in_view(int visible);
    bool grid() const { return columns_ > 0; }
    float max_scroll_rows(int visible) const;

    bool open_ = false;
    std::vector<std::string> choices_;
    std::vector<std::pair<ButtonGlyph, ButtonGlyph>> glyph_pairs_;
    float glyph_aspect_ = 1.0f;
    std::vector<Color> colors_;
    int columns_ = 0;
    int current_ = -1;
    int focus_ = 0;
    // How far the list is scrolled, in rows, so it means the same at any size.
    float scroll_rows_ = 0.0f;
    // How many rows above the current choice were in view as it opened: the
    // box is placed by them, and only by them, so it never moves.
    float anchor_rows_ = 0.0f;
    PixelRect row_;
    // Drawn at least once, so set where it is.
    bool placed_ = false;
    // Its first pass, when the list is brought round to the current choice at
    // however many rows fit.
    bool fresh_ = true;
    ListDrag drag_;
};

}  // namespace ds
