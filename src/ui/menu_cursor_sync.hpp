#pragma once

#include <optional>

#include "net/protocol.hpp"
#include "ui/list_touch.hpp"
#include "ui/menu_page.hpp"

namespace ds {

// Keeps the cursor on the menu page both devices show the same on both: one
// page, whichever screen it is on. A finger going down on either hides the
// ring on both; a press of either pad brings it back on both; and a finger
// held on a row, a button or a choice fills it on both. Run once a pass on
// each device, after the page has had this pass's input, with the page shown
// (or none).
//
// Each says what it has whenever that changes here, and takes the other's as
// it arrives -- and what it took is never said back. Coming to a page, only
// the host says how it stands, so the two never trade places.
class MenuCursorSync {
public:
    explicit MenuCursorSync(bool host) : host_(host) {}

    // `theirs` is the other device's, newly arrived, or none. What to tell
    // the other device, if anything.
    std::optional<net::MenuCursorMessage> sync(const MenuPage* page, net::MirroredPage shown,
                                               const net::MenuCursorMessage* theirs, float output_width,
                                               float output_height) {
        if (page == nullptr || shown == net::MirroredPage::None) {
            if (!on_page_) return std::nullopt;
            set_remote_held(false, 0.0f, 0.0f);
            on_page_ = false;
            // The host says it has left, so that coming back -- to the same
            // page, the same cursor -- is said again.
            if (host_) return net::MenuCursorMessage{};
            return std::nullopt;
        }
        net::MenuCursorMessage mine = here(*page, shown, output_width, output_height);
        const bool arrived = !on_page_ || said_.page != shown;
        if (arrived) {
            on_page_ = true;
            set_remote_held(false, 0.0f, 0.0f);
            said_ = mine;
            if (host_) return mine;
        }
        if (theirs != nullptr && theirs->page == shown) {
            set_menu_cursor_shown(theirs->shown);
            mine.shown = theirs->shown;
            said_.shown = theirs->shown;
            float x = 0.0f;
            float y = 0.0f;
            const bool held = theirs->held >= 0 && entry_point(*page, theirs->held, output_width, output_height, x, y);
            set_remote_held(held, x, y);
        }
        if (mine == said_) return std::nullopt;
        said_ = mine;
        return mine;
    }

    // Somewhere on the page that is on `entry`, found by looking: the page
    // says what a point is on, not where an entry is. Only when a finger
    // there goes down on something, so seldom.
    static bool entry_point(const MenuPage& page, int entry, float output_width, float output_height, float& x,
                            float& y) {
        // Coarsely first, which finds a row or a tile at a few hundred
        // looks; then finely, for anything smaller.
        for (const float step : {24.0f, 6.0f}) {
            const int columns = step > 10.0f ? 16 : 32;
            for (y = step * 0.5f; y < output_height; y += step) {
                for (int column = 0; column < columns; ++column) {
                    x = output_width * (static_cast<float>(column) + 0.5f) / static_cast<float>(columns);
                    if (page.entry_at(x, y, output_width, output_height) == entry) return true;
                }
            }
        }
        return false;
    }

private:
    // The cursor as it is here: whether it shows, and what a finger here is
    // held on -- what it went down on, while it is still on that and has not
    // moved off to scroll.
    static net::MenuCursorMessage here(const MenuPage& page, net::MirroredPage shown, float output_width,
                                       float output_height) {
        net::MenuCursorMessage mine;
        mine.page = shown;
        mine.shown = menu_cursor_shown();
        const LivePointer& live = live_pointer();
        if (live.down && !live.moved) {
            const int entry = page.entry_at(live.press_x, live.press_y, output_width, output_height);
            if (entry >= 0 && page.entry_at(live.x, live.y, output_width, output_height) == entry) mine.held = entry;
        }
        return mine;
    }

    bool host_;
    bool on_page_ = false;
    net::MenuCursorMessage said_;
};

}  // namespace ds
