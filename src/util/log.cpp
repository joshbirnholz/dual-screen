#include "util/log.hpp"

#include <cstdio>
#include <mutex>

namespace ds {
namespace {

std::mutex g_mutex;
LogLevel g_min_level = LogLevel::Info;
std::FILE* g_file = nullptr;

const char* level_tag(LogLevel level) {
    switch (level) {
        case LogLevel::Debug: return "debug";
        case LogLevel::Info:  return "info ";
        case LogLevel::Warn:  return "warn ";
        case LogLevel::Error: return "error";
    }
    return "?????";
}

}  // namespace

void set_min_log_level(LogLevel level) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_min_level = level;
}

bool set_log_file(const std::string& path) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file != nullptr) {
        std::fclose(g_file);
        g_file = nullptr;
    }
    // Truncated per run: the interesting failure is always the most recent one,
    // and an unbounded log on a handheld is its own problem.
    g_file = std::fopen(path.c_str(), "w");
    return g_file != nullptr;
}

void close_log_file() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file != nullptr) {
        std::fclose(g_file);
        g_file = nullptr;
    }
}

void log_message(LogLevel level, const std::string& message) {
    std::lock_guard<std::mutex> lock(g_mutex);
    // The file always gets everything, whatever the console level is set to:
    // when someone is reading the log it is because something went wrong.
    if (g_file != nullptr) {
        std::fprintf(g_file, "[%s] %s\n", level_tag(level), message.c_str());
        std::fflush(g_file);
    }
    if (level < g_min_level) {
        return;
    }
    std::FILE* out = (level == LogLevel::Error || level == LogLevel::Warn) ? stderr : stdout;
    std::fprintf(out, "[dualscreen][%s] %s\n", level_tag(level), message.c_str());
    std::fflush(out);
}

void log_debug(const std::string& message) { log_message(LogLevel::Debug, message); }
void log_info(const std::string& message) { log_message(LogLevel::Info, message); }
void log_warn(const std::string& message) { log_message(LogLevel::Warn, message); }
void log_error(const std::string& message) { log_message(LogLevel::Error, message); }

}  // namespace ds
