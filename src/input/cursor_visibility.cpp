#include "input/cursor_visibility.hpp"

namespace ds {

const char* to_string(CursorMode mode) {
    switch (mode) {
        case CursorMode::AlwaysShown: return "Always shown";
        case CursorMode::AlwaysHidden: return "Always hidden";
        case CursorMode::AutoHide: break;
    }
    return "Hide when still";
}

CursorMode next_cursor_mode(CursorMode mode) {
    switch (mode) {
        case CursorMode::AutoHide: return CursorMode::AlwaysShown;
        case CursorMode::AlwaysShown: return CursorMode::AlwaysHidden;
        case CursorMode::AlwaysHidden: break;
    }
    return CursorMode::AutoHide;
}

const char* cursor_mode_key(CursorMode mode) {
    switch (mode) {
        case CursorMode::AlwaysShown: return "shown";
        case CursorMode::AlwaysHidden: return "hidden";
        case CursorMode::AutoHide: break;
    }
    return "auto";
}

CursorMode cursor_mode_from_key(const std::string& key) {
    if (key == "shown") return CursorMode::AlwaysShown;
    if (key == "hidden") return CursorMode::AlwaysHidden;
    return CursorMode::AutoHide;
}

}  // namespace ds
