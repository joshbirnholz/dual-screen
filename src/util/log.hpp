#pragma once

#include <string>

namespace ds {

enum class LogLevel { Debug, Info, Warn, Error };

// Minimal logging shim. Everything (including messages the libretro core emits
// through its log interface) funnels through here so a future companion/host
// split has a single place to tee logs from.
void log_message(LogLevel level, const std::string& message);
void set_min_log_level(LogLevel level);

// Also append everything to this file, starting a fresh one each run. In Game
// Mode there is no terminal to print to, so without this a startup failure is
// completely silent -- the app just vanishes. Returns false if the file cannot
// be opened, which is not itself fatal.
bool set_log_file(const std::string& path);
void close_log_file();
// Waits until every line logged so far is on the file -- before anything that
// may end the process without closing it. See log.cpp for why lines wait.
void flush_log_file();

void log_debug(const std::string& message);
void log_info(const std::string& message);
void log_warn(const std::string& message);
void log_error(const std::string& message);

}  // namespace ds
