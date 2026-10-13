#include "config/settings.hpp"

#include <nlohmann/json.hpp>

#include <unistd.h>

#if defined(__ANDROID__)
#include <SDL_system.h>
#include <jni.h>
#endif

#include "input/switch_console.hpp"

#include <algorithm>
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

// The spellings of Settings::wii_u_screen_alone in a settings file. Not "top"
// and "bottom": those are this app's names for a console's two screens in
// general, and for a Wii U they are a television across the room and a pad in
// your hands. A person editing this file by hand should see the words that
// mean something about a Wii U.
const char* screen_alone_key(WiiUScreenAlone screen) {
    switch (screen) {
        case WiiUScreenAlone::Auto: return "auto";
        case WiiUScreenAlone::Television: return "tv";
        case WiiUScreenAlone::GamePad: return "gamepad";
    }
    return "auto";
}

WiiUScreenAlone screen_alone_from_key(const std::string& key) {
    if (key == "gamepad") return WiiUScreenAlone::GamePad;
    if (key == "tv") return WiiUScreenAlone::Television;
    // Anything else, including a file written by a version that never had
    // this, is Auto: the one answer that is right for a Deck in the hand and
    // a Deck on a dock alike.
    return WiiUScreenAlone::Auto;
}

#if defined(__ANDROID__)
// What Android calls this device -- DualscreenActivity.deviceName(). Empty
// when it cannot be asked.
std::string android_device_name() {
    JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    jobject activity = static_cast<jobject>(SDL_AndroidGetActivity());
    if (env == nullptr || activity == nullptr) return std::string();
    std::string name;
    jclass type = env->GetObjectClass(activity);
    jmethodID method = env->GetMethodID(type, "deviceName", "()Ljava/lang/String;");
    if (method != nullptr) {
        auto text = static_cast<jstring>(env->CallObjectMethod(activity, method));
        if (!env->ExceptionCheck() && text != nullptr) {
            const char* chars = env->GetStringUTFChars(text, nullptr);
            if (chars != nullptr) {
                name = chars;
                env->ReleaseStringUTFChars(text, chars);
            }
        }
        if (text != nullptr) env->DeleteLocalRef(text);
    }
    if (env->ExceptionCheck()) env->ExceptionClear();
    env->DeleteLocalRef(type);
    env->DeleteLocalRef(activity);
    return name;
}
#endif

std::string default_device_name() {
#if defined(__ANDROID__)
    // Not the host name: every Android device's is "localhost".
    const std::string name = android_device_name();
    return name.empty() ? std::string("Android") : name;
#elif defined(__SWITCH__)
    // What the console is, the same name the companion gives itself -- a
    // Switch has no host name worth showing.
    return switch_console_name();
#else
    char hostname[256] = {0};
    if (::gethostname(hostname, sizeof(hostname) - 1) == 0 && hostname[0] != '\0') {
        return std::string(hostname);
    }
    return std::string("Dualscreen");
#endif
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

    settings.device_name = get_or<std::string>(document, "device_name", settings.device_name);
#if defined(__ANDROID__)
    // Written by a build that took the host name, which on Android is the
    // same "localhost" everywhere: not a name anybody chose.
    if (settings.device_name.empty() || settings.device_name == "localhost") {
        settings.device_name = default_device_name();
    }
#endif
    settings.last_browse_dir = expand_user_path(get_or<std::string>(document, "last_browse_dir", settings.last_browse_dir));
    settings.amiibo_dir = expand_user_path(get_or<std::string>(document, "amiibo_dir", settings.amiibo_dir));
    settings.steamgriddb_api_key =
        get_or<std::string>(document, "steamgriddb_api_key", settings.steamgriddb_api_key);
    settings.system_dir = expand_user_path(get_or<std::string>(document, "system_dir", settings.system_dir));
    settings.core_path = expand_user_path(get_or<std::string>(document, "core_path", settings.core_path));
    settings.core_path_3ds =
        expand_user_path(get_or<std::string>(document, "core_path_3ds", settings.core_path_3ds));
    settings.wii_u_program =
        expand_user_path(get_or<std::string>(document, "wii_u_program", settings.wii_u_program));
    settings.wii_u_mlc_path =
        expand_user_path(get_or<std::string>(document, "wii_u_mlc_path", settings.wii_u_mlc_path));
    settings.pause_wii_u_in_menu =
        get_or<bool>(document, "pause_wii_u_in_menu", settings.pause_wii_u_in_menu);
    if (document.contains("wii_u_arguments") && document["wii_u_arguments"].is_array()) {
        settings.wii_u_arguments.clear();
        for (const auto& argument : document["wii_u_arguments"]) {
            if (argument.is_string()) {
                settings.wii_u_arguments.push_back(argument.get<std::string>());
            }
        }
    }
    settings.default_bilinear_filtering =
        get_or<bool>(document, "default_bilinear_filtering", settings.default_bilinear_filtering);
    settings.default_render_scale =
        get_or<int>(document, "default_render_scale", settings.default_render_scale);
    settings.default_screen_gap =
        get_or<bool>(document, "default_screen_gap", settings.default_screen_gap);
    settings.default_right_stick_mode = right_stick_mode_from_key(get_or<std::string>(
        document, "default_right_stick_mode",
        right_stick_mode_key(settings.default_right_stick_mode)));
    settings.left_stick_press = left_stick_press_from_key(get_or<std::string>(
        document, "left_stick_press", left_stick_press_key(settings.left_stick_press)));
    // Left alone when the file says something this build does not know, which
    // is what keeps a settings file written by a later version from resetting
    // the choice rather than merely not understanding it.
    pad_glyph_style_from_string(
        get_or<std::string>(document, "pad_glyphs", to_string(settings.pad_glyphs)),
        settings.pad_glyphs);
    confirm_button_from_string(
        get_or<std::string>(document, "confirm_button", to_string(settings.confirm_button)),
        settings.confirm_button);
    settings.video_hold = video::video_hold_from_key(
        get_or<std::string>(document, "video_hold", video::video_hold_key(settings.video_hold)),
        settings.video_hold);
    settings.run_ahead_ds = static_cast<unsigned>(
        std::clamp(get_or<int>(document, "run_ahead_ds", static_cast<int>(settings.run_ahead_ds)),
                   0, 2));
    settings.audio_target = audio_target_from_key(
        get_or<std::string>(document, "audio_target", audio_target_key(settings.audio_target)));
    const auto left_handed = document.find("left_handed_layouts");
    if (left_handed != document.end() && left_handed->is_array()) {
        for (const auto& id : *left_handed) {
            if (id.is_string()) settings.left_handed_layouts.push_back(id.get<std::string>());
        }
    } else {
        settings.legacy_left_handed =
            handedness_from_key(get_or<std::string>(document, "handedness", "right")) ==
            Handedness::Left;
    }
    // Under a new name, and the old one is not read. Every file the old name
    // is in was written by a version that stored what it had guessed from
    // the machine's shape whether or not anybody chose it, so what is there
    // is almost always a guess rather than a choice -- and carrying it over
    // would keep a Deck pinned to GamePad on the dock, the one case Auto
    // exists for.
    settings.wii_u_screen_alone = screen_alone_from_key(get_or<std::string>(
        document, "wii_u_alone", screen_alone_key(settings.wii_u_screen_alone)));
    settings.cursor_mode = cursor_mode_from_key(
        get_or<std::string>(document, "cursor_mode", cursor_mode_key(settings.cursor_mode)));
    settings.stick_deadzone = get_or<float>(document, "stick_deadzone", settings.stick_deadzone);
    settings.trigger_deadzone = get_or<float>(document, "trigger_deadzone", settings.trigger_deadzone);
    settings.vsync = get_or<bool>(document, "vsync", settings.vsync);
    settings.ui_sounds = get_or<bool>(document, "ui_sounds", settings.ui_sounds);
    settings.ui_sound_style = get_or<std::string>(document, "ui_sound_style", settings.ui_sound_style);
    settings.show_battery = get_or<bool>(document, "show_battery", settings.show_battery);

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
    document["device_name"] = device_name;
    document["last_browse_dir"] = last_browse_dir;
    document["amiibo_dir"] = amiibo_dir;
    if (!steamgriddb_api_key.empty()) document["steamgriddb_api_key"] = steamgriddb_api_key;
    document["system_dir"] = system_dir;
    document["core_path"] = core_path;
    document["core_path_3ds"] = core_path_3ds;
    document["wii_u_program"] = wii_u_program;
    document["wii_u_arguments"] = wii_u_arguments;
    if (!wii_u_mlc_path.empty()) document["wii_u_mlc_path"] = wii_u_mlc_path;
    document["pause_wii_u_in_menu"] = pause_wii_u_in_menu;
    document["default_bilinear_filtering"] = default_bilinear_filtering;
    document["default_render_scale"] = default_render_scale;
    document["default_screen_gap"] = default_screen_gap;
    document["default_right_stick_mode"] = right_stick_mode_key(default_right_stick_mode);
    document["left_stick_press"] = left_stick_press_key(left_stick_press);
    document["pad_glyphs"] = to_string(pad_glyphs);
    document["confirm_button"] = to_string(confirm_button);
    document["video_hold"] = video::video_hold_key(video_hold);
    document["run_ahead_ds"] = run_ahead_ds;
    document["wii_u_alone"] = screen_alone_key(wii_u_screen_alone);
    document["audio_target"] = audio_target_key(audio_target);
    document["cursor_mode"] = cursor_mode_key(cursor_mode);
    document["left_handed_layouts"] = left_handed_layouts;
    document["stick_deadzone"] = stick_deadzone;
    document["trigger_deadzone"] = trigger_deadzone;
    document["vsync"] = vsync;
    document["ui_sounds"] = ui_sounds;
    document["ui_sound_style"] = ui_sound_style;
    document["show_battery"] = show_battery;

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
    std::string why;
    if (!replace_file(temporary, paths.settings_file, &why)) {
        log_error("could not save: " + why);
        return false;
    }
    return true;
}

std::vector<std::string> core_search_paths(SystemId system) {
    // The usual places a libretro core ends up, crossed with the filenames this
    // system's cores go by. The order is preference: this app's own folder
    // first, then RetroArch's, then the distribution's.
    static const char* const kDirectories[] = {
        nullptr,  // stands in for this app's config directory, filled in below
        "~/.config/retroarch/cores",
        "~/.var/app/org.libretro.RetroArch/config/retroarch/cores",
        "~/.local/share/retroarch/cores",
        "/usr/lib/libretro",
        "/usr/lib64/libretro",
        "/usr/lib/x86_64-linux-gnu/libretro",
    };

    const AppPaths app_paths = AppPaths::resolve();
    const std::string own_cores = path_join(app_paths.config_dir, "cores");

    std::vector<std::string> paths;
    for (const std::string& filename : system_profile(system).core_filenames) {
        for (const char* directory : kDirectories) {
            const std::string folder =
                (directory == nullptr) ? own_cores : expand_user_path(directory);
            paths.push_back(path_join(folder, filename));
        }
    }
    return paths;
}

}  // namespace ds
