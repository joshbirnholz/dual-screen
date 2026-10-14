#pragma once

#include <string>
#include <vector>

#include "input/input_manager.hpp"
#include "render/quad_renderer.hpp"
#include "render/text_renderer.hpp"

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
// list scrolls, with a bar. A tap on a choice chooses it; a tap outside puts
// it away, changing nothing.
class ChoicePopup {
public:
    // Up over `row`, the row whose value it is, with `current` filled.
    void open(std::vector<std::string> choices, int current, const PixelRect& row);
    void close() { open_ = false; }
    bool is_open() const { return open_; }
    const std::vector<std::string>& choices() const { return choices_; }
    int current() const { return current_; }
    int focus() const { return focus_; }
    // From the other device, drawing the same popup.
    void set_focus(int focus);
    // The row it was opened over, moved -- the page scrolled, say.
    void set_row(const PixelRect& row) { row_ = row; }

    ChoicePopupResult update(const std::vector<UiAction>& actions,
                             const std::vector<PointerEvent>& pointers, float output_width,
                             float output_height);
    void render(QuadRenderer& quads, TextRenderer& text, float output_width, float output_height);

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
        std::vector<PixelRect> rows;  // every choice's, scrolled
        float scale = 1.0f;
        float row_height = 0.0f;
        int visible = 0;
    };
    Geometry geometry(float output_width, float output_height) const;
    void keep_focus_in_view(int visible);

    bool open_ = false;
    std::vector<std::string> choices_;
    int current_ = -1;
    int focus_ = 0;
    int first_ = 0;
    PixelRect row_;
};

}  // namespace ds
