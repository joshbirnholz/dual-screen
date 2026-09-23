#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "input/ds_input.hpp"

namespace ds {

// Which controller each physical pad is, on a console whose controllers each
// belong to one player -- a Wii U, where the GamePad is one controller and
// every Pro Controller or Wii Remote is another. (A DS or a 3DS is one console
// two people share, and merges every pad instead; see PadOwner.)
//
// Decided here, on the device running the game, because this is the only
// place every controller on every device can be seen at once. Cemu is handed
// the GamePad and Player 1 to Player 8 and never learns which pad is which --
// which is what lets a mapping made once in Cemu stay put while the pads
// behind each player are changed here.
//
// The pads stand in one line, the way SteamOS's own Controller Order screen
// has them: the first is the GamePad, the next eight are Player 1 to Player 8,
// and any after that are not used. Moving a pad up or down the line is the
// whole of choosing what it is.

// How many players there are besides the GamePad. The same number as the link
// to Cemu carries -- see kExternalPlayers -- restated here so this file needs
// nothing from that one.
inline constexpr int kPadPlayers = 8;

// Where one pad's presses go.
struct PadSlot {
    enum class Kind { NotUsed, GamePad, Player };
    Kind kind = Kind::NotUsed;
    // 0 for Player 1. Only meaningful for Kind::Player.
    int player = 0;

    static PadSlot not_used() { return PadSlot{}; }
    static PadSlot gamepad() { return PadSlot{Kind::GamePad, 0}; }
    static PadSlot player_number(int index) { return PadSlot{Kind::Player, index}; }

    bool operator==(const PadSlot& other) const {
        return kind == other.kind && (kind != Kind::Player || player == other.player);
    }
    bool operator!=(const PadSlot& other) const { return !(*this == other); }
};

// What the Controllers screen calls it: "GamePad", "Player 1", "Not used".
std::string pad_slot_name(PadSlot slot);

// One controller somewhere in the session.
//
// Never known by its name. The name Steam hands SDL for a virtual pad is
// fixed when the pad appears, and after controllers come, go and are
// reordered mid-game it can describe a different controller from the one now
// behind it. A label that might be wrong is worse than a number that is
// always right -- and a controller that buzzes when asked is better than
// either.
struct KnownPad {
    // Which device it is plugged into: this one, or the companion.
    bool on_this_device = true;
    // Its id on that device: SDL's instance id here, the companion's own
    // number for it there. Stays the same for as long as the pad stays
    // connected, which is what lets a pad keep its place in the line while
    // others come and go around it. Also what a rumble is sent to.
    uint32_t id = 0;
    // Where it comes in that device's order, from 0 -- Steam's order, under
    // Steam Input. Where the line starts from, and what the pad is called.
    int position = 0;
    // What SDL calls it -- under Steam Input, the name given to it in Steam.
    // Shown beside the number and nothing more: never used to recognise a
    // pad, because it can be stale (see above). Empty where there is none.
    std::string name;
    // Which player slot its own device has it in, from 0 -- Steam's slot
    // under Steam Input, the console's on a Switch, the number the pad's own
    // lights show where it has them. -1 where there is none, as for a
    // Switch's handheld controls. Shown only, like the name.
    int slot = -1;
    // What it is holding right now: buttons and both sticks, and how it is
    // being moved if it can say.
    DsInputState held;
};

// Which pad this is, for as long as it stays connected: "this#12",
// "companion#3".
std::string pad_identity(const KnownPad& pad);

// What the Controllers screen calls it: "Controller 1".
std::string pad_label(const KnownPad& pad);

// The line itself, as indexes into a list of pads.

// No keyboards anywhere in here. On a Wii U the keys are Cemu's to map, as
// its own Keyboard controller, and none of these carry them.
struct PadLineup {
    // The GamePad's pad, or -1 for none: a companion with no controller of
    // its own holding the GamePad is played by its touch screen and motion
    // alone.
    int gamepad = -1;
    // Player 1 onward; never more than kPadPlayers.
    std::vector<int> players;
    // Everything past Player 8, and anything moved down there.
    std::vector<int> unused;
};

// A line the player has rearranged, held by pad_identity() so it survives
// pads coming and going for the rest of the session.
struct PadOrder {
    // False until something has been moved: the line is then straight from
    // each device's own order, which follows Steam's as it changes.
    bool chosen = false;
    // Which device was holding the GamePad when it was arranged. A line put
    // together with the GamePad on one device says nothing useful once it has
    // moved to the other, so it is dropped when that changes.
    bool gamepad_here = true;
    std::string gamepad;  // empty for none
    std::vector<std::string> players;
    std::vector<std::string> unused;
};

// Where each pad stands.
//
// Straight from the order when nothing has been chosen:
//
//  - The device showing the GamePad's screen is holding the GamePad, so its
//    first controller is the GamePad.
//  - Every other controller is a player, numbered upward in order -- this
//    device's first, then the companion's. Past eight, not used.
//
// With a chosen order, each pad keeps its place; a pad that has gone leaves
// a gap the rest close up; and a pad that has arrived since joins the end of
// the players -- or takes an empty GamePad, when it is on the device holding
// it -- the same as it would have by default.
//
// `gamepad_here` is whether this device is the one showing the GamePad's
// screen: true when nothing is connected, or when the companion took the
// television.
PadLineup line_up_pads(const std::vector<KnownPad>& pads, const PadOrder& order,
                       bool gamepad_here);

// The same line, held by identity, to be kept.
PadOrder order_of(const std::vector<KnownPad>& pads, const PadLineup& lineup, bool gamepad_here);

// Moves pad `pad` (an index into the pads) one place up (-1) or down (+1).
// Up from Player 1 is the GamePad -- taking it outright when it is empty,
// trading places when it is not -- and down from the last player is the top
// of Not used. Returns whether anything moved.
bool move_in_lineup(PadLineup& lineup, int pad, int direction);

// One line of the line as shown: its slot, and its pad, or -1 for an empty
// GamePad.
struct LineupRow {
    PadSlot slot;
    int pad = -1;
};
std::vector<LineupRow> lineup_rows(const PadLineup& lineup);

// Every pad's slot, in the order given. No two pads ever share one: the line
// has one place for each.
std::vector<PadSlot> slots_of(const std::vector<KnownPad>& pads, const PadLineup& lineup);

// The GamePad's and each player's pad, merged from every pad assigned there.
//
// Merged the way two pads on one DS are: any button pressed on either is
// pressed, and whichever stick is pushed further wins, so two pads never
// average each other out into standing still. Motion is taken from the first
// pad that has any -- two orientations cannot be added up into anything.
struct RoutedPads {
    DsInputState gamepad;
    bool gamepad_fed = false;
    DsInputState players[kPadPlayers];
    bool player_fed[kPadPlayers] = {};
};
RoutedPads route_pads(const std::vector<KnownPad>& pads, const std::vector<PadSlot>& slots);

// Which pads are assigned to `slot`, as indexes into the list given.
std::vector<std::size_t> pads_in_slot(const std::vector<PadSlot>& slots, PadSlot slot);

}  // namespace ds
