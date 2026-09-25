#pragma once

#include <cstddef>
#include <cstdint>

namespace ds {

// Draws a frame as late as the display allows, rather than straight after
// the last one went up.
//
// A loop that reads the network, decodes and draws and then waits in the swap
// for the display's refresh does its reading right after each refresh -- so a
// picture that lands a moment later waits almost a whole refresh before it is
// even looked at, and another before it is shown. Waiting first instead, until
// just before the next refresh, and then doing the whole pass, gets the
// newest picture on screen up to a refresh sooner: about 16ms at 60Hz, on
// every frame, for nothing but a sleep in the right place. RetroArch calls the
// same idea frame delay; Moonlight gets the effect by decoding on a thread of
// its own and handing each picture to the display the moment it is ready.
//
// All of it is measured, not assumed. The refresh period and phase come from
// when swaps return; how long a pass takes comes from the passes themselves,
// the slowest recent one, with a margin on top. A refresh that is missed --
// the pass ran long and the picture waited a whole extra period -- widens the
// margin, and calm narrows it again. Where swaps do not follow a regular
// refresh at all (no vsync, or a compositor that does its own thing), there
// is nothing to aim at and it waits for nothing.
//
// Microseconds throughout, on whatever monotonic clock the caller uses.
class FrameDelay {
public:
    // DUALSCREEN_FRAME_DELAY=0 turns it off, for comparing with and without.
    FrameDelay();
    // Right after the swap returns: `now_us` is when it returned, and
    // `blocked_us` how long the swap call itself took. A swap that blocks is
    // what says the display paces this loop; one that returns at once says
    // nothing does.
    void presented(uint64_t now_us, uint64_t blocked_us);
    // How long a pass took, from waking up to handing the frame to the swap.
    void worked(uint64_t work_us);
    // How long to wait now -- called after presented() -- before starting
    // the next pass. Zero when there is nothing to aim at.
    uint64_t wait_us() const;

    // What the measurements currently say, for the log and for tests.
    bool active() const { return active_; }
    uint64_t period_us() const { return period_us_; }
    uint64_t margin_us() const { return kBaseMarginUs + backoff_us_; }
    uint64_t work_estimate_us() const;

    // The window of recent intervals and passes it judges from.
    static constexpr std::size_t kWindow = 32;
    // Always left between the end of a pass and the refresh, on top of the
    // slowest recent pass: a scheduler waking a sleeper late, a swap that
    // costs more than it did.
    static constexpr uint64_t kBaseMarginUs = 2500;
    static constexpr uint64_t kMaxBackoffUs = 8000;

private:
    void judge();

    uint64_t intervals_[kWindow] = {};
    std::size_t interval_count_ = 0;
    std::size_t interval_next_ = 0;
    uint64_t blocked_[kWindow] = {};
    uint64_t work_[kWindow] = {};
    std::size_t work_count_ = 0;
    std::size_t work_next_ = 0;
    uint64_t last_presented_us_ = 0;
    uint64_t period_us_ = 0;
    bool active_ = false;
    uint64_t backoff_us_ = 0;
    // Refreshes presented since the margin last had to widen.
    unsigned calm_frames_ = 0;
    bool disabled_ = false;
};

}  // namespace ds
