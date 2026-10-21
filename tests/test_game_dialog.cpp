// A game's own dialogs, shown as this app's popups: a message answered with
// its buttons, and the keyboard typed into.

#include <vector>

#include "test_support.hpp"
#include "ui/game_dialog.hpp"
#include "ui/mirrored_game_dialog.hpp"

using namespace ds;

namespace {

constexpr float kWidth = 1280.0f;
constexpr float kHeight = 720.0f;

ExternalGameDialog message(std::vector<std::string> buttons) {
    ExternalGameDialog dialog;
    dialog.kind = ExternalGameDialogKind::Message;
    dialog.serial = 0x80000001u;
    dialog.text = "Error Code: 102-2802\nCould not connect.";
    dialog.buttons = std::move(buttons);
    return dialog;
}

GameDialogResult press(GameDialogScreen& screen, UiAction action) {
    return screen.update({action}, {}, kWidth, kHeight);
}

// A message with no buttons of its own is put away with OK -- A or B.
void test_a_message_is_answered_with_ok() {
    GameDialogScreen screen;
    screen.show(message({}));
    CHECK(screen.is_open() && !screen.typing());
    GameDialogResult result = press(screen, UiAction::Down);
    CHECK(!result.answered);
    result = press(screen, UiAction::Cancel);
    CHECK_MSG(result.answered, "B did not put away a message with only OK");
    CHECK_EQ(result.answer.button, 0);
    CHECK_EQ(result.answer.serial, 0x80000001u);
    CHECK(!screen.is_open());
}

// With two, the cursor starts on the first; Right and A press the second; B
// leaves the choice to be made.
void test_a_question_is_the_players_to_answer() {
    GameDialogScreen screen;
    screen.show(message({"Retry", "Cancel"}));
    CHECK(!press(screen, UiAction::Cancel).answered);
    CHECK(screen.is_open());
    press(screen, UiAction::Right);
    const GameDialogResult result = press(screen, UiAction::Confirm);
    CHECK(result.answered);
    CHECK_EQ(result.answer.button, 1);
}

// A tap made on the other device's drawing of it, as the button it landed
// on: pressed by the one tap, as a Switch's are.
void test_a_tap_from_either_device_presses_a_button() {
    GameDialogScreen screen;
    screen.show(message({"Retry", "Cancel"}));
    screen.press_entry(1);
    const GameDialogResult result = screen.update({}, {}, kWidth, kHeight);
    CHECK(result.answered);
    CHECK_EQ(result.answer.button, 1);
}

// The keyboard: what is typed, OK handing it over; Cancel handing back what
// the game started with; the game's limit kept.
void test_the_keyboard_types_and_hands_back() {
    ExternalGameDialog keyboard;
    keyboard.kind = ExternalGameDialogKind::Keyboard;
    keyboard.serial = 3;
    keyboard.text = "Epona";
    keyboard.max_length = 8;

    GameDialogScreen screen;
    screen.show(keyboard);
    CHECK(screen.typing());
    screen.type("", 5);
    screen.update({}, {}, kWidth, kHeight);
    screen.type("Roach the Horse", 0);
    screen.update({}, {}, kWidth, kHeight);
    GameDialogResult result = press(screen, UiAction::Confirm);
    CHECK(result.answered);
    CHECK(result.answer.text == "Roach th");
    CHECK_EQ(result.answer.serial, 3u);
    CHECK(!screen.typing());

    // Typed where the game is -- Cemu's window, which has the keyboard --
    // and shown here as it is.
    screen.show(keyboard);
    ExternalGameDialog typed_there = keyboard;
    typed_there.text = "Agro";
    screen.refresh(typed_there);
    result = press(screen, UiAction::Confirm);
    CHECK(result.answer.text == "Agro");

    screen.show(keyboard);
    screen.type("x", 0);
    screen.update({}, {}, kWidth, kHeight);
    result = press(screen, UiAction::Cancel);
    CHECK(result.answered);
    CHECK(result.answer.text == "Epona");
}

// On the companion, when it is the GamePad: typed there, what is typed goes
// over as it is typed, then the answer; and it is gone once answered.
void test_the_companion_types_into_the_hosts_keyboard() {
    net::GameDialogMessage host;
    host.kind = 2;
    host.serial = 4;
    host.text = "Epona";
    host.max_length = 10;
    host.typed_here = true;

    MirroredGameDialog mirror;
    CHECK(mirror.follow(host));
    CHECK(mirror.typing_here());
    mirror.type("!", 0);
    std::vector<net::GameDialogAnswerMessage> said = mirror.update({}, {}, kWidth, kHeight);
    CHECK_EQ(said.size(), std::size_t{1});
    if (!said.empty()) CHECK(!said[0].final && said[0].text == "Epona!" && said[0].serial == 4u);
    // The host echoes it back; typed here, this device's field stands.
    host.text = "Epona!";
    CHECK(mirror.follow(host));
    CHECK(mirror.update({}, {}, kWidth, kHeight).empty());
    said = mirror.update({UiAction::Confirm}, {}, kWidth, kHeight);
    CHECK_EQ(said.size(), std::size_t{1});
    if (!said.empty()) CHECK(said[0].final && said[0].text == "Epona!");
    CHECK_MSG(!mirror.follow(host), "an answered dialog came back while the host caught up");
    host.kind = 0;
    CHECK(!mirror.follow(host));

    // Not the GamePad: shown as typed on the host, no keyboard here.
    net::GameDialogMessage elsewhere = host;
    elsewhere.kind = 2;
    elsewhere.serial = 5;
    elsewhere.typed_here = false;
    CHECK(mirror.follow(elsewhere));
    CHECK(!mirror.typing_here());
    elsewhere.text = "Agro";
    mirror.follow(elsewhere);
    said = mirror.update({UiAction::Confirm}, {}, kWidth, kHeight);
    CHECK(!said.empty() && said.back().text == "Agro");
}

}  // namespace

int main() {
    test_a_message_is_answered_with_ok();
    test_a_question_is_the_players_to_answer();
    test_a_tap_from_either_device_presses_a_button();
    test_the_keyboard_types_and_hands_back();
    test_the_companion_types_into_the_hosts_keyboard();
    return test::finish("game-dialog");
}
