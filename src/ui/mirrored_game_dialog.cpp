#include "ui/mirrored_game_dialog.hpp"

namespace ds {

namespace {

ExternalGameDialog from_wire(const net::GameDialogMessage& message) {
    ExternalGameDialog dialog;
    dialog.kind = message.kind <= 2 ? static_cast<ExternalGameDialogKind>(message.kind) : ExternalGameDialogKind::None;
    dialog.serial = message.serial;
    dialog.title = message.title;
    dialog.text = message.text;
    dialog.buttons = message.buttons;
    dialog.max_length = message.max_length;
    return dialog;
}

}  // namespace

bool MirroredGameDialog::follow(const net::GameDialogMessage& host) {
    if (host.kind == 0 || (any_answered_ && host.serial == answered_)) {
        screen_.close();
        return false;
    }
    const ExternalGameDialog dialog = from_wire(host);
    if (!screen_.is_open() || host.serial != serial_) {
        screen_.show(dialog);
        serial_ = host.serial;
        sent_ = host.text;
    } else if (!host.typed_here || !typed_here_) {
        // Typed on the host -- or in Cemu's window -- and shown here as it
        // is. Typed here, this device's field is the one that counts.
        screen_.refresh(dialog);
        sent_ = host.text;
    }
    typed_here_ = host.typed_here;
    return true;
}

std::vector<net::GameDialogAnswerMessage> MirroredGameDialog::update(const std::vector<UiAction>& actions,
                                                                     const std::vector<PointerEvent>& pointers,
                                                                     float output_width, float output_height) {
    std::vector<net::GameDialogAnswerMessage> said;
    if (!screen_.is_open()) return said;
    const GameDialogResult result = screen_.update(actions, pointers, output_width, output_height);
    if (result.answered) {
        net::GameDialogAnswerMessage answer;
        answer.serial = serial_;
        answer.final = true;
        answer.button = result.answer.button;
        answer.text = result.answer.text;
        said.push_back(std::move(answer));
        answered_ = serial_;
        any_answered_ = true;
        return said;
    }
    const ExternalGameDialog now = screen_.current();
    if (now.kind == ExternalGameDialogKind::Keyboard && now.text != sent_) {
        net::GameDialogAnswerMessage typing;
        typing.serial = serial_;
        typing.final = false;
        typing.text = now.text;
        said.push_back(std::move(typing));
        sent_ = now.text;
    }
    return said;
}

}  // namespace ds
