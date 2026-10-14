#include "ui/game_dialog.hpp"

#include <sstream>

namespace ds {

void GameDialogScreen::show(const ExternalGameDialog& dialog) {
    kind_ = dialog.kind;
    serial_ = dialog.serial;
    title_ = dialog.title;
    text_ = dialog.text;
    button_labels_ = dialog.buttons;
    max_length_ = dialog.max_length;
    initial_ = dialog.text;
    typed_.clear();
    backspaces_ = 0;
    open_ = kind_ != ExternalGameDialogKind::None;
    if (kind_ == ExternalGameDialogKind::Keyboard) {
        entry_.open(title_.empty() ? std::string("Enter Text") : title_, initial_, dialog.max_length);
    } else {
        // The game's first button under the cursor, as its own window had it.
        cursor_.reset(static_cast<int>(buttons().size()), 0);
    }
}

void GameDialogScreen::refresh(const ExternalGameDialog& dialog) {
    if (!open_ || dialog.serial != serial_ || kind_ != ExternalGameDialogKind::Keyboard) return;
    entry_.set_text(dialog.text);
}

ExternalGameDialog GameDialogScreen::current() const {
    ExternalGameDialog dialog;
    if (!open_) return dialog;
    dialog.kind = kind_;
    dialog.serial = serial_;
    dialog.title = title_;
    dialog.text = kind_ == ExternalGameDialogKind::Keyboard ? entry_.text() : text_;
    dialog.buttons = button_labels_;
    dialog.max_length = max_length_;
    return dialog;
}

PopupText GameDialogScreen::words() const {
    // The title, then the words a line at a time: an error's code, then what
    // it means.
    PopupText words;
    std::vector<std::string> lines;
    std::istringstream stream(text_);
    for (std::string line; std::getline(stream, line);) {
        if (!line.empty()) lines.push_back(line);
    }
    if (!title_.empty()) {
        words.message = title_;
    } else if (!lines.empty()) {
        words.message = lines.front();
        lines.erase(lines.begin());
    }
    words.lines = lines;
    words.plain = lines.size();
    return words;
}

std::vector<PopupButton> GameDialogScreen::buttons() const {
    std::vector<PopupButton> buttons;
    for (const std::string& label : button_labels_) buttons.push_back({label, ""});
    if (buttons.empty()) buttons.push_back({"OK", ""});
    return buttons;
}

PopupGeometry GameDialogScreen::geometry(float output_width, float output_height) const {
    const float width = popup_body_width(output_width, output_height);
    return popup_geometry(output_width, output_height, popup_text_height(words(), width, output_height),
                          buttons());
}

int GameDialogScreen::entry_at(float x, float y, float output_width, float output_height) const {
    if (!open_ || kind_ != ExternalGameDialogKind::Message) return -1;
    return popup_button_at(geometry(output_width, output_height), x, y);
}

GameDialogResult GameDialogScreen::update(const std::vector<UiAction>& actions,
                                          const std::vector<PointerEvent>& pointers, float output_width,
                                          float output_height) {
    GameDialogResult result;
    if (!open_) return result;
    result.answer.serial = serial_;
    if (kind_ == ExternalGameDialogKind::Keyboard) {
        // The field and its buttons are the text entry popup's own, finger
        // and all.
        const TextEntryResult typed =
            entry_.update(actions, pointers, typed_, backspaces_, output_width, output_height);
        typed_.clear();
        backspaces_ = 0;
        if (typed.done || typed.cancelled) {
            result.answered = true;
            result.answer.text = typed.done ? entry_.text() : initial_;
        }
    } else {
        const PopupGeometry popup = geometry(output_width, output_height);
        const int count = static_cast<int>(popup.buttons.size());
        int answer = cursor_.update(actions, {}, popup);
        for (const int tap : take_taps(pointers, output_width, output_height)) {
            if (answer != PopupCursor::kNone) break;
            answer = cursor_.tap(tap);
        }
        // B presses the only button there is; with a choice, it is the
        // game's to make, and B leaves it to be made.
        if (answer == PopupCursor::kCancelled) answer = count == 1 ? 0 : PopupCursor::kNone;
        if (answer >= 0) {
            result.answered = true;
            result.answer.button = answer;
        }
    }
    if (result.answered) open_ = false;
    return result;
}

void GameDialogScreen::render(QuadRenderer& quads, TextRenderer& text, float output_width,
                              float output_height) {
    if (!open_) return;
    if (kind_ == ExternalGameDialogKind::Keyboard) {
        entry_.render(quads, text, output_width, output_height);
        return;
    }
    const PopupGeometry popup = geometry(output_width, output_height);
    draw_popup(quads, text, output_width, output_height, popup, buttons(), cursor_.focus(),
               cursor_.shown());
    draw_popup_text(quads, text, words(), popup.body.x, popup.body.y, popup.body.w, output_height);
}

}  // namespace ds
