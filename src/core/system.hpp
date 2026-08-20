#pragma once

#include <string>
#include <vector>

#include "layout/layout.hpp"

namespace ds {

// Which console a game belongs to. Everything that differs between them -- the
// size and shape of each screen, the console the Standard layout draws, which
// core plays it, what its ROMs are called -- hangs off this and nowhere else.
enum class SystemId { Ds, ThreeDs };

struct SystemProfile {
    SystemId id = SystemId::Ds;
    // Shown to the player, so it is the console's name rather than an id.
    std::string name;
    // The screens and the console they sit in.
    SystemScreens screens;
    // Filename extensions that mean "this is a game for this system", lowercase
    // and without the dot.
    std::vector<std::string> rom_extensions;
    // Library filenames of the cores that can play it, most preferred first.
    std::vector<std::string> core_filenames;
    // What to call the core when telling the player one is missing.
    std::string core_description;
};

const SystemProfile& system_profile(SystemId id);
// Every system, in a stable order.
const std::vector<SystemProfile>& system_profiles();

// Which system a file belongs to, by extension. False if nothing recognises it.
bool system_for_rom(const std::string& path, SystemId& out_system);
// True if any system claims this file, which is what the browser lists.
bool is_supported_rom_for_any_system(const std::string& filename);

// Round-trips through config files and over the wire.
const char* system_key(SystemId id);
bool system_from_key(const std::string& key, SystemId& out_system);

}  // namespace ds
