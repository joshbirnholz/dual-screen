#pragma once

#include <map>
#include <string>
#include <vector>

#include "config/paths.hpp"
#include "input/button_mapping.hpp"
#include "input/wii_u_mapping.hpp"

namespace ds {

// A button mapping kept under a name, for any game to load: a DS game's or a
// 3DS game's Change Controls, right stick and all, or one Wii U controller's
// Button Mapping. Loading one copies it into the game, which keeps it as its
// own from then on -- see MappingProfiles.
struct MappingProfile {
    std::string name;
    // A DS or 3DS profile's.
    ButtonMapping buttons;
    RightStickMode right_stick = RightStickMode::CStick;
    // A Wii U controller's.
    WiiUMapping wii_u;

    bool operator==(const MappingProfile& other) const {
        return name == other.name && buttons == other.buttons && right_stick == other.right_stick &&
               wii_u == other.wii_u;
    }
};

// What a profile is for, which is which list it is in: "ds", "3ds", or a Wii
// U controller's -- "wii_u.pro", "wii_u.remote" -- by wii_u_controller_key().
std::string mapping_profile_kind(SystemId system);
std::string mapping_profile_kind(WiiUController controller);
// The words over a kind's list: "DS Profiles", "Wii Remote + Nunchuk
// Profiles".
std::string mapping_profile_heading(const std::string& kind);

// Every profile, by kind, in the order they were saved. Kept by the device
// that runs games -- the one whose mappings they are -- in one file beside
// its settings, and the same for every game.
class MappingProfiles {
public:
    static constexpr std::size_t kMaxName = 32;
    static constexpr std::size_t kMaxPerKind = 64;

    static std::string file_path(const AppPaths& paths);
    // Missing or unreadable is none at all.
    static MappingProfiles load(const AppPaths& paths);
    bool save(const AppPaths& paths) const;

    const std::vector<MappingProfile>& of(const std::string& kind) const;
    // A new one at the end of its list: false with the list full.
    bool add(const std::string& kind, MappingProfile profile);
    // One put over, keeping its name; one taken out.
    bool replace(const std::string& kind, std::size_t index, const MappingProfile& profile);
    bool remove(const std::string& kind, std::size_t index);

    bool operator==(const MappingProfiles& other) const { return by_kind_ == other.by_kind_; }

private:
    std::map<std::string, std::vector<MappingProfile>> by_kind_;
};

}  // namespace ds
