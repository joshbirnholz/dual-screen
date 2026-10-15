#pragma once

#include <algorithm>
#include <chrono>

namespace ds {

// How the quick menu comes and goes: opened from the game, the blurred game
// behind it fades in quickly, then the menu itself fades in and grows the
// last little way into place, just after; closed back to the game, the menu
// fades and shrinks away first, then the backdrop -- the game already going
// again underneath. Each well under a quarter of a second. Going into a page
// of the menu, or back to the menu from one, is simply there: the menu never
// left.
class MenuEntrance {
public:
    using Clock = std::chrono::steady_clock;

    static constexpr float kBackdropSeconds = 0.12f;
    static constexpr float kContentsDelaySeconds = 0.07f;
    static constexpr float kContentsSeconds = 0.15f;
    // Going away: the menu first, then the backdrop, a little quicker.
    static constexpr float kLeaveContentsSeconds = 0.10f;
    static constexpr float kLeaveBackdropDelaySeconds = 0.04f;
    static constexpr float kLeaveBackdropSeconds = 0.10f;
    // How big the menu is at its smallest, before it grows or after it shrinks.
    static constexpr float kStartScale = 0.96f;

    // Opened from the game.
    void start(Clock::time_point now = Clock::now()) {
        started_ = now;
        mode_ = Mode::Entering;
    }
    // Closed back to the game.
    void leave(Clock::time_point now = Clock::now()) {
        started_ = now;
        mode_ = Mode::Leaving;
    }
    // Simply there, or simply gone: nothing left to animate.
    void finish() { mode_ = Mode::Still; }

    // How much of the backdrop is showing, 0 to 1.
    float backdrop(Clock::time_point now = Clock::now()) const {
        if (mode_ == Mode::Leaving) return 1.0f - ease(progress(now, kLeaveBackdropDelaySeconds, kLeaveBackdropSeconds));
        return ease(progress(now, 0.0f, kBackdropSeconds));
    }
    // How much of the menu is showing, 0 to 1, and how big it is drawn.
    float contents(Clock::time_point now = Clock::now()) const {
        if (mode_ == Mode::Leaving) return 1.0f - ease(progress(now, 0.0f, kLeaveContentsSeconds));
        return ease(progress(now, kContentsDelaySeconds, kContentsSeconds));
    }
    float scale(Clock::time_point now = Clock::now()) const {
        return kStartScale + (1.0f - kStartScale) * contents(now);
    }
    // Whether it is still coming up.
    bool animating(Clock::time_point now = Clock::now()) const {
        return mode_ == Mode::Entering && contents(now) < 1.0f;
    }
    // Whether it is still going away -- drawn over the game until it has.
    bool leaving(Clock::time_point now = Clock::now()) const {
        return mode_ == Mode::Leaving && backdrop(now) > 0.0f;
    }

private:
    enum class Mode { Still, Entering, Leaving };

    float progress(Clock::time_point now, float delay, float length) const {
        if (mode_ == Mode::Still) return 1.0f;
        const float since = std::chrono::duration<float>(now - started_).count() - delay;
        return std::clamp(since / length, 0.0f, 1.0f);
    }
    // Quick at first and settling at the end, as a console's menus move.
    static float ease(float t) {
        const float rest = 1.0f - t;
        return 1.0f - rest * rest * rest;
    }

    Clock::time_point started_{};
    Mode mode_ = Mode::Still;
};

}  // namespace ds
