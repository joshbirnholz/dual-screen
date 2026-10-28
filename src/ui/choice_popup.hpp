#pragma once

#include <chrono>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "input/input_manager.hpp"
#include "render/button_glyphs.hpp"
#include "render/quad_renderer.hpp"
#include "render/text_renderer.hpp"
#include "ui/list_touch.hpp"
#include "ui/ui_icons.hpp"

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
// pair of buttons, or a grid of colors. Some can be headings over the ones
// after them -- see set_headings().
class ChoicePopup {
public:
    // How much of the screen's height a long list's rows take at most, and
    // how many rows it shows whatever that comes to, where they fit.
    static constexpr float kMaxListShare = 0.66f;
    static constexpr int kMinRowsShown = 6;
    // Its proportions, in lines of the page's text, measured off a Switch 2's
    // own System Settings: a row's height, its radio circle's, where the
    // words start in a row and how far the circle stands from its end, the
    // rows' inset in the box, and the narrowest the box is.
    static constexpr float kRowLines = 3.5f;
    static constexpr float kCircleLines = 1.75f;
    static constexpr float kTextInLines = 1.35f;
    static constexpr float kCircleInLines = 1.35f;
    static constexpr float kInsetLines = 0.67f;
    static constexpr float kMinWidthLines = 18.0f;
    // A hint's words, beside a choice's.
    static constexpr float kHintScale = 0.8f;
    // An icon before a choice's words, square, in lines.
    static constexpr float kIconLines = 2.2f;

    // Up over `row`, the row whose value it is, with `current` filled.
    void open(std::vector<std::string> choices, int current, const PixelRect& row);
    // A grid of colored circles, `columns` across, unnamed; Left and Right
    // move along a line of them too. `names` are for anything that reads them.
    void open_colors(std::vector<Color> colors, std::vector<std::string> names, int columns,
                     int current, const PixelRect& row);
    // After open(): each choice drawn as two buttons, "first / second", in
    // place of its words -- `aspect` is a glyph's width for its height.
    void set_glyph_pairs(std::vector<std::pair<ButtonGlyph, ButtonGlyph>> pairs, float aspect);
    // After open(): a button drawn after a choice's words -- "Hold (-)" --
    // ButtonGlyph::Count for none; `aspect` as above.
    void set_trailing_glyphs(std::vector<ButtonGlyph> glyphs, float aspect);
    // After open(): which choices are only headings over the ones after them.
    // They are drawn dim, without a circle, and cannot be chosen: the cursor
    // passes over them, and a tap on one does nothing.
    void set_headings(std::vector<bool> headings);
    // After open(): a few words at the right of a choice, dim, saying more
    // of what it is -- "of another button". Empty for none.
    void set_hints(std::vector<std::string> hints) { hints_ = std::move(hints); }
    // After open(): an icon from the menus' strip, `strip`, before each
    // choice's words -- UiIcon::Count for none. `strip` is the caller's and
    // has to outlive the popup's drawing; null draws none.
    void set_icons(const Texture* strip, std::vector<UiIcon> icons) {
        icon_strip_ = strip;
        icons_ = std::move(icons);
    }
    bool heading(int index) const {
        return index >= 0 && index < static_cast<int>(headings_.size()) && headings_[static_cast<std::size_t>(index)];
    }
    void close() { open_ = false; }
    bool is_open() const { return open_; }
    const std::vector<std::string>& choices() const { return choices_; }
    int current() const { return current_; }
    int focus() const { return focus_; }
    // From the other device, drawing the same popup: the list brought round
    // to it only when it has moved.
    void set_focus(int focus);
    // How far its list is scrolled, in rows; and the other device's taken on.
    float scroll_rows() const { return scroll_rows_; }
    void set_scroll_rows(float rows) {
        scroll_rows_ = std::max(0.0f, rows);
        fresh_ = false;
        drag_.stop();
    }
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

    // A finger dragging the list, or the wheel turning it, and nothing else:
    // for a device drawing the other's popup, whose cursor is the other's.
    // True when this pass's release ended a drag rather than tapped.
    bool scroll(const std::vector<PointerEvent>& pointers, float output_width, float output_height);
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
    // How many rows fit, now it is laid out; on its first pass, the list
    // brought round to the current choice.
    void settle_rows(int visible);
    // The next choice that is not a heading from `from`, `step` at a time,
    // or -1.
    int next_choice(int from, int step) const;

    bool open_ = false;
    std::vector<std::string> choices_;
    std::vector<std::pair<ButtonGlyph, ButtonGlyph>> glyph_pairs_;
    std::vector<ButtonGlyph> trailing_glyphs_;
    float glyph_aspect_ = 1.0f;
    // How wide a choice's button after its words is, with the gap before it.
    float trailing_room(std::size_t index, float line) const;
    std::vector<Color> colors_;
    std::vector<bool> headings_;
    std::vector<std::string> hints_;
    const Texture* icon_strip_ = nullptr;
    std::vector<UiIcon> icons_;
    // How wide an icon is, with the gap after it, for a row whose words are
    // `line` high: none where nothing has one.
    float icon_room(float line) const;
    int columns_ = 0;
    int current_ = -1;
    int focus_ = 0;
    // The choice under the cursor as last drawn, and since when: one too
    // long for the box slides along -- see draw_fitted_text().
    int drawn_focus_ = -1;
    std::chrono::steady_clock::time_point focus_since_{};
    // How many rows of it are in view, as last laid out.
    int shown_rows_ = 1;
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
    ScrollEase scroll_ease_;
};

}  // namespace ds
