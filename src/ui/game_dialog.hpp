#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/external_frame_link.hpp"
#include "input/input_manager.hpp"
#include "render/quad_renderer.hpp"
#include "render/text_renderer.hpp"
#include "ui/menu_page.hpp"
#include "ui/popup.hpp"
#include "ui/text_entry.hpp"

namespace ds {

struct GameDialogResult {
    // The dialog answered, and what with: it is put away here at once, and
    // the game puts its own away once it hears.
    bool answered = false;
    ExternalGameDialogAnswer answer;
};

// A dialog a game put up -- an error, a question, its keyboard -- shown as a
// popup like every other of this app's, in place of the emulator's own small
// window (see kExternalFeatureGameDialogs).
//
// A message's buttons are the game's, the first under the cursor; with only
// one, B presses it too. The keyboard is the popup a DS nickname is typed in:
// typed on a keyboard or Steam's on-screen one, OK handing the game what was
// typed, Cancel handing it back what it started with.
class GameDialogScreen : public MenuPage {
public:
    void show(const ExternalGameDialog& dialog);
    // The same dialog said again by the game: a keyboard's field as typed
    // where the game is.
    void refresh(const ExternalGameDialog& dialog);
    // The dialog as it stands, the keyboard's field as typed so far: for the
    // other device to show the same.
    ExternalGameDialog current() const;
    void close() { open_ = false; }
    bool is_open() const { return open_; }
    uint32_t serial() const { return serial_; }
    // Whether something is being typed, so the keyboard should be up.
    bool typing() const { return open_ && kind_ == ExternalGameDialogKind::Keyboard; }
    // What was typed this frame, for the keyboard -- see InputManager.
    void type(const std::string& typed, int backspaces) {
        typed_ = typed;
        backspaces_ = backspaces;
    }

    GameDialogResult update(const std::vector<UiAction>& actions, const std::vector<PointerEvent>& pointers,
                            float output_width, float output_height);
    void render(QuadRenderer& quads, TextRenderer& text, float output_width, float output_height);

    // Nothing scrolls; a message's buttons are its entries; a game's
    // question is answered rather than tapped away.
    bool scroll(const std::vector<PointerEvent>&, float, float) override { return false; }
    int entry_at(float x, float y, float output_width, float output_height) const override;
    bool off_popup(float, float, float, float) const override { return false; }

    // For the sound judge: what a press could have changed.
    int focus() const { return kind_ == ExternalGameDialogKind::Keyboard ? 0 : cursor_.focus(); }

private:
    PopupText words() const;
    std::vector<PopupButton> buttons() const;
    PopupGeometry geometry(float output_width, float output_height) const;

    bool open_ = false;
    ExternalGameDialogKind kind_ = ExternalGameDialogKind::None;
    uint32_t serial_ = 0;
    std::string title_;
    std::string text_;
    std::vector<std::string> button_labels_;
    uint32_t max_length_ = 0;
    // What the keyboard's field held to begin with, which Cancel hands back.
    std::string initial_;
    PopupCursor cursor_;
    TextEntryPopup entry_;
    std::string typed_;
    int backspaces_ = 0;
};

}  // namespace ds
