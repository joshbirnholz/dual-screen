#pragma once

#include <cstdint>
#include <map>
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

// A slot as a word: "gamepad", "player1" to "player8", "unused".
std::string pad_slot_key(PadSlot slot);
// False, leaving `out` alone, for anything else.
bool pad_slot_from_key(const std::string& key, PadSlot& out);
// What the Controllers screen calls it: "GamePad", "Player 1", "Not used".
std::string pad_slot_name(PadSlot slot);
// The next one along, and the one before -- GamePad, Player 1 to 8, Not used,
// and round again.
PadSlot next_pad_slot(PadSlot slot);
PadSlot previous_pad_slot(PadSlot slot);

// One controller somewhere in the session.
//
// Known by where it comes in its device's order, not by its name. Under Steam
// Input that order is Steam's own, which a player can rearrange in Steam and
// which is the thing they actually reason about -- while the name Steam hands
// SDL for a virtual pad is fixed when the pad appears, and after controllers
// come, go and are reordered mid-game it can describe a different controller
// from the one now behind it. A label that might be wrong is worse than a
// number that is always right.
struct KnownPad {
    // Which device it is plugged into: this one, or the companion.
    bool on_this_device = true;
    // Its id on that device: SDL's instance id here, the companion's own
    // number for it there. What a rumble is sent to.
    uint32_t id = 0;
    // Where it comes in that device's order, from 0.
    int position = 0;
    // The keyboard on that device, rather than a pad. Listed so its keys can
    // go somewhere, but never counted among the controllers.
    bool keyboard = false;
    // What it is holding right now: buttons and both sticks, and how it is
    // being moved if it can say.
    DsInputState held;
};

// What a pad's choice is held under for the rest of the session: which device
// and where it comes -- "this:1", "companion:2", "this:keyboard".
std::string pad_key(const KnownPad& pad);

// What the Controllers screen calls it: "Controller 1", "Keyboard".
std::string pad_label(const KnownPad& pad);

// Where each pad goes, in the order given: what the player chose for it this
// session if they have (`chosen`, by pad_key()), and otherwise the default,
// straight from the order --
//
//  - The device showing the GamePad's screen is holding the GamePad, so its
//    first controller is the GamePad, and so are its keys.
//  - A keyboard on the other device is nobody's.
//  - Every other controller is a player, numbered upward in order -- this
//    device's first, then the companion's -- skipping any number a player has
//    already chosen for another pad. Past eight, nobody's.
//
// `gamepad_here` is whether this device is the one showing the GamePad's
// screen: true when nothing is connected, or when the companion took the
// television.
std::vector<PadSlot> assign_pads(const std::vector<KnownPad>& pads,
                                 const std::map<std::string, std::string>& chosen,
                                 bool gamepad_here);

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
