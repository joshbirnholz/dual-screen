#pragma once

#include <chrono>
#include <vector>

#include "input/input_manager.hpp"
#include "render/text_renderer.hpp"

namespace ds {

// Every page of menu in this app -- anything the player moves a cursor
// through and taps -- is a MenuPage, and works the same way on every device
// it is drawn on:
//
//   - every list on it scrolls under a finger, the mouse and the mouse wheel
//     (ListDrag does all three -- see scroll());
//   - a tap is heard in one place only, take_taps(), which hands over this
//     device's own taps and the ones the other device made on its drawing of
//     the page alike, so the two can never answer a tap differently;
//   - a tap off an open popup puts it away;
//   - with two devices showing it, it is one page: where it is scrolled is
//     the same on both (scroll_state(), and MenuScrollSync, which keeps it
//     so), as its cursor and popups already are.
//
// tests/lint_menu_pages.cmake fails the tests for a class that takes a
// menu's presses without being one, or a page that never asks for its taps,
// so a page added later cannot quietly leave any of this out.
class MenuPage {
public:
    virtual ~MenuPage() = default;

    // Scrolls this device's view of the page under a finger, the mouse or the
    // wheel, and does nothing else -- see ListDrag. True when this pass's
    // release ended a drag, and so was not a tap. A page with nothing that
    // scrolls says false.
    virtual bool scroll(const std::vector<PointerEvent>& pointers, float output_width,
                        float output_height) = 0;
    // What a point on the page is on, as an entry -- any number the page
    // chooses, 0 or more -- or -1 for nothing. Laid out for this device's
    // screen, which is how a tap on another device's drawing of the page is
    // told to the device holding it: as what it landed on, not where.
    virtual int entry_at(float x, float y, float output_width, float output_height) const = 0;
    // Whether a point is off an open popup altogether: a tap there puts it
    // away. False when none is open.
    virtual bool off_popup(float x, float y, float output_width, float output_height) const = 0;
    // Whether a finger that went down on one entry and came up on another
    // tapped -- the one it came up on. Not as a rule, which is a finger
    // sliding off what it was aiming at; but a page whose entries are places
    // on a picture rather than rows says yes for two of those.
    virtual bool same_entry(int pressed_on, int released_on) const { return pressed_on == released_on; }

    // Where each of its lists is scrolled -- a popup's too, while one is up
    // -- in a measure that is the same at any size: lines of text (see
    // scroll_in_lines()) or rows. The other device shows the page scrolled
    // exactly so. A page with nothing that scrolls says none.
    virtual std::vector<float> scroll_state(float output_width, float output_height) const = 0;
    // The other device's, taken on, as far as this device's lists go; a
    // glide here stopped.
    virtual void set_scroll_state(const std::vector<float>& state, float output_width,
                                  float output_height) = 0;
    // Whether this device has just scrolled the page itself -- a finger, a
    // glide, the wheel -- so that its scroll is the one that counts.
    bool scrolled_here(std::chrono::steady_clock::time_point now) const {
        return now - scrolled_here_at_ < std::chrono::milliseconds(400);
    }

    // A tap the other device made on its drawing of this page, already
    // resolved there to an entry (or kTapOffPopup). Heard by the next
    // take_taps(), with this device's own.
    void press_entry(int entry) { forwarded_.push_back(entry); }

    // A tap that landed off an open popup.
    static constexpr int kTapOffPopup = -2;

protected:
    // This pass's taps, oldest first: those the other device made, then this
    // device's own among `pointers` -- each the entry it landed on, or
    // kTapOffPopup -- after the page has scrolled under them. The one way a
    // page hears a tap.
    std::vector<int> take_taps(const std::vector<PointerEvent>& pointers, float output_width,
                               float output_height) {
        std::vector<int> taps;
        taps.swap(forwarded_);
        for (const int entry : taps_on(*this, pointers, output_width, output_height)) taps.push_back(entry);
        return taps;
    }

public:
    // The taps among `pointers` on a page drawn here, after it has scrolled
    // under them: each the entry it landed on, or kTapOffPopup. For a page
    // this device holds, take_taps() asks for these; for the other device's
    // page drawn here, they are what is sent over.
    static std::vector<int> taps_on(MenuPage& page, const std::vector<PointerEvent>& pointers,
                                    float output_width, float output_height) {
        std::vector<int> taps;
        const std::vector<float> before = page.scroll_state(output_width, output_height);
        const bool dragged = page.scroll(pointers, output_width, output_height);
        if (page.scroll_state(output_width, output_height) != before) {
            page.scrolled_here_at_ = std::chrono::steady_clock::now();
        }
        for (const PointerEvent& pointer : pointers) {
            if (!pointer.released || dragged) continue;
            if (page.off_popup(pointer.x, pointer.y, output_width, output_height) &&
                page.off_popup(pointer.press_x, pointer.press_y, output_width, output_height)) {
                taps.push_back(kTapOffPopup);
                continue;
            }
            // Only where the finger came down on the same entry it left on.
            const int entry = page.entry_at(pointer.x, pointer.y, output_width, output_height);
            if (entry >= 0 &&
                page.same_entry(page.entry_at(pointer.press_x, pointer.press_y, output_width, output_height),
                                entry)) {
                taps.push_back(entry);
            }
        }
        return taps;
    }

private:
    std::vector<int> forwarded_;
    std::chrono::steady_clock::time_point scrolled_here_at_{};
};

// A scroll in pixels as lines of text at a page's `scale`, and back: every
// page's layout grows with its text, so this means the same at any size.
inline float scroll_in_lines(float pixels, float scale) {
    const float line = TextRenderer::line_height(scale);
    return line > 0.0f ? pixels / line : 0.0f;
}
inline float scroll_from_lines(float lines, float scale) { return lines * TextRenderer::line_height(scale); }
// The `index`th of a state, or 0 when it has fewer.
inline float scroll_at(const std::vector<float>& state, std::size_t index) {
    return index < state.size() ? state[index] : 0.0f;
}

}  // namespace ds
