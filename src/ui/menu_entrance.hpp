#pragma once

#include <algorithm>
#include <chrono>

namespace ds {

// How the quick menu comes up: the blurred game behind it fades in quickly,
// then the menu itself fades in and grows the last little way into place,
// just after. Started as the menu opens -- on whichever device, as each draws
// its own -- and done in under a quarter of a second; a page opened from the
// menu, or the menu come back to from one, is simply there.
class MenuEntrance {
public:
    using Clock = std::chrono::steady_clock;

    static constexpr float kBackdropSeconds = 0.12f;
    static constexpr float kContentsDelaySeconds = 0.07f;
    static constexpr float kContentsSeconds = 0.15f;
    // How big the menu starts, before it grows to its size.
    static constexpr float kStartScale = 0.96f;

    void start(Clock::time_point now = Clock::now()) {
        started_ = now;
        running_ = true;
    }
    // Over at once: nothing left to animate.
    void finish() { running_ = false; }

    // How far the backdrop has faded in, 0 to 1.
    float backdrop(Clock::time_point now = Clock::now()) const {
        return ease(progress(now, 0.0f, kBackdropSeconds));
    }
    // How far the menu has faded in, 0 to 1, and how big it is drawn.
    float contents(Clock::time_point now = Clock::now()) const {
        return ease(progress(now, kContentsDelaySeconds, kContentsSeconds));
    }
    float scale(Clock::time_point now = Clock::now()) const {
        return kStartScale + (1.0f - kStartScale) * contents(now);
    }
    // Whether anything is still moving.
    bool animating(Clock::time_point now = Clock::now()) const { return contents(now) < 1.0f; }

private:
    float progress(Clock::time_point now, float delay, float length) const {
        if (!running_) return 1.0f;
        const float since = std::chrono::duration<float>(now - started_).count() - delay;
        return std::clamp(since / length, 0.0f, 1.0f);
    }
    // Quick at first and settling at the end, as a console's menus move.
    static float ease(float t) {
        const float rest = 1.0f - t;
        return 1.0f - rest * rest * rest;
    }

    Clock::time_point started_{};
    bool running_ = false;
};

}  // namespace ds
