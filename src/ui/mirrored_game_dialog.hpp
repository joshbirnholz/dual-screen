#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "net/protocol.hpp"
#include "ui/game_dialog.hpp"

namespace ds {

// The host's game dialog, shown on the companion: the same popup, answered
// here as it would be there. When the companion is the GamePad -- the device
// in the player's hands -- the keyboard is typed here, and what is typed goes
// over as it is typed, so the host's popup shows it too.
class MirroredGameDialog {
public:
    // Follows what the host says is up. True while something is.
    bool follow(const net::GameDialogMessage& host);
    bool is_open() const { return screen_.is_open(); }
    // Whether this device's keyboard should be up.
    bool typing_here() const { return screen_.typing() && typed_here_; }
    void type(const std::string& typed, int backspaces) { screen_.type(typed, backspaces); }
    // Answered some other way -- a Switch's own keyboard -- so shown no
    // longer, however long the host takes to say so.
    void answered(uint32_t serial) {
        answered_ = serial;
        any_answered_ = true;
        screen_.close();
    }

    // What to tell the host this pass, oldest first: the field as typed, and
    // the answer once there is one.
    std::vector<net::GameDialogAnswerMessage> update(const std::vector<UiAction>& actions,
                                                     const std::vector<PointerEvent>& pointers,
                                                     float output_width, float output_height);
    void render(QuadRenderer& quads, TextRenderer& text, float output_width, float output_height) {
        screen_.render(quads, text, output_width, output_height);
    }

private:
    GameDialogScreen screen_;
    uint32_t serial_ = 0;
    bool typed_here_ = false;
    // The field as last sent, so only a change goes over.
    std::string sent_;
    // Answered here: shown no longer, however long the host takes to say so.
    uint32_t answered_ = 0;
    bool any_answered_ = false;
};

}  // namespace ds
