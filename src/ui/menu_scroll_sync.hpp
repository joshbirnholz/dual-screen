#pragma once

#include <chrono>
#include <optional>
#include <vector>

#include "net/protocol.hpp"
#include "ui/menu_page.hpp"

namespace ds {

// Keeps the menu page both devices show scrolled the same on both: one page,
// whichever screen it is on. Run once a pass on each device, after the page
// has had this pass's input, with the page shown (or none).
//
// The host's scroll is the page's: it goes over whenever it changes -- a
// finger or the wheel there, or the cursor bringing the list with it. The
// companion's goes over while it is scrolling the page itself, and the host
// takes it. Each takes the other's but for while it is being scrolled itself,
// so a finger is never pulled back by its own echo.
class MenuScrollSync {
public:
    explicit MenuScrollSync(bool host) : host_(host) {}

    // `theirs` is the other device's: on the host, only one newly arrived;
    // on the companion, the host's as it last said. What to tell the other
    // device, if anything.
    std::optional<net::MenuScrollMessage> sync(MenuPage* page, net::MirroredPage shown,
                                               const net::MenuScrollMessage* theirs, float output_width,
                                               float output_height) {
        if (page == nullptr || shown == net::MirroredPage::None) {
            shown_ = net::MirroredPage::None;
            said_.clear();
            return std::nullopt;
        }
        const bool scrolling_here = page->scrolled_here(std::chrono::steady_clock::now());
        if (theirs != nullptr && theirs->page == shown && !scrolling_here) {
            page->set_scroll_state(theirs->positions, output_width, output_height);
        }
        const std::vector<float> state = page->scroll_state(output_width, output_height);
        if (!host_ && !scrolling_here) return std::nullopt;
        if (shown == shown_ && state == said_) return std::nullopt;
        shown_ = shown;
        said_ = state;
        net::MenuScrollMessage message;
        message.page = shown;
        message.positions = state;
        return message;
    }

private:
    bool host_ = false;
    net::MirroredPage shown_ = net::MirroredPage::None;
    std::vector<float> said_;
};

}  // namespace ds
