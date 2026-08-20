#include "config/settings.hpp"

#include <nlohmann/json.hpp>

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>

#include "util/log.hpp"

namespace ds {
namespace {

using json = nlohmann::json;

template <typename T>
T get_or(const json& object, const char* key, const T& fallback) {
    if (!object.is_object()) return fallback;
    const auto it = object.find(key);
    if (it == object.end() || it->is_null()) return fallback;
    try {
        return it->get<T>();
    } catch (const json::exception&) {
        log_warn(std::string("settings.json: ignoring malformed value for '") + key + "'");
        return fallback;
    }
}

}  // namespace

namespace {

std::string default_device_name() {
    char hostname[256] = {0};
    if (::gethostname(hostname, sizeof(hostname) - 1) == 0 && hostname[0] != '\0') {
        return std::string(hostname);
    }
    return std::string("dualscreen");
}

}  // namespace

Settings Settings::load(const AppPaths& paths) {
    Settings settings;
    settings.system_dir = paths.default_system_dir;
    settings.device_name = default_device_name();

    std::ifstream file(paths.settings_file);
    if (!file.is_open()) {
        return settings;
    }

    json document;
    try {
        file >> document;
    } catch (const json::exception& error) {
        log_warn(std::string("could not parse settings.json (using defaults): ") + error.what());
        return settings;
    }

    settings.default_layout_id = get_or<std::string>(document, "default_layout_id", settings.default_layout_id);
    settings.device_name = get_or<std::string>(document, "device_name", settings.device_name);
    settings.last_browse_dir = expand_user_path(get_or<std::string>(document, "last_browse_dir", settings.last_browse_dir));
    settings.system_dir = expand_user_path(get_or<std::string>(document, "system_dir", settings.system_dir));
    settings.core_path = expand_user_path(get_or<std::string>(document, "core_path", settings.core_path));
    settings.default_bilinear_filtering =
        get_or<bool>(document, "default_bilinear_filtering", settings.default_bilinear_filtering);
    settings.audio_target = audio_target_from_key(
        get_or<std::string>(document, "audio_target", audio_target_key(settings.audio_target)));
    settings.handedness = handedness_from_key(
        get_or<std::string>(document, "handedness", handedness_key(settings.handedness)));
    settings.cursor_mode = cursor_mode_from_key(
        get_or<std::string>(document, "cursor_mode", cursor_mode_key(settings.cursor_mode)));
    settings.stick_deadzone = get_or<float>(document, "stick_deadzone", settings.stick_deadzone);
    settings.trigger_deadzone = get_or<float>(document, "trigger_deadzone", settings.trigger_deadzone);
    settings.vsync = get_or<bool>(document, "vsync", settings.vsync);

    if (settings.system_dir.empty()) {
        settings.system_dir = paths.default_system_dir;
    }
    return settings;
}

bool Settings::save(const AppPaths& paths) const {
    if (!ensure_directory(paths.config_dir)) {
        return false;
    }

    json document;
    document["default_layout_id"] = default_layout_id;
    document["device_name"] = device_name;
    document["last_browse_dir"] = last_browse_dir;
    document["system_dir"] = system_dir;
    document["core_path"] = core_path;
    document["default_bilinear_filtering"] = default_bilinear_filtering;
    document["audio_target"] = audio_target_key(audio_target);
    document["cursor_mode"] = cursor_mode_key(cursor_mode);
    document["handedness"] = handedness_key(handedness);
    document["stick_deadzone"] = stick_deadzone;
    document["trigger_deadzone"] = trigger_deadzone;
    document["vsync"] = vsync;

    const std::string temporary = paths.settings_file + ".tmp";
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
    if (std::rename(temporary.c_str(), paths.settings_file.c_str()) != 0) {
        log_error("could not replace " + paths.settings_file);
        return false;
    }
    return true;
}

std::vector<std::string> core_search_paths() {
    std::vector<std::string> paths;
    const AppPaths app_paths = AppPaths::resolve();
    paths.push_back(path_join(app_paths.config_dir, "cores/melondsds_libretro.so"));
    paths.push_back(expand_user_path("~/.config/retroarch/cores/melondsds_libretro.so"));
    paths.push_back(expand_user_path(
        "~/.var/app/org.libretro.RetroArch/config/retroarch/cores/melondsds_libretro.so"));
    paths.push_back(expand_user_path("~/.local/share/retroarch/cores/melondsds_libretro.so"));
    paths.push_back("/usr/lib/libretro/melondsds_libretro.so");
    paths.push_back("/usr/lib64/libretro/melondsds_libretro.so");
    paths.push_back("/usr/lib/x86_64-linux-gnu/libretro/melondsds_libretro.so");
    return paths;
}

}  // namespace ds
