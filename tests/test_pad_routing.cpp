// Which pad is the GamePad and which is which player, on a Wii U. See
// input/pad_routing.hpp.

#include "test_support.hpp"

#include <map>
#include <string>
#include <vector>

#include "input/pad_routing.hpp"
#include "ui/controllers_screen.hpp"

namespace {

using ds::KnownPad;
using ds::PadSlot;

KnownPad pad(bool here, int position, uint32_t id = 0) {
    KnownPad result;
    result.on_this_device = here;
    result.position = position;
    result.id = id;
    return result;
}

KnownPad keyboard(bool here) {
    KnownPad result;
    result.on_this_device = here;
    result.keyboard = true;
    return result;
}

void test_slots_are_written_and_named_as_people_read_them() {
    CHECK(ds::pad_slot_key(PadSlot::gamepad()) == "gamepad");
    CHECK(ds::pad_slot_key(PadSlot::player_number(0)) == "player1");
    CHECK(ds::pad_slot_key(PadSlot::player_number(7)) == "player8");
    CHECK(ds::pad_slot_key(PadSlot::not_used()) == "unused");
    CHECK(ds::pad_slot_name(PadSlot::player_number(2)) == "Player 3");
    CHECK(ds::pad_slot_name(PadSlot::gamepad()) == "GamePad");

    for (const char* key : {"gamepad", "player1", "player5", "player8", "unused"}) {
        PadSlot slot;
        CHECK_MSG(ds::pad_slot_from_key(key, slot) && ds::pad_slot_key(slot) == key, key);
    }
    PadSlot untouched = PadSlot::gamepad();
    for (const char* key : {"player0", "player9", "player12", "Player1", "", "tv"}) {
        CHECK_MSG(!ds::pad_slot_from_key(key, untouched), key);
    }
    CHECK(untouched == PadSlot::gamepad());

    // Round the whole ring both ways.
    PadSlot slot = PadSlot::gamepad();
    for (int step = 0; step < 10; ++step) slot = ds::next_pad_slot(slot);
    CHECK_MSG(slot == PadSlot::gamepad(), "stepping through every slot did not come back round");
    CHECK(ds::next_pad_slot(PadSlot::gamepad()) == PadSlot::player_number(0));
    CHECK(ds::next_pad_slot(PadSlot::player_number(7)) == PadSlot::not_used());
    CHECK(ds::previous_pad_slot(PadSlot::gamepad()) == PadSlot::not_used());
}

// Steam Deck docked to a television, a Switch holding the GamePad, and three
// pads in Steam's order on the Deck. Straight from the order: the Switch's
// first controller is the GamePad, and the Deck's are Players 1 to 3 --
// Steam's order is the one a player rearranges, so it is the one followed.
void test_defaults_come_straight_from_the_order() {
    const std::vector<KnownPad> pads{
        pad(true, 0), pad(true, 1), pad(true, 2), pad(false, 0), keyboard(true),
    };
    const std::vector<PadSlot> slots = ds::assign_pads(pads, {}, /*gamepad_here=*/false);
    CHECK_MSG(slots[0] == PadSlot::player_number(0), ds::pad_slot_name(slots[0]));
    CHECK_MSG(slots[1] == PadSlot::player_number(1), ds::pad_slot_name(slots[1]));
    CHECK_MSG(slots[2] == PadSlot::player_number(2), ds::pad_slot_name(slots[2]));
    CHECK_MSG(slots[3] == PadSlot::gamepad(), "the companion holding the GamePad is not it");
    CHECK_MSG(slots[4] == PadSlot::not_used(),
              "the keyboard on the device not holding the GamePad went somewhere");
}

// The same Deck in the hand with nothing connected: its first controller and
// its keys are the GamePad, and the next controller is Player 1.
void test_the_device_holding_the_gamepad_gives_it_its_first_controller_and_keys() {
    const std::vector<KnownPad> pads{pad(true, 1), pad(true, 0), keyboard(true)};
    const std::vector<PadSlot> slots = ds::assign_pads(pads, {}, true);
    CHECK_MSG(slots[1] == PadSlot::gamepad(), "the first in order was not the GamePad");
    CHECK(slots[0] == PadSlot::player_number(0));
    CHECK(slots[2] == PadSlot::gamepad());
}

// A companion with more than one controller: its first is the GamePad, the
// rest are players after this device's.
void test_a_companions_extra_controllers_are_players_after_this_devices() {
    const std::vector<KnownPad> pads{pad(false, 0), pad(false, 1), pad(true, 0)};
    const std::vector<PadSlot> slots = ds::assign_pads(pads, {}, false);
    CHECK(slots[0] == PadSlot::gamepad());
    CHECK_MSG(slots[2] == PadSlot::player_number(0), "this device's controller did not come first");
    CHECK_MSG(slots[1] == PadSlot::player_number(1), ds::pad_slot_name(slots[1]));
}

// What the player chooses in the Controllers screen holds, and the defaults
// fill in around it -- including a docked Deck's own controls set aside.
void test_a_choice_holds_and_the_rest_fill_in_around_it() {
    const std::vector<KnownPad> pads{pad(true, 0), pad(true, 1), pad(true, 2), pad(false, 0)};
    const std::map<std::string, std::string> chosen{
        {"this:1", "unused"},
        {"this:3", "player1"},
    };
    const std::vector<PadSlot> slots = ds::assign_pads(pads, chosen, false);
    CHECK(slots[0] == PadSlot::not_used());
    CHECK_MSG(slots[2] == PadSlot::player_number(0), "the chosen player did not hold");
    CHECK_MSG(slots[1] == PadSlot::player_number(1),
              "a default took a player number somebody had chosen: " + ds::pad_slot_name(slots[1]));
    CHECK(slots[3] == PadSlot::gamepad());

    // Choosing a GamePad from this device means the companion's first
    // controller is not also made one.
    const std::vector<PadSlot> moved = ds::assign_pads(pads, {{"this:2", "gamepad"}}, false);
    CHECK(moved[1] == PadSlot::gamepad());
    CHECK_MSG(moved[3] == PadSlot::player_number(2),
              "the GamePad was chosen elsewhere and the companion's controller still took it");
}

void test_pads_are_known_and_labelled_by_their_place() {
    CHECK(ds::pad_key(pad(true, 0)) == "this:1");
    CHECK(ds::pad_key(pad(false, 2)) == "companion:3");
    CHECK(ds::pad_key(keyboard(false)) == "companion:keyboard");
    CHECK(ds::pad_label(pad(true, 3)) == "Controller 4");
    CHECK(ds::pad_label(keyboard(true)) == "Keyboard");
}

// More controllers than players: the ninth is nobody's rather than doubling up.
void test_a_ninth_controller_is_nobodys() {
    std::vector<KnownPad> pads;
    for (int position = 0; position < 9; ++position) pads.push_back(pad(true, position));
    const std::vector<PadSlot> slots = ds::assign_pads(pads, {}, false);
    for (int index = 0; index < 8; ++index) CHECK(slots[index] == PadSlot::player_number(index));
    CHECK(slots[8] == PadSlot::not_used());
}

// Each slot gets exactly its own pads' presses, merged the way two pads on
// one console are.
void test_each_slot_gets_its_own_pads_presses() {
    std::vector<KnownPad> pads{pad(true, 0), pad(true, 1), pad(false, 0)};
    pads[0].held.set(ds::DsButton::A, true);
    pads[0].held.circle_x = 1000;
    pads[1].held.set(ds::DsButton::B, true);
    pads[1].held.circle_x = -20000;
    pads[2].held.set(ds::DsButton::X, true);
    pads[2].held.motion.present = true;
    pads[2].held.motion.gyro_x = 1.5f;
    const std::vector<PadSlot> slots{PadSlot::player_number(0), PadSlot::player_number(0),
                                     PadSlot::gamepad()};
    const ds::RoutedPads routed = ds::route_pads(pads, slots);
    CHECK(routed.player_fed[0]);
    CHECK(routed.players[0].pressed(ds::DsButton::A) && routed.players[0].pressed(ds::DsButton::B));
    CHECK_MSG(routed.players[0].circle_x == -20000, "the stick pushed further did not win");
    CHECK(!routed.players[0].pressed(ds::DsButton::X));
    CHECK(routed.gamepad_fed && routed.gamepad.pressed(ds::DsButton::X));
    CHECK(routed.gamepad.motion.present && routed.gamepad.motion.gyro_x == 1.5f);
    for (int player = 1; player < ds::kPadPlayers; ++player) CHECK(!routed.player_fed[player]);
    CHECK(ds::pads_in_slot(slots, PadSlot::player_number(0)).size() == 2);
}

// The Controllers screen, worked with a pad: each press moves the focused
// controller on one slot, and what it chose holds while the rest fill in.
void test_the_controllers_screen_steps_a_pad_through_the_slots() {
    const std::vector<KnownPad> pads = {pad(true, 0, 10), pad(true, 1, 11), keyboard(true)};
    std::map<std::string, std::string> choices;
    ds::ControllersScreen screen;
    screen.open();
    const auto press = [&](ds::UiAction action) {
        return screen.update({action}, {}, 1280.0f, 800.0f, pads, true, choices);
    };

    // Controller 1 is the GamePad by default; one step on is Player 1, and
    // the GamePad, which still needs holding, falls to the next controller on
    // this device nobody has chosen for anything: Controller 2.
    ds::ControllersScreenResult result = press(ds::UiAction::Right);
    CHECK(result.choices_changed);
    CHECK(choices["this:1"] == "player1");
    std::vector<PadSlot> slots = ds::assign_pads(pads, choices, true);
    CHECK(slots[0] == PadSlot::player_number(0));
    CHECK(slots[1] == PadSlot::gamepad());
    CHECK(slots[2] == PadSlot::gamepad());  // the keys stay with the device

    // Back the other way is the GamePad again, and one further round is Not
    // used -- the list wraps.
    press(ds::UiAction::Left);
    CHECK(choices["this:1"] == "gamepad");
    press(ds::UiAction::Left);
    CHECK(choices["this:1"] == "unused");

    // The row after the controllers puts everything back to the order.
    press(ds::UiAction::Down);
    press(ds::UiAction::Down);
    press(ds::UiAction::Down);
    CHECK(screen.focus() == 3);
    result = press(ds::UiAction::Confirm);
    CHECK(result.choices_changed);
    CHECK(choices.empty());

    // And Back leaves.
    result = press(ds::UiAction::Cancel);
    CHECK(result.close_requested);
    CHECK(!screen.is_open());
}

void test_a_pad_is_in_use_while_pressed_or_pushed() {
    ds::DsInputState held;
    CHECK(!ds::pad_in_use(held));
    held.circle_x = 2000;  // drift
    CHECK(!ds::pad_in_use(held));
    held.circle_x = -20000;
    CHECK(ds::pad_in_use(held));
    held.circle_x = 0;
    held.set(ds::DsButton::B, true);
    CHECK(ds::pad_in_use(held));
}

}  // namespace

int main() {
    test_the_controllers_screen_steps_a_pad_through_the_slots();
    test_a_pad_is_in_use_while_pressed_or_pushed();
    test_slots_are_written_and_named_as_people_read_them();
    test_defaults_come_straight_from_the_order();
    test_the_device_holding_the_gamepad_gives_it_its_first_controller_and_keys();
    test_a_companions_extra_controllers_are_players_after_this_devices();
    test_a_choice_holds_and_the_rest_fill_in_around_it();
    test_pads_are_known_and_labelled_by_their_place();
    test_a_ninth_controller_is_nobodys();
    test_each_slot_gets_its_own_pads_presses();
    return test::finish("pad-routing");
}
