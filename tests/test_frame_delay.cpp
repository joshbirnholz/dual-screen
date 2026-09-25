// Drawing as late as the display allows -- see render/frame_delay.hpp. Pure
// timing arithmetic, so every case here is a made-up clock rather than a
// real display.

#include "test_support.hpp"

#include "render/frame_delay.hpp"

using namespace ds;

namespace {

constexpr uint64_t kPeriod = 16667;  // 60Hz

// A display that refreshes every period, and a loop whose passes take `work`
// and whose swap waits out the rest -- which, while nothing sleeps, is most of
// the period.
void run(FrameDelay& delay, uint64_t& now, int frames, uint64_t work) {
    for (int frame = 0; frame < frames; ++frame) {
        const uint64_t wait = delay.wait_us();
        const uint64_t started = now + wait;
        const uint64_t submitted = started + work;
        delay.worked(work);
        // The next refresh at or after the submit.
        const uint64_t refresh = ((submitted / kPeriod) + 1) * kPeriod;
        now = refresh;
        delay.presented(now, refresh - submitted);
    }
}

void test_it_waits_until_just_before_the_refresh() {
    FrameDelay delay;
    uint64_t now = kPeriod;
    run(delay, now, 64, 3000);
    CHECK(delay.active());
    CHECK(delay.period_us() >= kPeriod - 100 && delay.period_us() <= kPeriod + 100);
    // A 3ms pass and the margin, and the rest spent waiting.
    const uint64_t wait = delay.wait_us();
    CHECK_MSG(wait >= 10000 && wait <= kPeriod - 3000 - FrameDelay::kBaseMarginUs,
              "waited " + std::to_string(wait) + "us");
    // And it never costs a refresh: every interval stays one period.
    const uint64_t before = now;
    run(delay, now, 60, 3000);
    CHECK_EQ(now - before, 60 * kPeriod);
}

void test_a_slow_pass_is_allowed_for() {
    FrameDelay delay;
    uint64_t now = kPeriod;
    run(delay, now, 64, 3000);
    // One pass that took 9ms shrinks the wait to fit it, for as long as it is
    // in the window.
    delay.worked(9000);
    CHECK(delay.wait_us() <= kPeriod - 9000 - FrameDelay::kBaseMarginUs);
}

void test_a_missed_refresh_widens_the_margin() {
    FrameDelay delay;
    uint64_t now = kPeriod;
    run(delay, now, 64, 3000);
    const uint64_t margin = delay.margin_us();
    // A refresh that came two periods after the last: missed.
    now += 2 * kPeriod;
    delay.presented(now, 0);
    CHECK(delay.margin_us() > margin);
}

void test_nothing_to_aim_at_without_a_refresh() {
    // A swap that returns at once, in a loop that runs as fast as it can.
    FrameDelay delay;
    uint64_t now = 1000;
    for (int frame = 0; frame < 64; ++frame) {
        delay.worked(500);
        now += 700 + static_cast<uint64_t>(frame % 5) * 300;
        delay.presented(now, 10);
    }
    CHECK(!delay.active());
    CHECK_EQ(delay.wait_us(), uint64_t{0});

    // Regular, but the swap never waits: something other than a display is
    // pacing it, and aiming at that would only add delay.
    FrameDelay unheld;
    now = 1000;
    for (int frame = 0; frame < 64; ++frame) {
        unheld.worked(3000);
        now += kPeriod;
        unheld.presented(now, 100);
    }
    CHECK(!unheld.active());
}

void test_a_pass_longer_than_the_period_waits_for_nothing() {
    FrameDelay delay;
    uint64_t now = kPeriod;
    run(delay, now, 64, 3000);
    delay.worked(15000);
    CHECK_EQ(delay.wait_us(), uint64_t{0});
}

}  // namespace

int main() {
    test_it_waits_until_just_before_the_refresh();
    test_a_slow_pass_is_allowed_for();
    test_a_missed_refresh_widens_the_margin();
    test_nothing_to_aim_at_without_a_refresh();
    test_a_pass_longer_than_the_period_waits_for_nothing();
    return test::finish("frame_delay");
}
