#include "util/log.hpp"

#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <mutex>
#include <thread>

#ifdef __ANDROID__
#include <android/log.h>
#endif

namespace ds {
namespace {

std::mutex g_mutex;
LogLevel g_min_level = LogLevel::Info;
std::FILE* g_file = nullptr;

// The file is written by a thread of its own. Written and flushed in place,
// each line was a trip to the storage underneath on whatever thread logged
// it -- the game's, mostly -- and on a Switch, where that is the SD card,
// one took long enough that a pair of them cost a whole frame: a Mario Kart
// race streamed to a Deck hitched every time its speed report was written.
// Lines are queued here instead, in order, and the writer takes them all at
// once and flushes. A warning or an error still waits until it is on the
// card, since that is the line a crash right after it needs to have left.
std::deque<std::string> g_pending;
std::condition_variable g_wake;
std::condition_variable g_written;
std::thread g_writer;
bool g_stopping = false;
uint64_t g_queued = 0;   // lines queued, ever
uint64_t g_flushed = 0;  // of those, on the file

void writer_loop() {
    std::unique_lock<std::mutex> lock(g_mutex);
    while (true) {
        g_wake.wait(lock, [] { return g_stopping || !g_pending.empty(); });
        if (g_pending.empty()) {
            return;  // stopping, with everything written
        }
        std::deque<std::string> lines;
        lines.swap(g_pending);
        std::FILE* const file = g_file;
        lock.unlock();
        for (const std::string& line : lines) {
            std::fputs(line.c_str(), file);
        }
        std::fflush(file);
        lock.lock();
        g_flushed += lines.size();
        g_written.notify_all();
    }
}

// Under g_mutex. Everything queued is on the file when this returns.
void stop_writer(std::unique_lock<std::mutex>& lock) {
    if (!g_writer.joinable()) return;
    g_stopping = true;
    g_wake.notify_all();
    lock.unlock();
    g_writer.join();
    lock.lock();
    g_stopping = false;
}

const char* level_tag(LogLevel level) {
    switch (level) {
        case LogLevel::Debug: return "debug";
        case LogLevel::Info:  return "info ";
        case LogLevel::Warn:  return "warn ";
        case LogLevel::Error: return "error";
    }
    return "?????";
}

// Declared after everything above, so destroyed before it: a program that
// ends without close_log_file() still has its last lines written, and its
// writer stopped before the thread object that holds it is torn down --
// which, still running, would end the process on the way out.
struct CloseAtExit {
    ~CloseAtExit() { close_log_file(); }
} g_close_at_exit;

}  // namespace

void set_min_log_level(LogLevel level) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_min_level = level;
}

bool set_log_file(const std::string& path) {
    std::unique_lock<std::mutex> lock(g_mutex);
    stop_writer(lock);
    if (g_file != nullptr) {
        std::fclose(g_file);
        g_file = nullptr;
    }
    // Truncated per run: the interesting failure is always the most recent one,
    // and an unbounded log on a handheld is its own problem.
    g_file = std::fopen(path.c_str(), "w");
    if (g_file == nullptr) return false;
    g_writer = std::thread(writer_loop);
    return true;
}

void close_log_file() {
    std::unique_lock<std::mutex> lock(g_mutex);
    stop_writer(lock);
    if (g_file != nullptr) {
        std::fclose(g_file);
        g_file = nullptr;
    }
}

void flush_log_file() {
    std::unique_lock<std::mutex> lock(g_mutex);
    const uint64_t queued = g_queued;
    g_written.wait(lock, [queued] { return g_flushed >= queued || !g_writer.joinable(); });
}

void log_message(LogLevel level, const std::string& message) {
    std::unique_lock<std::mutex> lock(g_mutex);
    // The file always gets everything, whatever the console level is set to:
    // when someone is reading the log it is because something went wrong.
    if (g_file != nullptr && g_writer.joinable()) {
        g_pending.push_back(std::string("[") + level_tag(level) + "] " + message + "\n");
        const uint64_t mine = ++g_queued;
        g_wake.notify_one();
        if (level >= LogLevel::Warn) {
            g_written.wait(lock, [mine] { return g_flushed >= mine || !g_writer.joinable(); });
        }
    }
    if (level < g_min_level) {
        return;
    }
#ifdef __ANDROID__
    // There is no terminal behind an Android app and nothing reads its stdout,
    // so the log goes where `adb logcat -s dualscreen` will find it. The file
    // above is still written, for reading after the fact without a computer
    // attached.
    static const int kAndroidPriority[] = {ANDROID_LOG_DEBUG, ANDROID_LOG_INFO,
                                           ANDROID_LOG_WARN, ANDROID_LOG_ERROR};
    __android_log_write(kAndroidPriority[static_cast<int>(level)], "dualscreen", message.c_str());
#else
    std::FILE* out = (level == LogLevel::Error || level == LogLevel::Warn) ? stderr : stdout;
    std::fprintf(out, "[dualscreen][%s] %s\n", level_tag(level), message.c_str());
    std::fflush(out);
#endif
}

void log_debug(const std::string& message) { log_message(LogLevel::Debug, message); }
void log_info(const std::string& message) { log_message(LogLevel::Info, message); }
void log_warn(const std::string& message) { log_message(LogLevel::Warn, message); }
void log_error(const std::string& message) { log_message(LogLevel::Error, message); }

}  // namespace ds
