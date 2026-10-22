#include "config/mapping_json.hpp"

#include <algorithm>
#include <string>

namespace ds {

using json = nlohmann::json;

void write_button_mapping(const ButtonMapping& mapping, json& document) {
    document["button_mapping"] = json::object();
    for (std::size_t index = 0; index < kPadControls; ++index) {
        const PadControl control = static_cast<PadControl>(index);
        if (mapping[control] != PadAction::Standard) {
            document["button_mapping"][pad_control_key(control)] = pad_action_key(mapping[control]);
        }
    }
    document["button_turbo"] = json::object();
    document["button_touch_points"] = json::object();
    for (std::size_t index = 0; index < kPadControls; ++index) {
        const PadControl control = static_cast<PadControl>(index);
        if (mapping.turbo(control) != PadAction::Standard) {
            document["button_turbo"][pad_control_key(control)] = pad_action_key(mapping.turbo(control));
        }
        if (!(mapping.spot(control) == TouchSpot())) {
            document["button_touch_points"][pad_control_key(control)] =
                json::array({mapping.spot(control).u, mapping.spot(control).v});
        }
    }
    document["touch_analog_stick"] =
        json::array({mapping.stick_centre.u, mapping.stick_centre.v, mapping.stick_radius});
}

void read_button_mapping(const json& document, ButtonMapping& mapping) {
    if (!document.is_object()) return;
    if (const auto found = document.find("button_mapping"); found != document.end() && found->is_object()) {
        for (const auto& [key, action] : found->items()) {
            PadControl control = PadControl::A;
            if (action.is_string() && pad_control_from_key(key, control)) {
                mapping[control] = pad_action_from_key(action.get<std::string>());
            }
        }
    }
    // What a Turbo button presses, and where a Touch Point touches.
    if (const auto found = document.find("button_turbo"); found != document.end() && found->is_object()) {
        for (const auto& [key, action] : found->items()) {
            PadControl control = PadControl::A;
            if (action.is_string() && pad_control_from_key(key, control)) {
                mapping.turbo(control) = pad_action_from_key(action.get<std::string>());
            }
        }
    }
    if (const auto found = document.find("touch_analog_stick"); found != document.end() && found->is_array() &&
                                                                 found->size() == 3 && (*found)[0].is_number() &&
                                                                 (*found)[1].is_number() && (*found)[2].is_number()) {
        mapping.stick_centre = TouchSpot{std::clamp((*found)[0].get<float>(), 0.0f, 1.0f),
                                         std::clamp((*found)[1].get<float>(), 0.0f, 1.0f)};
        mapping.stick_radius = clamp_stick_radius((*found)[2].get<float>());
    }
    if (const auto found = document.find("button_touch_points"); found != document.end() && found->is_object()) {
        for (const auto& [key, spot] : found->items()) {
            PadControl control = PadControl::A;
            if (spot.is_array() && spot.size() == 2 && spot[0].is_number() && spot[1].is_number() &&
                pad_control_from_key(key, control)) {
                mapping.spot(control) = TouchSpot{std::clamp(spot[0].get<float>(), 0.0f, 1.0f),
                                                  std::clamp(spot[1].get<float>(), 0.0f, 1.0f)};
            }
        }
    }
}

json wii_u_mapping_json(const WiiUMapping& mapping) {
    json kept = {{"actions", json::object()}, {"turbo", json::object()}};
    for (std::size_t index = 0; index < kPadControls; ++index) {
        const PadControl control = static_cast<PadControl>(index);
        if (mapping[control] != WiiUAction::Standard) {
            kept["actions"][pad_control_key(control)] = wii_u_action_key(mapping[control]);
        }
        if (mapping.turbo(control) != WiiUAction::Standard) {
            kept["turbo"][pad_control_key(control)] = wii_u_action_key(mapping.turbo(control));
        }
    }
    return kept;
}

WiiUMapping wii_u_mapping_from_json(const json& kept) {
    WiiUMapping mapping;
    if (!kept.is_object()) return mapping;
    for (const char* part : {"actions", "turbo"}) {
        const auto actions = kept.find(part);
        if (actions == kept.end() || !actions->is_object()) continue;
        for (const auto& [control_key, action] : actions->items()) {
            PadControl control = PadControl::A;
            if (!action.is_string() || !pad_control_from_key(control_key, control)) continue;
            const WiiUAction read = wii_u_action_from_key(action.get<std::string>());
            if (std::string(part) == "actions") {
                mapping[control] = read;
            } else {
                mapping.turbo(control) = read;
            }
        }
    }
    return mapping;
}

}  // namespace ds
