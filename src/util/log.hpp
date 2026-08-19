#pragma once

#include <string>

namespace ds {

enum class LogLevel { Debug, Info, Warn, Error };

// Minimal logging shim. Everything (including messages the libretro core emits
// through its log interface) funnels through here so a future companion/host
// split has a single place to tee logs from.
void log_message(LogLevel level, const std::string& message);
void set_min_log_level(LogLevel level);

void log_debug(const std::string& message);
void log_info(const std::string& message);
void log_warn(const std::string& message);
void log_error(const std::string& message);

}  // namespace ds
