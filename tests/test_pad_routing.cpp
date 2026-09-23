// Which pad is the GamePad and which is which player, on a Wii U. See
// input/pad_routing.hpp.

#include "test_support.hpp"

#include <string>
#include <vector>

#include "input/pad_routing.hpp"
#include "ui/controllers_screen.hpp"

namespace {

using ds::KnownPad;
using ds::LineupRow;
using ds::PadLineup;
using ds::PadOrder;
using ds::PadSlot;

KnownPad pad(bool here, int position, uint32_t id = 0) {
    KnownPad result;
    result.on_this_device = here;
    result.position = position;
    result.id = id != 0 ? id : static_cast<uint32_t>(100 * (here ? 1 : 2) + position);
    return result;
}

std::vector<PadSlot> slots(const std::vector<KnownPad>& pads, const PadOrder& order,
                           bool gamepad_here) {
    return ds::slots_of(pads, ds::line_up_pads(pads, order, gamepad_here));
}

void test_slots_and_pads_are_named_as_people_read_them() {
    CHECK(ds::pad_slot_name(PadSlot::player_number(2)) == "Player 3");
    CHECK(ds::pad_slot_name(PadSlot::gamepad()) == "GamePad");
    CHECK(ds::pad_slot_name(PadSlot::not_used()) == "Not used");
    CHECK(ds::pad_label(pad(true, 3)) == "Controller 4");
    CHECK(ds::pad_identity(pad(true, 0, 12)) == "this#12");
    CHECK(ds::pad_identity(pad(false, 0, 3)) == "companion#3");
}

// Steam Deck docked to a television, a Switch holding the GamePad, and three
// pads in Steam's order on the Deck. Straight from the order: the Switch's
// first controller is the GamePad, and the Deck's are Players 1 to 3 --
// Steam's order is the one a player rearranges, so it is the one followed.
void test_defaults_come_straight_from_the_order() {
    const std::vector<KnownPad> pads{pad(true, 0), pad(true, 1), pad(true, 2), pad(false, 0)};
    const std::vector<PadSlot> got = slots(pads, PadOrder(), /*gamepad_here=*/false);
    CHECK_MSG(got[0] == PadSlot::player_number(0), ds::pad_slot_name(got[0]));
    CHECK_MSG(got[1] == PadSlot::player_number(1), ds::pad_slot_name(got[1]));
    CHECK_MSG(got[2] == PadSlot::player_number(2), ds::pad_slot_name(got[2]));
    CHECK_MSG(got[3] == PadSlot::gamepad(), "the companion holding the GamePad is not it");
}

// The same Deck in the hand with nothing connected: its first controller is
// the GamePad and the next is Player 1, whatever order they are handed in.
void test_the_device_holding_the_gamepad_gives_it_its_first_controller() {
    const std::vector<KnownPad> pads{pad(true, 1), pad(true, 0)};
    const std::vector<PadSlot> got = slots(pads, PadOrder(), true);
    CHECK_MSG(got[1] == PadSlot::gamepad(), "the first in order was not the GamePad");
    CHECK(got[0] == PadSlot::player_number(0));
}

// A companion holding the GamePad with no controller of its own -- a phone --
// leaves the GamePad to its touch screen, and every controller here is a
// player.
void test_a_gamepad_with_no_controller_is_left_empty() {
    const std::vector<KnownPad> pads{pad(true, 0), pad(true, 1)};
    const PadLineup lineup = ds::line_up_pads(pads, PadOrder(), false);
    CHECK_EQ(lineup.gamepad, -1);
    CHECK_EQ(lineup.players.size(), static_cast<std::size_t>(2));
    const std::vector<LineupRow> rows = ds::lineup_rows(lineup);
    CHECK_EQ(rows.size(), static_cast<std::size_t>(3));
    CHECK(rows[0].slot == PadSlot::gamepad() && rows[0].pad == -1);
}

// A companion with more than one controller: its first is the GamePad, the
// rest are players after this device's.
void test_a_companions_extra_controllers_are_players_after_this_devices() {
    const std::vector<KnownPad> pads{pad(false, 0), pad(false, 1), pad(true, 0)};
    const std::vector<PadSlot> got = slots(pads, PadOrder(), false);
    CHECK(got[0] == PadSlot::gamepad());
    CHECK_MSG(got[2] == PadSlot::player_number(0), "this device's controller did not come first");
    CHECK_MSG(got[1] == PadSlot::player_number(1), ds::pad_slot_name(got[1]));
}

// More controllers than players: the ninth past the GamePad is not used,
// rather than doubling up.
void test_past_eight_players_a_controller_is_not_used() {
    std::vector<KnownPad> pads;
    for (int position = 0; position < 10; ++position) pads.push_back(pad(true, position));
    const std::vector<PadSlot> got = slots(pads, PadOrder(), true);
    CHECK(got[0] == PadSlot::gamepad());
    for (int index = 1; index < 9; ++index) CHECK(got[index] == PadSlot::player_number(index - 1));
    CHECK(got[9] == PadSlot::not_used());
}

// Moving a pad is trading places with its neighbour, the way SteamOS's own
// Controller Order screen does it -- and no two pads ever share a slot.
void test_moving_a_pad_trades_places() {
    const std::vector<KnownPad> pads{pad(true, 0), pad(true, 1), pad(true, 2)};
    PadLineup lineup = ds::line_up_pads(pads, PadOrder(), true);
    CHECK_EQ(lineup.gamepad, 0);

    // Player 1 up is the GamePad; the GamePad comes down to Player 1.
    CHECK(ds::move_in_lineup(lineup, 1, -1));
    CHECK_EQ(lineup.gamepad, 1);
    CHECK_EQ(lineup.players[0], 0);
    // Nothing is above the GamePad.
    CHECK(!ds::move_in_lineup(lineup, 1, -1));

    // The last player down is Not used; back up is the last player again.
    CHECK(ds::move_in_lineup(lineup, 2, 1));
    CHECK_EQ(lineup.players.size(), static_cast<std::size_t>(1));
    CHECK_EQ(lineup.unused.size(), static_cast<std::size_t>(1));
    CHECK(ds::slots_of(pads, lineup)[2] == PadSlot::not_used());
    CHECK(!ds::move_in_lineup(lineup, 2, 1));
    CHECK(ds::move_in_lineup(lineup, 2, -1));
    CHECK_EQ(lineup.players.size(), static_cast<std::size_t>(2));

    // However it is shuffled, every slot holds one pad at most.
    for (int step = 0; step < 40; ++step) {
        ds::move_in_lineup(lineup, step % 3, (step % 5) < 2 ? -1 : 1);
        const std::vector<PadSlot> got = ds::slots_of(pads, lineup);
        for (std::size_t a = 0; a < got.size(); ++a) {
            for (std::size_t b = a + 1; b < got.size(); ++b) {
                CHECK_MSG(got[a].kind == PadSlot::Kind::NotUsed || got[a] != got[b],
                          "two controllers were given " + ds::pad_slot_name(got[a]));
            }
        }
    }
}

// Moving the GamePad's only controller out of the way leaves it empty, and a
// controller moved up from Player 1 takes it without trading.
void test_an_empty_gamepad_is_taken_rather_than_traded() {
    const std::vector<KnownPad> pads{pad(true, 0)};
    PadLineup lineup = ds::line_up_pads(pads, PadOrder(), true);
    CHECK(ds::move_in_lineup(lineup, 0, 1));
    CHECK_EQ(lineup.gamepad, -1);
    CHECK(ds::slots_of(pads, lineup)[0] == PadSlot::not_used());
    CHECK(ds::move_in_lineup(lineup, 0, -1));
    CHECK_EQ(lineup.gamepad, 0);

    const std::vector<KnownPad> two{pad(true, 0), pad(true, 1)};
    PadLineup empty = ds::line_up_pads(two, PadOrder(), false);
    CHECK_EQ(empty.gamepad, -1);
    CHECK(ds::move_in_lineup(empty, 0, -1));
    CHECK_EQ(empty.gamepad, 0);
    CHECK_EQ(empty.players.size(), static_cast<std::size_t>(1));
}

// A rearranged line holds by identity: pads keep their places while others
// leave and arrive, a newcomer joins the end, and a line arranged with the
// GamePad on the other device is dropped rather than misapplied.
void test_a_chosen_order_holds_while_pads_come_and_go() {
    const std::vector<KnownPad> pads{pad(true, 0), pad(true, 1), pad(true, 2)};
    PadLineup lineup = ds::line_up_pads(pads, PadOrder(), true);
    ds::move_in_lineup(lineup, 2, -1);  // Controller 3 up to Player 1
    const PadOrder order = ds::order_of(pads, lineup, true);
    CHECK(order.chosen);

    // Controller 2 unplugged: the others keep their places and close up.
    const std::vector<KnownPad> fewer{pads[0], pads[2]};
    std::vector<PadSlot> got = slots(fewer, order, true);
    CHECK(got[0] == PadSlot::gamepad());
    CHECK(got[1] == PadSlot::player_number(0));

    // A new controller joins at the end, and Steam's numbering moving around
    // under the old ones changes nothing: they are held by id, not position.
    std::vector<KnownPad> more = pads;
    more[2].position = 0;
    more[0].position = 2;
    more.push_back(pad(true, 3, 999));
    got = slots(more, order, true);
    CHECK(got[0] == PadSlot::gamepad());
    CHECK(got[2] == PadSlot::player_number(0));
    CHECK(got[1] == PadSlot::player_number(1));
    CHECK(got[3] == PadSlot::player_number(2));

    // The GamePad moved to the other device: back to the defaults.
    got = slots(pads, order, false);
    CHECK(got[0] == PadSlot::player_number(0));
}

// Each slot gets exactly its own pads' presses.
void test_each_slot_gets_its_own_pads_presses() {
    std::vector<KnownPad> pads{pad(true, 0), pad(true, 1), pad(false, 0)};
    pads[0].held.set(ds::DsButton::A, true);
    pads[1].held.set(ds::DsButton::B, true);
    pads[1].held.circle_x = -20000;
    pads[2].held.set(ds::DsButton::X, true);
    pads[2].held.motion.present = true;
    pads[2].held.motion.gyro_x = 1.5f;
    const std::vector<PadSlot> assigned{PadSlot::player_number(0), PadSlot::player_number(1),
                                        PadSlot::gamepad()};
    const ds::RoutedPads routed = ds::route_pads(pads, assigned);
    CHECK(routed.player_fed[0] && routed.player_fed[1]);
    CHECK(routed.players[0].pressed(ds::DsButton::A) && !routed.players[0].pressed(ds::DsButton::B));
    CHECK(routed.players[1].pressed(ds::DsButton::B) && routed.players[1].circle_x == -20000);
    CHECK(routed.gamepad_fed && routed.gamepad.pressed(ds::DsButton::X));
    CHECK(routed.gamepad.motion.present && routed.gamepad.motion.gyro_x == 1.5f);
    for (int player = 2; player < ds::kPadPlayers; ++player) CHECK(!routed.player_fed[player]);
    CHECK(ds::pads_in_slot(assigned, PadSlot::player_number(1)).size() == 1);
}

// The Controllers screen, worked with a pad: confirm picks a controller up,
// up and down carry it, confirm puts it down; north asks which one it is.
void test_the_controllers_screen_moves_a_pad_like_steam_does() {
    const std::vector<KnownPad> pads = {pad(true, 0, 10), pad(true, 1, 11), pad(true, 2, 12)};
    PadOrder order;
    ds::ControllersScreen screen;
    screen.open();
    const auto press = [&](ds::UiAction action) {
        return screen.update({action}, {}, 1280.0f, 800.0f, pads, true, order);
    };

    // North on the GamePad's row buzzes Controller 1.
    ds::ControllersScreenResult result = press(ds::UiAction::MenuQuit);
    CHECK_EQ(result.identify, 0);

    // Down to Player 2 (Controller 3), pick it up, carry it to the top.
    press(ds::UiAction::Down);
    press(ds::UiAction::Down);
    CHECK_EQ(screen.focus(), 2);
    press(ds::UiAction::Confirm);
    CHECK(screen.holding());
    result = press(ds::UiAction::Up);
    CHECK(result.order_changed);
    CHECK_EQ(screen.focus(), 1);
    press(ds::UiAction::Up);
    CHECK_EQ(screen.focus(), 0);
    press(ds::UiAction::Confirm);
    CHECK(!screen.holding());

    std::vector<PadSlot> got = slots(pads, order, true);
    CHECK_MSG(got[2] == PadSlot::gamepad(), ds::pad_slot_name(got[2]));
    CHECK(got[0] == PadSlot::player_number(0));
    CHECK(got[1] == PadSlot::player_number(1));

    // Back puts a pad down before it leaves anything.
    press(ds::UiAction::Confirm);
    CHECK(screen.holding());
    result = press(ds::UiAction::Cancel);
    CHECK(!screen.holding() && !result.close_requested && screen.is_open());

    // The row after the line puts everything back to the order.
    press(ds::UiAction::Down);
    press(ds::UiAction::Down);
    press(ds::UiAction::Down);
    CHECK_EQ(screen.focus(), 3);
    result = press(ds::UiAction::Confirm);
    CHECK(result.order_changed);
    CHECK(!order.chosen);

    // And Back leaves.
    result = press(ds::UiAction::Cancel);
    CHECK(result.close_requested);
    CHECK(!screen.is_open());
}

}  // namespace

int main() {
    test_slots_and_pads_are_named_as_people_read_them();
    test_defaults_come_straight_from_the_order();
    test_the_device_holding_the_gamepad_gives_it_its_first_controller();
    test_a_gamepad_with_no_controller_is_left_empty();
    test_a_companions_extra_controllers_are_players_after_this_devices();
    test_past_eight_players_a_controller_is_not_used();
    test_moving_a_pad_trades_places();
    test_an_empty_gamepad_is_taken_rather_than_traded();
    test_a_chosen_order_holds_while_pads_come_and_go();
    test_each_slot_gets_its_own_pads_presses();
    test_the_controllers_screen_moves_a_pad_like_steam_does();
    return test::finish("pad-routing");
}
