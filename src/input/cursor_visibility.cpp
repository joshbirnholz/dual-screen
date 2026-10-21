#include "input/cursor_visibility.hpp"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

namespace ds {

const char* to_string(CursorMode mode) {
    // As the Hide Cursor switch reads: on when it is hidden.
    return mode == CursorMode::Hidden ? "On" : "Off";
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

bool device_has_pointer() {
#if defined(__SWITCH__) || defined(__ANDROID__)
    return false;
#elif defined(__APPLE__) && TARGET_OS_IPHONE
    // An iPhone, an iPad and an Apple TV alike: TARGET_OS_IPHONE covers all
    // three.
    return false;
#else
    return true;
#endif
}

}  // namespace ds
