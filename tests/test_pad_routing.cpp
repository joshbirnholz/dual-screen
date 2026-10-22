// Which pad is the GamePad and which is which player, on a Wii U. See
// input/pad_routing.hpp.

#include "test_support.hpp"

#include <string>
#include <vector>

#include "input/pad_routing.hpp"
#include "ui/controllers_screen.hpp"
#include "ui/wii_u_player_screen.hpp"

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

    // Either upper face button on the GamePad's row buzzes Controller 1:
    // which one a pad calls X depends on the pad.
    ds::ControllersScreenResult result = press(ds::UiAction::MenuQuit);
    CHECK_EQ(result.identify, 0);
    result = press(ds::UiAction::MenuReset);
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

    // The row after the line is Done, and nothing is to be found on it.
    press(ds::UiAction::Down);
    press(ds::UiAction::Down);
    press(ds::UiAction::Down);
    CHECK_EQ(screen.focus(), 3);
    CHECK_EQ(press(ds::UiAction::MenuQuit).identify, -1);
    result = press(ds::UiAction::Confirm);
    CHECK(result.close_requested);
    CHECK(order.chosen);
    CHECK(!screen.is_open());

    // And Back leaves too.
    screen.open();
    result = press(ds::UiAction::Cancel);
    CHECK(result.close_requested);
    CHECK(!screen.is_open());
}

}  // namespace

// A player's page choosing a controller puts it in that place, trading with
// the one there; a GamePad with none takes it; and one taken out of play goes
// to the top of Not used, the players after it moving up.
void test_a_controller_chosen_for_a_place_trades_with_the_one_there() {
    const std::vector<KnownPad> pads{pad(true, 0), pad(true, 1), pad(true, 2)};
    PadLineup lineup = ds::line_up_pads(pads, PadOrder(), true);
    CHECK(ds::put_in_slot(lineup, 2, PadSlot::player_number(0)));
    CHECK(lineup.players == std::vector<int>({2, 1}));
    CHECK(ds::put_in_slot(lineup, 1, PadSlot::gamepad()));
    CHECK_EQ(lineup.gamepad, 1);
    CHECK(lineup.players == std::vector<int>({2, 0}));
    CHECK(!ds::put_in_slot(lineup, 1, PadSlot::gamepad()));
    // No Player 3 to be.
    CHECK(!ds::put_in_slot(lineup, 1, PadSlot::player_number(2)));

    PadLineup empty = ds::line_up_pads(pads, PadOrder(), false);
    CHECK_EQ(empty.gamepad, -1);
    CHECK(ds::put_in_slot(empty, 1, PadSlot::gamepad()));
    CHECK_EQ(empty.gamepad, 1);
    CHECK(empty.players == std::vector<int>({0, 2}));

    CHECK(ds::take_out_of_play(lineup, 2));
    CHECK(lineup.players == std::vector<int>({0}));
    CHECK(lineup.unused == std::vector<int>({2}));
    // And one not used back in, trading with Player 1.
    CHECK(ds::put_in_slot(lineup, 2, PadSlot::player_number(0)));
    CHECK(lineup.players == std::vector<int>({2}));
    CHECK(lineup.unused == std::vector<int>({0}));
    CHECK(!ds::take_out_of_play(lineup, 0));
}

std::vector<ds::ControllerRowView> controller_rows() {
    ds::ControllerRowView gamepad;
    gamepad.slot = PadSlot::gamepad();
    gamepad.pad = "this#1";
    gamepad.name = "Steam Deck Controls";
    gamepad.type = "Wii U GamePad";
    gamepad.device = "Steam Deck";
    gamepad.place = "Steam Deck  P1";
    ds::ControllerRowView first;
    first.slot = PadSlot::player_number(0);
    first.pad = "companion#2";
    first.name = "Xbox Wireless Controller";
    first.type = "Wii U Pro Controller";
    first.device = "Living Room PC";
    first.place = "Living Room PC  P1";
    ds::ControllerRowView spare;
    spare.pad = "companion#3";
    spare.name = "8BitDo Pro 2";
    spare.device = "Living Room PC";
    spare.place = "Living Room PC  P2";
    return {gamepad, first, spare};
}

// A player's page: Controller Type, Cemu's but for None, and Controller --
// Detect Controller, None and every controller under its device -- and X
// taking the player out of play. The GamePad's has no type to choose.
void test_a_players_page_chooses_a_type_and_a_controller() {
    using ds::UiAction;
    using ds::WiiUPlayerScreen;
    constexpr float kWidth = 1280.0f;
    constexpr float kHeight = 800.0f;
    const std::vector<std::string> types{"Pro Controller", "Classic Controller", "Wii Remote",
                                         "Wii Remote + Nunchuk", "Wii Remote + Classic Controller", "None"};
    WiiUPlayerScreen page;
    page.open(PadSlot::player_number(0));
    page.set_controllers(controller_rows(), types, 0);
    CHECK(page.title() == "Player 1");
    CHECK_EQ(page.rows().size(), static_cast<std::size_t>(2));
    CHECK(page.own_row() != nullptr && page.own_row()->pad == "companion#2");

    // Controller Type: five of Cemu's six, on the one it is.
    page.update({UiAction::Confirm}, {}, kWidth, kHeight);
    CHECK(page.choosing());
    CHECK_EQ(page.choice_focus(), 0);
    page.update({UiAction::Down}, {}, kWidth, kHeight);
    page.update({UiAction::Down}, {}, kWidth, kHeight);
    page.update({UiAction::Down}, {}, kWidth, kHeight);
    ds::WiiUPlayerScreenResult result = page.update({UiAction::Confirm}, {}, kWidth, kHeight);
    CHECK_EQ(result.type_chosen, 3);
    CHECK(!page.choosing());
    for (int step = 0; step < 8; ++step) page.update({UiAction::Down}, {}, kWidth, kHeight);

    // Controller: on the one it is, past Detect, None and its device's name.
    page.update({UiAction::Confirm}, {}, kWidth, kHeight);
    CHECK(page.choosing());
    CHECK_EQ(page.choice_focus(), 5);
    // The spare, under it.
    result = page.update({UiAction::Down}, {}, kWidth, kHeight);
    result = page.update({UiAction::Confirm}, {}, kWidth, kHeight);
    CHECK(result.controller_chosen && *result.controller_chosen == "companion#3");

    // Detect Controller, which runs out.
    page.update({UiAction::Confirm}, {}, kWidth, kHeight);
    for (int step = 0; step < 8; ++step) page.update({UiAction::Up}, {}, kWidth, kHeight);
    result = page.update({UiAction::Confirm}, {}, kWidth, kHeight);
    CHECK(result.detect_started && page.detecting());
    CHECK_EQ(page.detect_seconds(), 10);
    // Nothing but B, while it waits.
    result = page.update({UiAction::Confirm}, {}, kWidth, kHeight);
    CHECK(page.detecting() && !page.choosing());
    for (int step = 0; step < 50; ++step) page.tick(0.25f);
    CHECK(!page.detecting());
    page.update({UiAction::Confirm}, {}, kWidth, kHeight);
    for (int step = 0; step < 8; ++step) page.update({UiAction::Up}, {}, kWidth, kHeight);
    page.update({UiAction::Confirm}, {}, kWidth, kHeight);
    page.update({UiAction::Cancel}, {}, kWidth, kHeight);
    CHECK(!page.detecting() && page.is_open());

    // X: out of play, and the page with it.
    result = page.update({UiAction::MenuQuit}, {}, kWidth, kHeight);
    CHECK(result.removed && result.close_requested && !page.is_open());

    WiiUPlayerScreen gamepad;
    gamepad.open(PadSlot::gamepad());
    gamepad.set_controllers(controller_rows(), types, 0);
    CHECK(gamepad.title() == "GamePad");
    CHECK_EQ(gamepad.rows().size(), static_cast<std::size_t>(1));
    result = gamepad.update({UiAction::MenuQuit}, {}, kWidth, kHeight);
    CHECK(result.removed && !result.close_requested && gamepad.is_open());
}

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
    test_a_controller_chosen_for_a_place_trades_with_the_one_there();
    test_a_players_page_chooses_a_type_and_a_controller();
    return test::finish("pad-routing");
}
