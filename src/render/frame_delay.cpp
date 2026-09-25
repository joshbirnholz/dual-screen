#include "render/frame_delay.hpp"

#include <algorithm>
#include <cstdlib>

namespace ds {

namespace {

// A refresh this far past the period it should have taken is one the pass
// missed: the picture waited a whole extra refresh.
constexpr double kMissedAfterPeriods = 1.5;
// How far a swap interval may stray from the typical one and still count as
// the display's own rhythm, and how many of them have to.
constexpr double kRegularWithin = 0.12;
constexpr double kRegularShare = 0.75;
// Displays run from 24Hz to 240Hz; anything outside that is not a refresh.
constexpr uint64_t kShortestPeriodUs = 4000;
constexpr uint64_t kLongestPeriodUs = 42000;
// Before trusting the rhythm at all: a swap that returns at once is not
// being held to a refresh, however regular the loop around it happens to be.
constexpr uint64_t kSwapWaitsAtLeastUs = 2000;
// Not worth sleeping for less than this: the wake-up is not that precise.
constexpr uint64_t kShortestWaitUs = 1000;
// The margin widens by this on a missed refresh, and narrows by half of it
// after this many calm ones -- about five seconds at 60Hz.
constexpr uint64_t kBackoffStepUs = 1000;
constexpr unsigned kCalmFramesToNarrow = 300;

}  // namespace

FrameDelay::FrameDelay() {
    const char* setting = std::getenv("DUALSCREEN_FRAME_DELAY");
    disabled_ = setting != nullptr && setting[0] == '0';
}

void FrameDelay::presented(uint64_t now_us, uint64_t blocked_us) {
    if (last_presented_us_ != 0 && now_us > last_presented_us_) {
        const uint64_t interval = now_us - last_presented_us_;
        intervals_[interval_next_] = interval;
        blocked_[interval_next_] = blocked_us;
        interval_next_ = (interval_next_ + 1) % kWindow;
        interval_count_ = std::min(interval_count_ + 1, kWindow);
        if (active_ && period_us_ > 0 &&
            static_cast<double>(interval) > static_cast<double>(period_us_) * kMissedAfterPeriods) {
            backoff_us_ = std::min(kMaxBackoffUs, backoff_us_ + kBackoffStepUs);
            calm_frames_ = 0;
        } else if (++calm_frames_ >= kCalmFramesToNarrow) {
            calm_frames_ = 0;
            backoff_us_ = backoff_us_ > kBackoffStepUs / 2 ? backoff_us_ - kBackoffStepUs / 2 : 0;
        }
    }
    last_presented_us_ = now_us;
    judge();
}

void FrameDelay::worked(uint64_t work_us) {
    work_[work_next_] = work_us;
    work_next_ = (work_next_ + 1) % kWindow;
    work_count_ = std::min(work_count_ + 1, kWindow);
}

uint64_t FrameDelay::work_estimate_us() const {
    // The slowest recent pass, not a typical one: aiming for the typical pass
    // misses the refresh on every slow one, and a missed refresh is a whole
    // period late -- far worse than the few milliseconds this gives back.
    uint64_t slowest = 0;
    for (std::size_t index = 0; index < work_count_; ++index) {
        slowest = std::max(slowest, work_[index]);
    }
    return slowest;
}

void FrameDelay::judge() {
    if (interval_count_ < kWindow / 2) {
        active_ = false;
        return;
    }
    uint64_t sorted[kWindow];
    std::copy(intervals_, intervals_ + interval_count_, sorted);
    std::nth_element(sorted, sorted + interval_count_ / 2, sorted + interval_count_);
    const uint64_t typical = sorted[interval_count_ / 2];
    if (typical < kShortestPeriodUs || typical > kLongestPeriodUs) {
        active_ = false;
        return;
    }
    std::size_t regular = 0;
    for (std::size_t index = 0; index < interval_count_; ++index) {
        const double off = static_cast<double>(intervals_[index]) - static_cast<double>(typical);
        if ((off < 0 ? -off : off) <= static_cast<double>(typical) * kRegularWithin) {
            ++regular;
        }
    }
    if (static_cast<double>(regular) < static_cast<double>(interval_count_) * kRegularShare) {
        active_ = false;
        return;
    }
    if (!active_) {
        // Only start aiming at a rhythm the swap itself is holding the loop
        // to. Once started, the swaps block for less -- the pass now ends just
        // before the refresh -- so the rhythm alone is what keeps it going.
        std::copy(blocked_, blocked_ + interval_count_, sorted);
        std::nth_element(sorted, sorted + interval_count_ / 2, sorted + interval_count_);
        if (sorted[interval_count_ / 2] < kSwapWaitsAtLeastUs) {
            return;
        }
    }
    active_ = true;
    period_us_ = typical;
}

uint64_t FrameDelay::wait_us() const {
    if (disabled_ || !active_ || period_us_ == 0 || work_count_ == 0) {
        return 0;
    }
    const uint64_t needed = work_estimate_us() + margin_us();
    if (needed + kShortestWaitUs >= period_us_) {
        return 0;
    }
    return period_us_ - needed;
}

}  // namespace ds
