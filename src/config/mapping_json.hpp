#pragma once

#include <nlohmann/json.hpp>

#include "input/button_mapping.hpp"
#include "input/wii_u_mapping.hpp"

namespace ds {

// A DS or 3DS game's ButtonMapping as JSON, under the keys a game's config
// has always kept it in -- "button_mapping", "button_turbo",
// "button_touch_points", "touch_analog_stick" -- written into `document`, an
// object, and read back out of one. Only what differs from Standard is
// written; what is missing reads as Standard.
void write_button_mapping(const ButtonMapping& mapping, nlohmann::json& document);
void read_button_mapping(const nlohmann::json& document, ButtonMapping& mapping);

// A Wii U controller's, as {"actions": {...}, "turbo": {...}}.
nlohmann::json wii_u_mapping_json(const WiiUMapping& mapping);
WiiUMapping wii_u_mapping_from_json(const nlohmann::json& kept);
// Profile Switching, as a game keeps it for a player: its button, "minus",
// "plus" or "off", and the profiles it moves through.
nlohmann::json profile_switching_json(const ProfileSwitching& switching);
ProfileSwitching profile_switching_from_json(const nlohmann::json& kept);

}  // namespace ds
