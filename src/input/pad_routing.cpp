#include "input/pad_routing.hpp"

#include <algorithm>
#include <cstdlib>

namespace ds {

std::string pad_slot_key(PadSlot slot) {
    switch (slot.kind) {
        case PadSlot::Kind::GamePad: return "gamepad";
        case PadSlot::Kind::Player: return "player" + std::to_string(slot.player + 1);
        case PadSlot::Kind::NotUsed: break;
    }
    return "unused";
}

bool pad_slot_from_key(const std::string& key, PadSlot& out) {
    if (key == "gamepad") {
        out = PadSlot::gamepad();
        return true;
    }
    if (key == "unused") {
        out = PadSlot::not_used();
        return true;
    }
    const std::string prefix = "player";
    if (key.size() == prefix.size() + 1 && key.compare(0, prefix.size(), prefix) == 0) {
        const int number = key.back() - '0';
        if (number >= 1 && number <= kPadPlayers) {
            out = PadSlot::player_number(number - 1);
            return true;
        }
    }
    return false;
}

std::string pad_slot_name(PadSlot slot) {
    switch (slot.kind) {
        case PadSlot::Kind::GamePad: return "GamePad";
        case PadSlot::Kind::Player: return "Player " + std::to_string(slot.player + 1);
        case PadSlot::Kind::NotUsed: break;
    }
    return "Not used";
}

namespace {

// Every slot in the order the Controllers screen steps through them.
int slot_position(PadSlot slot) {
    switch (slot.kind) {
        case PadSlot::Kind::GamePad: return 0;
        case PadSlot::Kind::Player: return 1 + std::clamp(slot.player, 0, kPadPlayers - 1);
        case PadSlot::Kind::NotUsed: break;
    }
    return 1 + kPadPlayers;
}

PadSlot slot_at(int position) {
    constexpr int kCount = 2 + kPadPlayers;
    position = ((position % kCount) + kCount) % kCount;
    if (position == 0) return PadSlot::gamepad();
    if (position <= kPadPlayers) return PadSlot::player_number(position - 1);
    return PadSlot::not_used();
}

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

}  // namespace

PadSlot next_pad_slot(PadSlot slot) { return slot_at(slot_position(slot) + 1); }
PadSlot previous_pad_slot(PadSlot slot) { return slot_at(slot_position(slot) - 1); }

std::string pad_key(const KnownPad& pad) {
    std::string key = pad.on_this_device ? "this:" : "companion:";
    key += pad.keyboard ? std::string("keyboard") : std::to_string(pad.position + 1);
    return key;
}

std::string pad_label(const KnownPad& pad) {
    return pad.keyboard ? std::string("Keyboard")
                        : "Controller " + std::to_string(pad.position + 1);
}

std::vector<PadSlot> assign_pads(const std::vector<KnownPad>& pads,
                                 const std::map<std::string, std::string>& chosen,
                                 bool gamepad_here) {
    std::vector<PadSlot> slots(pads.size());
    std::vector<bool> decided(pads.size(), false);
    bool player_taken[kPadPlayers] = {};
    bool gamepad_taken = false;

    // What the player chose comes first, so the defaults below fill in around
    // it rather than the other way round.
    for (std::size_t index = 0; index < pads.size(); ++index) {
        const auto it = chosen.find(pad_key(pads[index]));
        PadSlot slot;
        if (it == chosen.end() || !pad_slot_from_key(it->second, slot)) {
            continue;
        }
        slots[index] = slot;
        decided[index] = true;
        if (slot.kind == PadSlot::Kind::Player) player_taken[slot.player] = true;
        if (slot.kind == PadSlot::Kind::GamePad && !pads[index].keyboard) gamepad_taken = true;
    }

    // Keys go with their device: the GamePad's on the device holding it, and
    // nowhere on the other.
    for (std::size_t index = 0; index < pads.size(); ++index) {
        if (decided[index] || !pads[index].keyboard) continue;
        slots[index] = pads[index].on_this_device == gamepad_here ? PadSlot::gamepad()
                                                                  : PadSlot::not_used();
        decided[index] = true;
    }

    // The GamePad, from the device holding its screen, unless a controller was
    // already chosen for it: that device's first.
    if (!gamepad_taken) {
        std::size_t pick = pads.size();
        for (std::size_t index = 0; index < pads.size(); ++index) {
            if (decided[index] || pads[index].on_this_device != gamepad_here) continue;
            if (pick == pads.size() || pads[index].position < pads[pick].position) pick = index;
        }
        if (pick < pads.size()) {
            slots[pick] = PadSlot::gamepad();
            decided[pick] = true;
        }
    }

    // Everything else is a player, in order: this device's, then the
    // companion's.
    std::vector<std::size_t> order;
    for (std::size_t index = 0; index < pads.size(); ++index) {
        if (!decided[index]) order.push_back(index);
    }
    std::stable_sort(order.begin(), order.end(), [&](std::size_t left, std::size_t right) {
        if (pads[left].on_this_device != pads[right].on_this_device) {
            return pads[left].on_this_device;
        }
        return pads[left].position < pads[right].position;
    });
    for (const std::size_t index : order) {
        int player = 0;
        while (player < kPadPlayers && player_taken[player]) ++player;
        if (player < kPadPlayers) {
            slots[index] = PadSlot::player_number(player);
            player_taken[player] = true;
        } else {
            slots[index] = PadSlot::not_used();  // nine controllers and eight players
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
