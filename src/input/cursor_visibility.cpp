#include "input/cursor_visibility.hpp"

namespace ds {

const char* to_string(CursorMode mode) {
    return mode == CursorMode::Hidden ? "Off" : "On";
}

CursorMode next_cursor_mode(CursorMode mode) {
    return mode == CursorMode::Shown ? CursorMode::Hidden : CursorMode::Shown;
}

const char* cursor_mode_key(CursorMode mode) {
    return mode == CursorMode::Hidden ? "hidden" : "shown";
}

CursorMode cursor_mode_from_key(const std::string& key) {
    // "auto" was what the old three-way version called this one.
    if (key == "hidden") return CursorMode::Hidden;
    return CursorMode::Shown;
}

}  // namespace ds
