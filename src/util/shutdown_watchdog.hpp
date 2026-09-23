#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "util/log.hpp"

namespace ds {

// Makes sure leaving actually leaves.
//
// Everything this app does on the way out -- telling a companion goodbye,
// closing the audio device, stopping the emulator it started, letting go of
// the controllers, closing the window -- is a call into something else, and
// any of them can wait for ever on a thing that is not coming back. A Steam
// Deck that had closed Cemu sat on its last frame, "Playing on Cemu", with
// F1 and Tab doing nothing, because the loop that answers them had already
// stopped and whatever the way out was waiting on never answered. In Game
// Mode that is a window that cannot be closed from anywhere but the power
// button.
//
// So each step says what it is, which is what makes the log able to name the
// one that stuck, and if leaving has not finished by the deadline the process
// ends without it. Nothing worth keeping is lost by that: settings and the
// game's own configuration are written before any of the steps that can hang.
class ShutdownWatchdog {
public:
    using StuckHandler = std::function<void(const std::string& step)>;

    // What happens by default when leaving is stuck: say which step, and end
    // the process without finishing it. _Exit rather than exit, because exit
    // runs the same destructors and atexit handlers that may be what is stuck.
    static void end_the_process(const std::string& step) {
        log_warn("leaving has been stuck for too long at: " + step +
                 "; ending without finishing it");
        close_log_file();
        std::_Exit(0);
    }

    explicit ShutdownWatchdog(std::chrono::milliseconds deadline,
                              StuckHandler on_stuck = &ShutdownWatchdog::end_the_process)
        : on_stuck_(std::move(on_stuck)), thread_([this, deadline] { watch(deadline); }) {}

    ~ShutdownWatchdog() { finish(); }

    ShutdownWatchdog(const ShutdownWatchdog&) = delete;
    ShutdownWatchdog& operator=(const ShutdownWatchdog&) = delete;

    // The step about to be taken, logged before it is taken.
    void at(const std::string& step) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            step_ = step;
        }
        log_info("leaving: " + step);
    }

    // Everything is done; the watchdog stands down.
    void finish() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            done_ = true;
        }
        wake_.notify_all();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

private:
    void watch(std::chrono::milliseconds deadline) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (wake_.wait_for(lock, deadline, [this] { return done_; })) {
            return;
        }
        const std::string step = step_.empty() ? std::string("before the first step") : step_;
        lock.unlock();
        on_stuck_(step);
    }

    StuckHandler on_stuck_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::string step_;
    bool done_ = false;
    // Last, so it starts only once everything it reads exists.
    std::thread thread_;
};

}  // namespace ds
