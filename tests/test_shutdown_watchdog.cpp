// The way out of the app always leaves, even when a step of it never returns.
// See ShutdownWatchdog.

#include "test_support.hpp"

#include <chrono>
#include <string>
#include <thread>

#include "util/shutdown_watchdog.hpp"

namespace {

using namespace std::chrono_literals;

// A way out that finishes in time costs nothing and ends nothing.
void test_a_way_out_that_finishes_is_left_alone() {
    bool stuck = false;
    {
        ds::ShutdownWatchdog watchdog(2000ms, [&](const std::string&) { stuck = true; });
        watchdog.at("closing the audio device");
        watchdog.at("closing the window");
        watchdog.finish();
    }
    CHECK_MSG(!stuck, "a way out that finished in time was treated as stuck");
}

// One that does not finish is ended, and says which step it was on -- which
// is the one line a log needs to say what hung.
//
// Found on a Steam Deck: after Cemu was closed, the app sat on "Playing on
// Cemu" with the menu keys doing nothing and had to be powered off, and the
// log could not say which part of leaving it was waiting on.
void test_a_way_out_that_hangs_is_ended_naming_the_step() {
    std::string stuck_at;
    {
        ds::ShutdownWatchdog watchdog(100ms, [&](const std::string& step) { stuck_at = step; });
        watchdog.at("stopping the emulator this app started");
        // The step that never returns, for longer than the deadline.
        std::this_thread::sleep_for(400ms);
        watchdog.finish();
    }
    CHECK_MSG(stuck_at == "stopping the emulator this app started",
              "the watchdog named '" + stuck_at + "' rather than the step that hung");
}

// Stuck before any step was named still says something.
void test_stuck_before_the_first_step_still_says_so() {
    std::string stuck_at;
    {
        ds::ShutdownWatchdog watchdog(50ms, [&](const std::string& step) { stuck_at = step; });
        std::this_thread::sleep_for(250ms);
    }
    CHECK(!stuck_at.empty());
}

}  // namespace

int main() {
    test_a_way_out_that_finishes_is_left_alone();
    test_a_way_out_that_hangs_is_ended_naming_the_step();
    test_stuck_before_the_first_step_still_says_so();
    return test::finish("shutdown-watchdog");
}
