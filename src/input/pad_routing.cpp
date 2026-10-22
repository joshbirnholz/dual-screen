#include "input/pad_routing.hpp"

#include <algorithm>
#include <cstdlib>
#include <utility>

namespace ds {

std::string pad_slot_name(PadSlot slot) {
    switch (slot.kind) {
        case PadSlot::Kind::GamePad: return "GamePad";
        case PadSlot::Kind::Player: return "Player " + std::to_string(slot.player + 1);
        case PadSlot::Kind::NotUsed: break;
    }
    return "Not used";
}

std::string pad_identity(const KnownPad& pad) {
    return std::string(pad.on_this_device ? "this#" : "companion#") + std::to_string(pad.id);
}

std::string pad_label(const KnownPad& pad) {
    return "Controller " + std::to_string(pad.position + 1);
}

namespace {

// Two pads' worth of presses as one: see RoutedPads.
void merge_into(DsInputState& into, const DsInputState& from) {
    into.buttons |= from.buttons;
    const auto further = [](int16_t mine, int16_t theirs) {
        return std::abs(static_cast<int>(theirs)) > std::abs(static_cast<int>(mine)) ? theirs
                                                                                    : mine;
    };
    into.circle_x = further(into.circle_x, from.circle_x);
    into.circle_y = further(into.circle_y, from.circle_y);
    into.c_stick_x = further(into.c_stick_x, from.c_stick_x);
    into.c_stick_y = further(into.c_stick_y, from.c_stick_y);
    if (from.touching && !into.touching) {
        into.touching = true;
        into.touch_x = from.touch_x;
        into.touch_y = from.touch_y;
    }
    if (from.motion.present && !into.motion.present) {
        into.motion = from.motion;
    }
}

// Every pad in the default order: this device's by position, then the
// companion's.
std::vector<int> default_order(const std::vector<KnownPad>& pads) {
    std::vector<int> order;
    for (std::size_t index = 0; index < pads.size(); ++index) {
        order.push_back(static_cast<int>(index));
    }
    std::stable_sort(order.begin(), order.end(), [&](int left, int right) {
        const KnownPad& a = pads[static_cast<std::size_t>(left)];
        const KnownPad& b = pads[static_cast<std::size_t>(right)];
        if (a.on_this_device != b.on_this_device) return a.on_this_device;
        return a.position < b.position;
    });
    return order;
}

// Past Player 8 is Not used, at the top of it: the pads pushed off the end are
// the ones nearest to it.
void cap_players(PadLineup& lineup) {
    if (static_cast<int>(lineup.players.size()) <= kPadPlayers) return;
    lineup.unused.insert(lineup.unused.begin(), lineup.players.begin() + kPadPlayers,
                         lineup.players.end());
    lineup.players.resize(kPadPlayers);
}

}  // namespace

PadLineup line_up_pads(const std::vector<KnownPad>& pads, const PadOrder& order,
                       bool gamepad_here) {
    PadLineup lineup;
    const std::vector<int> natural = default_order(pads);
    std::vector<bool> placed(pads.size(), false);

    if (order.chosen && order.gamepad_here == gamepad_here) {
        const auto find = [&](const std::string& identity) {
            for (const int index : natural) {
                if (!placed[static_cast<std::size_t>(index)] &&
                    pad_identity(pads[static_cast<std::size_t>(index)]) == identity) {
                    placed[static_cast<std::size_t>(index)] = true;
                    return index;
                }
            }
            return -1;
        };
        if (!order.gamepad.empty()) lineup.gamepad = find(order.gamepad);
        for (const std::string& identity : order.players) {
            const int index = find(identity);
            if (index >= 0) lineup.players.push_back(index);
        }
        for (const std::string& identity : order.unused) {
            const int index = find(identity);
            if (index >= 0) lineup.unused.push_back(index);
        }
    }

    // Whatever is not placed yet -- everything, with nothing chosen -- takes
    // its default place.
    for (const int index : natural) {
        if (placed[static_cast<std::size_t>(index)]) continue;
        placed[static_cast<std::size_t>(index)] = true;
        if (lineup.gamepad < 0 && pads[static_cast<std::size_t>(index)].on_this_device == gamepad_here) {
            lineup.gamepad = index;
        } else {
            lineup.players.push_back(index);
        }
    }
    cap_players(lineup);
    return lineup;
}

PadOrder order_of(const std::vector<KnownPad>& pads, const PadLineup& lineup, bool gamepad_here) {
    PadOrder order;
    order.chosen = true;
    order.gamepad_here = gamepad_here;
    const auto identity = [&](int index) { return pad_identity(pads[static_cast<std::size_t>(index)]); };
    if (lineup.gamepad >= 0) order.gamepad = identity(lineup.gamepad);
    for (const int index : lineup.players) order.players.push_back(identity(index));
    for (const int index : lineup.unused) order.unused.push_back(identity(index));
    return order;
}

bool move_in_lineup(PadLineup& lineup, int pad, int direction) {
    if (direction == 0) return false;
    std::vector<int>& players = lineup.players;
    std::vector<int>& unused = lineup.unused;

    if (lineup.gamepad == pad) {
        if (direction < 0) return false;
        if (!players.empty()) {
            std::swap(lineup.gamepad, players.front());
        } else {
            unused.insert(unused.begin(), pad);
            lineup.gamepad = -1;
        }
        return true;
    }

    const auto in_players = std::find(players.begin(), players.end(), pad);
    if (in_players != players.end()) {
        const std::size_t at = static_cast<std::size_t>(in_players - players.begin());
        if (direction < 0) {
            if (at > 0) {
                std::swap(players[at], players[at - 1]);
            } else if (lineup.gamepad < 0) {
                lineup.gamepad = pad;
                players.erase(players.begin());
            } else {
                std::swap(lineup.gamepad, players.front());
            }
        } else if (at + 1 < players.size()) {
            std::swap(players[at], players[at + 1]);
        } else {
            players.erase(players.begin() + static_cast<std::ptrdiff_t>(at));
            unused.insert(unused.begin(), pad);
        }
        return true;
    }

    const auto in_unused = std::find(unused.begin(), unused.end(), pad);
    if (in_unused == unused.end()) return false;
    const std::size_t at = static_cast<std::size_t>(in_unused - unused.begin());
    if (direction > 0) {
        if (at + 1 >= unused.size()) return false;
        std::swap(unused[at], unused[at + 1]);
        return true;
    }
    if (at > 0) {
        std::swap(unused[at], unused[at - 1]);
        return true;
    }
    // Back into play: the next player along, or, with no players at all and
    // nobody holding the GamePad, straight to it -- the same place the line
    // would have put it.
    unused.erase(unused.begin());
    if (static_cast<int>(players.size()) < kPadPlayers) {
        if (players.empty() && lineup.gamepad < 0) {
            lineup.gamepad = pad;
        } else {
            players.push_back(pad);
        }
    } else {
        unused.insert(unused.begin(), players.back());
        players.back() = pad;
    }
    return true;
}

namespace {

// Where a pad stands in the line: the GamePad's place, a player's or one of
// Not used's -- or nowhere.
int* place_of(PadLineup& lineup, int pad) {
    if (pad < 0) return nullptr;
    if (lineup.gamepad == pad) return &lineup.gamepad;
    for (int& player : lineup.players) {
        if (player == pad) return &player;
    }
    for (int& unused : lineup.unused) {
        if (unused == pad) return &unused;
    }
    return nullptr;
}

}  // namespace

bool put_in_slot(PadLineup& lineup, int pad, PadSlot slot) {
    int* from = place_of(lineup, pad);
    if (from == nullptr) return false;
    int* to = nullptr;
    if (slot.kind == PadSlot::Kind::GamePad) {
        to = &lineup.gamepad;
    } else if (slot.kind == PadSlot::Kind::Player && slot.player >= 0 &&
               slot.player < static_cast<int>(lineup.players.size())) {
        to = &lineup.players[static_cast<std::size_t>(slot.player)];
    } else {
        return false;
    }
    if (from == to) return false;
    if (*to >= 0) {
        std::swap(*from, *to);
        return true;
    }
    // An empty GamePad: the pad leaves its place, and the line closes up.
    *to = pad;
    if (from == &lineup.gamepad) {
        lineup.gamepad = -1;
    } else {
        std::vector<int>& list =
            (from >= lineup.players.data() && from < lineup.players.data() + lineup.players.size())
                ? lineup.players
                : lineup.unused;
        list.erase(list.begin() + (from - list.data()));
    }
    return true;
}

bool take_out_of_play(PadLineup& lineup, int pad) {
    if (pad < 0) return false;
    if (lineup.gamepad == pad) {
        lineup.gamepad = -1;
    } else {
        const auto at = std::find(lineup.players.begin(), lineup.players.end(), pad);
        if (at == lineup.players.end()) return false;
        lineup.players.erase(at);
    }
    lineup.unused.insert(lineup.unused.begin(), pad);
    return true;
}

std::vector<LineupRow> lineup_rows(const PadLineup& lineup) {
    std::vector<LineupRow> rows;
    rows.push_back(LineupRow{PadSlot::gamepad(), lineup.gamepad});
    for (std::size_t index = 0; index < lineup.players.size(); ++index) {
        rows.push_back(LineupRow{PadSlot::player_number(static_cast<int>(index)),
                                 lineup.players[index]});
    }
    for (const int pad : lineup.unused) rows.push_back(LineupRow{PadSlot::not_used(), pad});
    return rows;
}

std::vector<PadSlot> slots_of(const std::vector<KnownPad>& pads, const PadLineup& lineup) {
    std::vector<PadSlot> slots(pads.size());
    for (const LineupRow& row : lineup_rows(lineup)) {
        if (row.pad >= 0 && static_cast<std::size_t>(row.pad) < slots.size()) {
            slots[static_cast<std::size_t>(row.pad)] = row.slot;
        }
    }
    return slots;
}

RoutedPads route_pads(const std::vector<KnownPad>& pads, const std::vector<PadSlot>& slots) {
    RoutedPads routed;
    for (std::size_t index = 0; index < pads.size() && index < slots.size(); ++index) {
        const PadSlot slot = slots[index];
        switch (slot.kind) {
            case PadSlot::Kind::GamePad:
                merge_into(routed.gamepad, pads[index].held);
                routed.gamepad_fed = true;
                break;
            case PadSlot::Kind::Player:
                if (slot.player >= 0 && slot.player < kPadPlayers) {
                    merge_into(routed.players[slot.player], pads[index].held);
                    routed.player_fed[slot.player] = true;
                }
                break;
            case PadSlot::Kind::NotUsed: break;
        }
    }
    return routed;
}

std::vector<std::size_t> pads_in_slot(const std::vector<PadSlot>& slots, PadSlot slot) {
    std::vector<std::size_t> found;
    for (std::size_t index = 0; index < slots.size(); ++index) {
        if (slots[index] == slot) found.push_back(index);
    }
    return found;
}

}  // namespace ds
