#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
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
//
// Taken, it is glided to rather than jumped to: positions come over the
// network a few at a time, and a list that jumped to each one stuttered. And
// what was taken is never said back -- a host whose screen holds more rows
// stops its list sooner, and saying that back pulled the other device's list
// away from where its finger left it.
class MenuScrollSync {
public:
    using Clock = std::chrono::steady_clock;
    // How quickly a following list closes on where it was told to be: most
    // of the way in a few frames, so it keeps up with a finger.
    static constexpr float kFollowSeconds = 0.045f;

    explicit MenuScrollSync(bool host) : host_(host) {}

    // `theirs` is the other device's: on the host, only one newly arrived;
    // on the companion, the host's as it last said. What to tell the other
    // device, if anything.
    std::optional<net::MenuScrollMessage> sync(MenuPage* page, net::MirroredPage shown,
                                               const net::MenuScrollMessage* theirs, float output_width,
                                               float output_height, Clock::time_point now = Clock::now()) {
        if (page == nullptr || shown == net::MirroredPage::None) {
            shown_ = net::MirroredPage::None;
            page_ = net::MirroredPage::None;
            said_.clear();
            heard_.clear();
            following_ = false;
            return std::nullopt;
        }
        if (shown != page_) {
            // A new page: whatever was followed belonged to the last one.
            page_ = shown;
            following_ = false;
            heard_.clear();
        }
        const float seconds = std::clamp(std::chrono::duration<float>(now - last_pass_).count(), 0.0f, 0.1f);
        last_pass_ = now;
        const bool scrolling_here = page->scrolled_here(now);
        std::vector<float> state = page->scroll_state(output_width, output_height);

        // Something here moved it since it was last set -- a finger, the
        // cursor bringing the list with it: that wins over following.
        if (following_ && state != placed_) following_ = false;
        if (theirs != nullptr && theirs->page == shown && theirs->positions != heard_) {
            heard_ = theirs->positions;
            if (!scrolling_here) {
                target_ = theirs->positions;
                following_ = true;
            }
        }
        if (scrolling_here) following_ = false;

        if (following_) {
            std::vector<float> next = target_;
            if (state.size() == target_.size() && seconds > 0.0f) {
                const float step = 1.0f - std::exp(-seconds / kFollowSeconds);
                for (std::size_t index = 0; index < next.size(); ++index) {
                    const float gap = target_[index] - state[index];
                    next[index] = std::fabs(gap) < 0.01f ? target_[index] : state[index] + gap * step;
                }
            }
            page->set_scroll_state(next, output_width, output_height);
            placed_ = page->scroll_state(output_width, output_height);
            // There, or as near as this screen goes: done.
            if (placed_ == state || placed_ == target_) following_ = false;
            // Taken, not said: see above.
            shown_ = shown;
            said_ = placed_;
            return std::nullopt;
        }

        if (!host_ && !scrolling_here) return std::nullopt;
        if (shown == shown_ && state == said_) return std::nullopt;
        shown_ = shown;
        said_ = state;
        net::MenuScrollMessage message;
        message.page = shown;
        message.positions = state;
        return message;
    }

    // Whether it is still gliding to where it was told, for a test.
    bool following() const { return following_; }

private:
    bool host_ = false;
    // The page last said about, and the page this pass is on.
    net::MirroredPage shown_ = net::MirroredPage::None;
    net::MirroredPage page_ = net::MirroredPage::None;
    std::vector<float> said_;
    // The other device's last word, so the same word is not followed twice.
    std::vector<float> heard_;
    std::vector<float> target_;
    std::vector<float> placed_;
    bool following_ = false;
    Clock::time_point last_pass_{};
};

}  // namespace ds
