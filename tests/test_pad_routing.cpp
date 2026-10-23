// Which pad is the GamePad and which is which player, on a Wii U. See
// input/pad_routing.hpp.

#include "test_support.hpp"

#include <string>
#include <vector>

#include "core/external_frame_link.hpp"
#include "input/pad_routing.hpp"
#include "input/wii_u_mapping.hpp"
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

// A Wii U's Controls changes the GamePad's and the players' order: Player 1
// trades places with the GamePad, the last player goes no lower, and Not
// used's controllers do not move at all.
void test_a_wii_u_moves_only_among_its_players() {
    const std::vector<KnownPad> pads{pad(true, 0), pad(true, 1), pad(true, 2), pad(true, 3)};
    PadLineup lineup = ds::line_up_pads(pads, PadOrder(), true);
    ds::take_out_of_play(lineup, 3);
    CHECK_EQ(lineup.gamepad, 0);
    CHECK_EQ(lineup.players.size(), static_cast<std::size_t>(2));

    CHECK(!ds::move_among_players(lineup, 2, 1));
    CHECK(!ds::move_among_players(lineup, 0, -1));
    CHECK(!ds::move_among_players(lineup, 3, -1));
    CHECK(ds::move_among_players(lineup, 1, 1));
    CHECK_EQ(lineup.players[0], 2);
    CHECK_EQ(lineup.players[1], 1);
    CHECK_EQ(lineup.gamepad, 0);
    CHECK_EQ(lineup.unused.size(), static_cast<std::size_t>(1));

    // Up from Player 1 is the GamePad, and down from the GamePad Player 1.
    CHECK(ds::move_among_players(lineup, 2, -1));
    CHECK_EQ(lineup.gamepad, 2);
    CHECK_EQ(lineup.players[0], 0);
    CHECK(ds::move_among_players(lineup, 2, 1));
    CHECK_EQ(lineup.gamepad, 0);
    CHECK_EQ(lineup.players[0], 2);

    // An empty GamePad is taken outright, Player 1's place left empty.
    lineup.gamepad = -1;
    CHECK(ds::move_among_players(lineup, 2, -1));
    CHECK_EQ(lineup.gamepad, 2);
    CHECK_EQ(lineup.players.size(), static_cast<std::size_t>(2));
    CHECK_EQ(lineup.players[0], -1);
    CHECK_EQ(lineup.players[1], 1);
}

// A rearranged line holds by identity: pads keep their places while others
// leave and arrive -- one leaving leaves its place empty, nobody moving up --
// a newcomer takes the first empty place, and the GamePad gone over to the
// other device leaves its pad to join the players.
void test_a_chosen_order_holds_while_pads_come_and_go() {
    const std::vector<KnownPad> pads{pad(true, 0), pad(true, 1), pad(true, 2)};
    PadLineup lineup = ds::line_up_pads(pads, PadOrder(), true);
    ds::move_in_lineup(lineup, 2, -1);  // Controller 3 up to Player 1
    const PadOrder order = ds::order_of(pads, lineup, true);
    CHECK(order.chosen);

    // Controller 2 unplugged: the others keep their places.
    const std::vector<KnownPad> fewer{pads[0], pads[2]};
    std::vector<PadSlot> got = slots(fewer, order, true);
    CHECK(got[0] == PadSlot::gamepad());
    CHECK(got[1] == PadSlot::player_number(0));
    // Player 1 unplugged instead: Player 2 stays Player 2, and a controller
    // arriving is Player 1.
    const std::vector<KnownPad> gap{pads[0], pads[1]};
    PadLineup held = ds::line_up_pads(gap, order, true);
    CHECK(held.players == std::vector<int>({-1, 1}));
    got = ds::slots_of(gap, held);
    CHECK(got[1] == PadSlot::player_number(1));
    const PadOrder kept = ds::order_of(gap, held, true);
    const std::vector<KnownPad> back{pads[0], pads[1], pad(true, 5, 77)};
    got = slots(back, kept, true);
    CHECK(got[1] == PadSlot::player_number(1));
    CHECK(got[2] == PadSlot::player_number(0));
    // The GamePad's controller gone: nobody moves up into it.
    const std::vector<KnownPad> no_gamepad{pads[1], pads[2]};
    got = slots(no_gamepad, order, true);
    CHECK(got[0] == PadSlot::player_number(1));
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

    // A controller on the other device made the GamePad stays it while the
    // GamePad's screen stays where it is.
    const std::vector<KnownPad> both{pad(true, 0), pad(false, 1)};
    PadLineup mixed = ds::line_up_pads(both, PadOrder(), true);
    CHECK(ds::move_among_players(mixed, 1, -1));
    CHECK_EQ(mixed.gamepad, 1);
    const PadOrder mixed_order = ds::order_of(both, mixed, true);
    got = slots(both, mixed_order, true);
    CHECK(got[1] == PadSlot::gamepad());
    CHECK(got[0] == PadSlot::player_number(0));

    // The GamePad moved to the other device: its pad is a player now, in
    // the first empty place; the players stay.
    got = slots(pads, order, false);
    CHECK(got[2] == PadSlot::player_number(0));
    CHECK(got[1] == PadSlot::player_number(1));
    CHECK(got[0] == PadSlot::player_number(2));
}

// A controller arriving becomes a player only where it may -- on a Wii U,
// a player the game has a type for -- and otherwise waits, not used.
void test_a_controller_joins_only_a_player_it_may_be() {
    const std::vector<KnownPad> pads{pad(true, 0), pad(true, 1), pad(true, 2)};
    PadOrder order;
    order.joinable = {true, false};
    PadLineup lineup = ds::line_up_pads(pads, order, true);
    CHECK_EQ(lineup.gamepad, 0);
    CHECK(lineup.players == std::vector<int>({1}));
    CHECK(lineup.unused == std::vector<int>({2}));
    // Anyone, with nothing said.
    order.joinable.clear();
    lineup = ds::line_up_pads(pads, order, true);
    CHECK(lineup.players == std::vector<int>({1, 2}));
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
// the one there; a GamePad with none takes it, leaving its place empty; and
// one taken out of play goes to the top of Not used, its place left empty.
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
    CHECK(empty.players == std::vector<int>({0, -1, 2}));
    CHECK_EQ(ds::first_free_player(empty), 1);

    CHECK(ds::take_out_of_play(lineup, 2));
    CHECK(lineup.players == std::vector<int>({-1, 0}));
    CHECK(lineup.unused == std::vector<int>({2}));
    // And one not used back in, to the empty Player 1.
    CHECK(ds::put_in_slot(lineup, 2, PadSlot::player_number(0)));
    CHECK(lineup.players == std::vector<int>({2, 0}));
    CHECK(lineup.unused.empty());
    // The last player out: no empty place after the rest.
    CHECK(ds::take_out_of_play(lineup, 0));
    CHECK(lineup.players == std::vector<int>({2}));
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
    gamepad.listed = 2;
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
    spare.listed = 1;
    return {gamepad, first, spare};
}

// A player's page: Controller Type, Cemu's but for None; Controller, every
// controller under its device, in the order they are listed rather than the
// order they play in; and Remove Player, taking the player out of play. Y
// is Detect Controller. The GamePad's has no type to choose.
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
    const std::vector<WiiUPlayerScreen::Row> rows{WiiUPlayerScreen::Row::Type, WiiUPlayerScreen::Row::Controller,
                                                   WiiUPlayerScreen::Row::Mapping, WiiUPlayerScreen::Row::Motion,
                                                   WiiUPlayerScreen::Row::Rumble, WiiUPlayerScreen::Row::Remove};
    CHECK(page.rows() == rows);
    CHECK(page.own_row() != nullptr && page.own_row()->pad == "companion#2");

    // Controller Type: four of Cemu's six -- not None, nor a Classic
    // Controller alone -- on the one it is.
    page.update({UiAction::Confirm}, {}, kWidth, kHeight);
    CHECK(page.choosing());
    CHECK_EQ(page.choice_focus(), 0);
    page.update({UiAction::Down}, {}, kWidth, kHeight);
    page.update({UiAction::Down}, {}, kWidth, kHeight);
    page.update({UiAction::Down}, {}, kWidth, kHeight);
    ds::WiiUPlayerScreenResult result = page.update({UiAction::Confirm}, {}, kWidth, kHeight);
    CHECK_EQ(result.type_chosen, 4);
    CHECK(!page.choosing());

    // Controller: on the one it is, past its device's name -- which is
    // listed first, though the GamePad's plays first.
    page.update({UiAction::Down}, {}, kWidth, kHeight);
    page.update({UiAction::Confirm}, {}, kWidth, kHeight);
    CHECK(page.choosing());
    CHECK_EQ(page.choice_focus(), 1);
    // The spare, under it.
    result = page.update({UiAction::Down}, {}, kWidth, kHeight);
    result = page.update({UiAction::Confirm}, {}, kWidth, kHeight);
    CHECK(result.controller_chosen && *result.controller_chosen == "companion#3");

    // Detect Controller, on Y, which runs out.
    result = page.update({UiAction::MenuReset}, {}, kWidth, kHeight);
    CHECK(result.detect_started && page.detecting() && !page.choosing());
    CHECK_EQ(page.detect_seconds(), 10);
    // Nothing but B, while it waits.
    result = page.update({UiAction::Confirm}, {}, kWidth, kHeight);
    CHECK(page.detecting() && !page.choosing());
    for (int step = 0; step < 50; ++step) page.tick(0.25f);
    CHECK(!page.detecting());
    page.update({UiAction::MenuReset}, {}, kWidth, kHeight);
    page.update({UiAction::Cancel}, {}, kWidth, kHeight);
    CHECK(!page.detecting() && page.is_open());

    // Edit Button Mapping: its page, asked for.
    page.update({UiAction::Down}, {}, kWidth, kHeight);
    result = page.update({UiAction::Confirm}, {}, kWidth, kHeight);
    CHECK(result.edit_mapping && page.is_open() && !page.choosing());

    // Motion and Rumble: switched by A, on as they start.
    CHECK(page.motion() && page.rumble());
    page.update({UiAction::Down}, {}, kWidth, kHeight);
    result = page.update({UiAction::Confirm}, {}, kWidth, kHeight);
    CHECK(result.motion && !*result.motion && !result.rumble && !page.motion());
    page.update({UiAction::Down}, {}, kWidth, kHeight);
    result = page.update({UiAction::Confirm}, {}, kWidth, kHeight);
    CHECK(result.rumble && !*result.rumble && !result.motion && !page.rumble());
    // The caller says what the game has, and that is what is shown.
    page.set_switches(true, false);
    CHECK(page.motion() && !page.rumble());
    CHECK(ds::wii_u_player_view_of(page).motion && !ds::wii_u_player_view_of(page).rumble);

    // Remove Player: out of play, and the page with it.
    page.update({UiAction::Down}, {}, kWidth, kHeight);
    result = page.update({UiAction::Confirm}, {}, kWidth, kHeight);
    CHECK(result.removed && result.close_requested && !page.is_open());

    WiiUPlayerScreen gamepad;
    gamepad.open(PadSlot::gamepad());
    gamepad.set_controllers(controller_rows(), types, 0);
    CHECK(gamepad.title() == "GamePad");
    CHECK_EQ(gamepad.rows().size(), static_cast<std::size_t>(5));
    for (int step = 0; step < 4; ++step) gamepad.update({UiAction::Down}, {}, kWidth, kHeight);
    result = gamepad.update({UiAction::Confirm}, {}, kWidth, kHeight);
    CHECK(result.removed && !result.close_requested && gamepad.is_open());
    // With none there, nothing to remove.
    std::vector<ds::ControllerRowView> empty = controller_rows();
    empty[0].pad.clear();
    gamepad.set_controllers(empty, types, 0);
    CHECK_EQ(gamepad.rows().size(), static_cast<std::size_t>(4));
}

// Add Controller's page: the new player's type, what the game had it as
// last but never None, and its controller, the first not used -- and Add,
// which says both. Detect Controller only chooses one here.
void test_a_new_players_page_says_what_it_will_be_first() {
    using ds::UiAction;
    using ds::WiiUPlayerScreen;
    constexpr float kWidth = 1280.0f;
    constexpr float kHeight = 800.0f;
    const std::vector<std::string> types{"Pro Controller", "Classic Controller", "Wii Remote",
                                         "Wii Remote + Nunchuk", "Wii Remote + Classic Controller", "None"};
    WiiUPlayerScreen page;
    page.open_adding(PadSlot::player_number(1));
    page.set_controllers(controller_rows(), types, 5);
    CHECK(page.adding());
    CHECK(page.title() == "Player 2");
    const std::vector<WiiUPlayerScreen::Row> rows{WiiUPlayerScreen::Row::Type, WiiUPlayerScreen::Row::Controller,
                                                   WiiUPlayerScreen::Row::Add};
    CHECK(page.rows() == rows);
    CHECK_EQ(page.pending_type(), 0);
    CHECK(page.pending_pad() == "companion#3");

    // Chosen, but nothing done until Add.
    page.update({UiAction::Confirm}, {}, kWidth, kHeight);
    page.update({UiAction::Down}, {}, kWidth, kHeight);
    ds::WiiUPlayerScreenResult result = page.update({UiAction::Confirm}, {}, kWidth, kHeight);
    CHECK(result.type_chosen == -1 && page.pending_type() == 2);
    page.update({UiAction::Down}, {}, kWidth, kHeight);
    page.update({UiAction::Confirm}, {}, kWidth, kHeight);
    for (int step = 0; step < 8; ++step) page.update({UiAction::Down}, {}, kWidth, kHeight);
    result = page.update({UiAction::Confirm}, {}, kWidth, kHeight);
    CHECK(!result.controller_chosen && page.pending_pad() == "this#1");
    page.update({UiAction::MenuReset}, {}, kWidth, kHeight);
    page.detected("companion#2");
    CHECK(!page.detecting() && page.pending_pad() == "companion#2");
    page.set_controllers(controller_rows(), types, 5);
    CHECK(page.pending_pad() == "companion#2");

    page.update({UiAction::Down}, {}, kWidth, kHeight);
    result = page.update({UiAction::Confirm}, {}, kWidth, kHeight);
    CHECK(result.added && result.close_requested && result.type_chosen == 2);
    CHECK(result.controller_chosen && *result.controller_chosen == "companion#2");

    // Nothing to add with no controller.
    std::vector<ds::ControllerRowView> none = controller_rows();
    none.pop_back();
    page.open_adding(PadSlot::player_number(1));
    page.set_controllers(none, types, 2);
    CHECK(page.pending_pad().empty() && page.pending_type() == 2);
    page.update({UiAction::Down}, {}, kWidth, kHeight);
    page.update({UiAction::Down}, {}, kWidth, kHeight);
    result = page.update({UiAction::Confirm}, {}, kWidth, kHeight);
    CHECK(!result.added && page.is_open());

    // A Classic Controller alone is one plugged into a Wii Remote; None is
    // the first there is.
    CHECK(!ds::offered_type("Classic Controller") && !ds::offered_type("None") && ds::offered_type("Wii Remote"));
    CHECK_EQ(ds::offered_instead("Classic Controller", types), 4);
    CHECK_EQ(ds::offered_instead("None", types), 0);
    CHECK_EQ(ds::offered_instead("Wii Remote", types), -1);
    page.open_adding(PadSlot::player_number(1));
    page.set_controllers(controller_rows(), types, 1);
    CHECK_EQ(page.pending_type(), 0);

    // A pad made a player after the last, from wherever it was.
    const std::vector<KnownPad> pads{pad(true, 0), pad(true, 1), pad(true, 2)};
    PadLineup lineup = ds::line_up_pads(pads, PadOrder(), true);
    ds::take_out_of_play(lineup, 2);
    CHECK(ds::add_player(lineup, 2));
    CHECK_EQ(lineup.players.size(), static_cast<std::size_t>(2));
    CHECK_EQ(lineup.players[1], 2);
    CHECK(lineup.unused.empty());
    CHECK(ds::add_player(lineup, 0));
    CHECK_EQ(lineup.gamepad, -1);
    CHECK_EQ(lineup.players[2], 0);
}

uint32_t bit(ds::PadControl control) { return 1u << static_cast<unsigned>(control); }
uint32_t bit(ds::ExternalButton button) { return 1u << static_cast<unsigned>(button); }

// Each controller a Wii U game has is mapped as itself: the GamePad's and a Pro
// Controller's buttons straight across, a Wii Remote's held upright with 1 on X
// and 2 on Y, a Nunchuk's C and Z on L and ZL, and a Classic Controller's
// apart from the remote it is plugged into.
void test_a_wii_u_controller_is_mapped_as_itself() {
    using ds::PadControl;
    using ds::ExternalButton;
    using ds::WiiUAction;
    using ds::WiiUController;
    const ds::WiiUMapping standard;
    const uint32_t pressed = bit(PadControl::A) | bit(PadControl::X) | bit(PadControl::L) | bit(PadControl::Zl);
    CHECK_EQ(ds::wii_u_buttons(pressed, standard, WiiUController::Pro, 0),
             bit(ExternalButton::A) | bit(ExternalButton::X) | bit(ExternalButton::L) | bit(ExternalButton::Zl));
    CHECK_EQ(ds::wii_u_buttons(pressed, standard, WiiUController::Remote, 0),
             bit(ExternalButton::RemoteA) | bit(ExternalButton::One));
    CHECK_EQ(ds::wii_u_buttons(pressed, standard, WiiUController::RemoteNunchuk, 0),
             bit(ExternalButton::RemoteA) | bit(ExternalButton::One) | bit(ExternalButton::NunchukC) |
                 bit(ExternalButton::NunchukZ));
    CHECK_EQ(ds::wii_u_buttons(pressed, standard, WiiUController::RemoteClassic, 0),
             bit(ExternalButton::A) | bit(ExternalButton::X) | bit(ExternalButton::L) | bit(ExternalButton::Zl));

    // Changed: B is the remote's HOME Button beside a Classic Controller, Y is
    // turbo on its A, and R presses nothing.
    ds::WiiUMapping changed;
    changed[PadControl::B] = WiiUAction::RemoteHome;
    changed[PadControl::Y] = WiiUAction::Turbo;
    changed.turbo(PadControl::Y) = WiiUAction::A;
    changed[PadControl::R] = WiiUAction::Nothing;
    CHECK(!changed.is_standard(WiiUController::RemoteClassic));
    const uint32_t held = bit(PadControl::B) | bit(PadControl::Y) | bit(PadControl::R);
    uint32_t on = 0;
    uint32_t off = 0;
    for (uint32_t ms = 0; ms < 200; ms += 5) {
        const uint32_t buttons = ds::wii_u_buttons(held, changed, WiiUController::RemoteClassic, ms);
        CHECK(buttons & bit(ExternalButton::RemoteHome));
        CHECK(!(buttons & bit(ExternalButton::R)));
        (buttons & bit(ExternalButton::A) ? on : off) += 1;
    }
    CHECK_MSG(on > 0 && off > 0, "turbo's button never went up and down");

    // A Wii Remote held sideways: its D-Pad turned a quarter, Up pressing the
    // remote's Right, and the rest as they were -- a Nunchuk's never turned.
    ds::WiiUMapping sideways;
    sideways.sideways = true;
    CHECK(sideways.is_standard(WiiUController::Remote));
    CHECK_EQ(ds::wii_u_buttons(bit(PadControl::Up) | bit(PadControl::A), sideways, WiiUController::Remote, 0),
             bit(ExternalButton::RemoteRight) | bit(ExternalButton::RemoteA));
    CHECK_EQ(ds::wii_u_buttons(bit(PadControl::Right), sideways, WiiUController::Remote, 0),
             bit(ExternalButton::RemoteDown));
    CHECK_EQ(ds::wii_u_buttons(bit(PadControl::Down), sideways, WiiUController::Remote, 0),
             bit(ExternalButton::RemoteLeft));
    CHECK_EQ(ds::wii_u_buttons(bit(PadControl::Left), sideways, WiiUController::Remote, 0),
             bit(ExternalButton::RemoteUp));
    CHECK_EQ(ds::wii_u_buttons(bit(PadControl::Up), sideways, WiiUController::RemoteNunchuk, 0),
             bit(ExternalButton::RemoteUp));
    // What a button is chosen as turns too: A on Up presses the remote's Right.
    sideways[PadControl::A] = WiiUAction::RemoteUp;
    CHECK_EQ(ds::wii_u_buttons(bit(PadControl::A), sideways, WiiUController::Remote, 0),
             bit(ExternalButton::RemoteRight));

    // The picker names the remote's own as the remote's only beside a
    // Classic Controller's.
    CHECK(ds::wii_u_action_name(WiiUAction::RemoteA, WiiUController::Remote) == "A Button");
    CHECK(ds::wii_u_action_name(WiiUAction::RemoteA, WiiUController::RemoteClassic) == "Wii Remote A Button");
    for (std::size_t index = 0; index < static_cast<std::size_t>(WiiUAction::Count); ++index) {
        const WiiUAction action = static_cast<WiiUAction>(index);
        CHECK(ds::wii_u_action_from_key(ds::wii_u_action_key(action)) == action);
    }
    CHECK(ds::wii_u_mapping_key(-1, WiiUController::GamePad) == "gamepad");
    CHECK(ds::wii_u_mapping_key(1, WiiUController::RemoteNunchuk) == "player2.nunchuk");
    CHECK(ds::wii_u_controller_for("Wii Remote + Classic Controller") == WiiUController::RemoteClassic);
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
    test_a_wii_u_moves_only_among_its_players();
    test_a_chosen_order_holds_while_pads_come_and_go();
    test_a_controller_joins_only_a_player_it_may_be();
    test_each_slot_gets_its_own_pads_presses();
    test_the_controllers_screen_moves_a_pad_like_steam_does();
    test_a_controller_chosen_for_a_place_trades_with_the_one_there();
    test_a_players_page_chooses_a_type_and_a_controller();
    test_a_new_players_page_says_what_it_will_be_first();
    test_a_wii_u_controller_is_mapped_as_itself();
    return test::finish("pad-routing");
}
