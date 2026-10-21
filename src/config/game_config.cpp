#include "config/game_config.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>

#include "util/log.hpp"

namespace ds {
namespace {

using json = nlohmann::json;

bool get_bool(const json& object, const char* key, bool fallback) {
    if (!object.is_object()) return fallback;
    const auto it = object.find(key);
    if (it == object.end() || !it->is_boolean()) return fallback;
    return it->get<bool>();
}

int get_int(const json& object, const char* key, int fallback) {
    if (!object.is_object()) return fallback;
    const auto it = object.find(key);
    if (it == object.end() || !it->is_number_integer()) return fallback;
    return it->get<int>();
}

std::string get_string(const json& object, const char* key, const std::string& fallback) {
    if (!object.is_object()) return fallback;
    const auto it = object.find(key);
    if (it == object.end() || !it->is_string()) return fallback;
    return it->get<std::string>();
}

}  // namespace

std::string iso8601_utc_now() {
    const std::time_t now = std::time(nullptr);
    std::tm utc {};
#if defined(_WIN32)
    gmtime_s(&utc, &now);
#else
    gmtime_r(&now, &utc);
#endif
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return std::string(buffer);
}

std::string GameConfig::file_path(const AppPaths& paths, const std::string& rom_sha1) {
    return path_join(paths.games_dir, rom_sha1 + ".json");
}

GameConfig GameConfig::load_or_default(const AppPaths& paths,
                                       const Settings& settings,
                                       const std::string& rom_sha1,
                                       const std::string& display_name) {
    GameConfig config;
    config.rom_sha1 = rom_sha1;
    config.display_name = display_name;
    config.layout_id = kNewGameLayoutId;
    config.bilinear_filtering = settings.default_bilinear_filtering;
    config.render_scale = settings.default_render_scale;
    config.screen_gap = settings.default_screen_gap;
    config.right_stick_mode = settings.default_right_stick_mode;

    const std::string path = file_path(paths, rom_sha1);
    std::ifstream file(path);
    if (!file.is_open()) {
        return config;
    }

    std::stringstream contents;
    contents << file.rdbuf();
    const std::string raw = contents.str();

    json document;
    try {
        document = json::parse(raw);
    } catch (const json::exception& error) {
        log_warn("could not parse " + path + " (using defaults): " + error.what());
        return config;
    }
    if (!document.is_object()) {
        log_warn(path + " is not a JSON object; using defaults");
        return config;
    }

    config.raw_document_ = raw;
    config.layout_id = get_string(document, "layout_id", config.layout_id);
    config.bilinear_filtering = get_bool(document, "bilinear_filtering", config.bilinear_filtering);
    config.render_scale = get_int(document, "render_scale", config.render_scale);
    config.screen_gap = get_bool(document, "screen_gap", config.screen_gap);
    config.separate_screen =
        get_string(document, "separate_screen", "top") == "bottom" ? ScreenId::Bottom : ScreenId::Top;
    config.right_stick_mode = right_stick_mode_from_key(
        get_string(document, "right_stick_mode", right_stick_mode_key(config.right_stick_mode)));
    if (const auto found = document.find("button_mapping");
        found != document.end() && found->is_object()) {
        for (const auto& [key, action] : found->items()) {
            PadControl control = PadControl::A;
            if (action.is_string() && pad_control_from_key(key, control)) {
                config.button_mapping[control] = pad_action_from_key(action.get<std::string>());
            }
        }
    }
    // What a Turbo button presses, and where a Touch Point touches.
    if (const auto found = document.find("button_turbo"); found != document.end() && found->is_object()) {
        for (const auto& [key, action] : found->items()) {
            PadControl control = PadControl::A;
            if (action.is_string() && pad_control_from_key(key, control)) {
                config.button_mapping.turbo(control) = pad_action_from_key(action.get<std::string>());
            }
        }
    }
    if (const auto found = document.find("button_touch_points");
        found != document.end() && found->is_object()) {
        for (const auto& [key, spot] : found->items()) {
            PadControl control = PadControl::A;
            if (spot.is_array() && spot.size() == 2 && spot[0].is_number() && spot[1].is_number() &&
                pad_control_from_key(key, control)) {
                config.button_mapping.spot(control) =
                    TouchSpot{std::clamp(spot[0].get<float>(), 0.0f, 1.0f), std::clamp(spot[1].get<float>(), 0.0f, 1.0f)};
            }
        }
    }
    config.rumble_pak = get_bool(document, "rumble_pak", config.rumble_pak);
    config.custom_textures = get_bool(document, "custom_textures", config.custom_textures);
    config.lcd_filter = get_bool(document, "lcd_filter", config.lcd_filter);
    config.resume_from_save_state =
        get_bool(document, "resume_from_save_state", config.resume_from_save_state);
    config.screens_swapped = get_bool(document, "screens_swapped", config.screens_swapped);
    config.wii_u_options.clear();
    if (const auto found = document.find("wii_u_options");
        found != document.end() && found->is_object()) {
        for (const auto& [id, choice] : found->items()) {
            if (choice.is_number_integer()) config.wii_u_options[id] = choice.get<int>();
        }
    }
    config.last_played = get_string(document, "last_played", config.last_played);
    config.role_default = get_string(document, "role_default", config.role_default);
    config.companion_device_id = get_string(document, "companion_device_id", config.companion_device_id);
    // Prefer the name we were given (it comes from the ROM we actually loaded),
    // but keep the stored one if we have nothing better.
    if (config.display_name.empty()) {
        config.display_name = get_string(document, "display_name", std::string());
    }
    return config;
}

void GameConfig::touch_last_played() { last_played = iso8601_utc_now(); }

bool GameConfig::save(const AppPaths& paths) const {
    if (rom_sha1.empty()) {
        return false;  // nothing to key the file on
    }
    if (!ensure_directory(paths.games_dir)) {
        return false;
    }

    // Start from whatever was on disk so keys a newer build wrote survive.
    json document = json::object();
    if (!raw_document_.empty()) {
        try {
            json existing = json::parse(raw_document_);
            if (existing.is_object()) {
                document = std::move(existing);
            }
        } catch (const json::exception&) {
            // Already warned at load time.
        }
    }

    document["rom_sha1"] = rom_sha1;
    document["display_name"] = display_name;
    document["layout_id"] = layout_id;
    document["bilinear_filtering"] = bilinear_filtering;
    document["render_scale"] = render_scale;
    document["screen_gap"] = screen_gap;
    document["separate_screen"] = separate_screen == ScreenId::Bottom ? "bottom" : "top";
    document["right_stick_mode"] = right_stick_mode_key(right_stick_mode);
    document["button_mapping"] = json::object();
    for (std::size_t index = 0; index < kPadControls; ++index) {
        const PadControl control = static_cast<PadControl>(index);
        if (button_mapping[control] != PadAction::Standard) {
            document["button_mapping"][pad_control_key(control)] = pad_action_key(button_mapping[control]);
        }
    }
    document["button_turbo"] = json::object();
    document["button_touch_points"] = json::object();
    for (std::size_t index = 0; index < kPadControls; ++index) {
        const PadControl control = static_cast<PadControl>(index);
        if (button_mapping.turbo(control) != PadAction::Standard) {
            document["button_turbo"][pad_control_key(control)] = pad_action_key(button_mapping.turbo(control));
        }
        if (!(button_mapping.spot(control) == TouchSpot())) {
            document["button_touch_points"][pad_control_key(control)] =
                json::array({button_mapping.spot(control).u, button_mapping.spot(control).v});
        }
    }
    document["rumble_pak"] = rumble_pak;
    document["custom_textures"] = custom_textures;
    document["lcd_filter"] = lcd_filter;
    document["resume_from_save_state"] = resume_from_save_state;
    document["screens_swapped"] = screens_swapped;
    document["wii_u_options"] = json::object();
    for (const auto& [id, choice] : wii_u_options) document["wii_u_options"][id] = choice;
    document["last_played"] = last_played;
    // Reserved keys are written as nulls until the networked phase fills them,
    // so the shape of the file is stable from V1 onwards.
    document["role_default"] = role_default.empty() ? json(nullptr) : json(role_default);
    document["companion_device_id"] =
        companion_device_id.empty() ? json(nullptr) : json(companion_device_id);

    const std::string path = file_path(paths, rom_sha1);
    const std::string temporary = path + ".tmp";
    {
        std::ofstream file(temporary, std::ios::trunc);
        if (!file.is_open()) {
            log_error("could not write " + temporary);
            return false;
        }
        file << document.dump(2) << "\n";
        if (!file.good()) {
            log_error("could not write " + temporary);
            return false;
        }
    }
    std::string why;
    if (!replace_file(temporary, path, &why)) {
        log_error("could not save: " + why);
        std::remove(temporary.c_str());
        return false;
    }
    return true;
}

}  // namespace ds
