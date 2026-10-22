#include "config/mapping_profiles.hpp"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <fstream>
#include <sstream>

#include "config/mapping_json.hpp"
#include "input/controller_mapping.hpp"
#include "util/log.hpp"

namespace ds {

using json = nlohmann::json;

namespace {

const std::string kWiiUPrefix = "wii_u.";

}  // namespace

std::string mapping_profile_kind(SystemId system) { return system == SystemId::ThreeDs ? "3ds" : "ds"; }

std::string mapping_profile_kind(WiiUController controller) {
    return kWiiUPrefix + wii_u_controller_key(controller);
}

std::string mapping_profile_heading(const std::string& kind) {
    if (kind == "3ds") return "3DS Profiles";
    if (kind.rfind(kWiiUPrefix, 0) == 0) {
        for (const WiiUController controller :
             {WiiUController::GamePad, WiiUController::Pro, WiiUController::Remote, WiiUController::RemoteNunchuk,
              WiiUController::RemoteClassic}) {
            if (kind == mapping_profile_kind(controller)) {
                return std::string(wii_u_controller_name(controller)) + " Profiles";
            }
        }
    }
    return "DS Profiles";
}

std::string MappingProfiles::file_path(const AppPaths& paths) {
    return path_join(paths.config_dir, "mapping_profiles.json");
}

MappingProfiles MappingProfiles::load(const AppPaths& paths) {
    MappingProfiles profiles;
    const std::string path = file_path(paths);
    std::ifstream file(path);
    if (!file.is_open()) return profiles;
    std::stringstream contents;
    contents << file.rdbuf();
    json document;
    try {
        document = json::parse(contents.str());
    } catch (const json::exception& error) {
        log_warn("could not read the mapping profiles in " + path + ": " + error.what());
        return profiles;
    }
    const auto kinds = document.find("profiles");
    if (!document.is_object() || kinds == document.end() || !kinds->is_object()) return profiles;
    for (const auto& [kind, list] : kinds->items()) {
        if (!list.is_array()) continue;
        const bool wii_u = kind.rfind(kWiiUPrefix, 0) == 0;
        for (const json& kept : list) {
            if (!kept.is_object()) continue;
            const auto name = kept.find("name");
            if (name == kept.end() || !name->is_string()) continue;
            MappingProfile profile;
            profile.name = name->get<std::string>();
            if (wii_u) {
                if (const auto mapping = kept.find("wii_u"); mapping != kept.end()) {
                    profile.wii_u = wii_u_mapping_from_json(*mapping);
                }
            } else {
                read_button_mapping(kept, profile.buttons);
                if (const auto stick = kept.find("right_stick_mode"); stick != kept.end() && stick->is_string()) {
                    profile.right_stick = right_stick_mode_from_key(stick->get<std::string>());
                }
            }
            profiles.add(kind, std::move(profile));
        }
    }
    return profiles;
}

bool MappingProfiles::save(const AppPaths& paths) const {
    if (!ensure_directory(paths.config_dir)) return false;
    json kinds = json::object();
    for (const auto& [kind, list] : by_kind_) {
        if (list.empty()) continue;
        const bool wii_u = kind.rfind(kWiiUPrefix, 0) == 0;
        json kept_list = json::array();
        for (const MappingProfile& profile : list) {
            json kept = {{"name", profile.name}};
            if (wii_u) {
                kept["wii_u"] = wii_u_mapping_json(profile.wii_u);
            } else {
                write_button_mapping(profile.buttons, kept);
                kept["right_stick_mode"] = right_stick_mode_key(profile.right_stick);
            }
            kept_list.push_back(std::move(kept));
        }
        kinds[kind] = std::move(kept_list);
    }
    const json document = {{"profiles", kinds}};
    const std::string path = file_path(paths);
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
        log_error("could not save the mapping profiles: " + why);
        std::remove(temporary.c_str());
        return false;
    }
    return true;
}

const std::vector<MappingProfile>& MappingProfiles::of(const std::string& kind) const {
    static const std::vector<MappingProfile> kNone;
    const auto found = by_kind_.find(kind);
    return found != by_kind_.end() ? found->second : kNone;
}

bool MappingProfiles::add(const std::string& kind, MappingProfile profile) {
    std::vector<MappingProfile>& list = by_kind_[kind];
    if (list.size() >= kMaxPerKind) return false;
    list.push_back(std::move(profile));
    return true;
}

bool MappingProfiles::replace(const std::string& kind, std::size_t index, const MappingProfile& profile) {
    const auto found = by_kind_.find(kind);
    if (found == by_kind_.end() || index >= found->second.size()) return false;
    const std::string name = found->second[index].name;
    found->second[index] = profile;
    found->second[index].name = name;
    return true;
}

bool MappingProfiles::remove(const std::string& kind, std::size_t index) {
    const auto found = by_kind_.find(kind);
    if (found == by_kind_.end() || index >= found->second.size()) return false;
    found->second.erase(found->second.begin() + static_cast<std::ptrdiff_t>(index));
    return true;
}

}  // namespace ds
