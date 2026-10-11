#pragma once

#include "input/input_manager.hpp"
#include "net/protocol.hpp"

namespace ds {

// There is one menu per session, held by whichever device runs the game. These
// carry a press from the device that made it to the device that answers it.
// Shared by both clients so the two ends cannot disagree about what a button
// meant.

// Only the presses a menu answers travel; everything else a player does while
// one is up belongs to the device it happened on.
inline bool menu_action_for(UiAction action, net::MenuAction& out) {
    switch (action) {
        case UiAction::Up: out = net::MenuAction::Up; return true;
        case UiAction::Down: out = net::MenuAction::Down; return true;
        case UiAction::Left: out = net::MenuAction::Left; return true;
        case UiAction::Right: out = net::MenuAction::Right; return true;
        case UiAction::Confirm: out = net::MenuAction::Confirm; return true;
        case UiAction::Cancel: out = net::MenuAction::Cancel; return true;
        case UiAction::ToggleMenu: out = net::MenuAction::Close; return true;
        case UiAction::MenuQuit: out = net::MenuAction::Quit; return true;
        case UiAction::MenuReset: out = net::MenuAction::Reset; return true;
        case UiAction::MenuDisconnect: out = net::MenuAction::Disconnect; return true;
        case UiAction::MenuSettings: out = net::MenuAction::Settings; return true;
        default: break;
    }
    return false;
}

inline UiAction ui_action_for(net::MenuAction action) {
    switch (action) {
        case net::MenuAction::Up: return UiAction::Up;
        case net::MenuAction::Down: return UiAction::Down;
        case net::MenuAction::Left: return UiAction::Left;
        case net::MenuAction::Right: return UiAction::Right;
        case net::MenuAction::Cancel: return UiAction::Cancel;
        case net::MenuAction::Close: return UiAction::ToggleMenu;
        case net::MenuAction::Quit: return UiAction::MenuQuit;
        case net::MenuAction::Reset: return UiAction::MenuReset;
        case net::MenuAction::Disconnect: return UiAction::MenuDisconnect;
        case net::MenuAction::Settings: return UiAction::MenuSettings;
        case net::MenuAction::Confirm: break;
    }
    return UiAction::Confirm;
}

}  // namespace ds
