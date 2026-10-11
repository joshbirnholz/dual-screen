#pragma once

// A Switch's half of the updater (see src/update/updater.hpp): the .nro
// downloaded with libcurl and written over the one this program was started
// from, which the homebrew loader then starts again -- and which every
// forwarder made from it starts from then on, since they name its path.
//
// Only on a Switch build with libcurl and a DS core, which is what CI builds;
// anywhere else make_switch_update_backend() gives nothing, and nothing is
// offered.

#include <memory>
#include <string>

#include "update/updater.hpp"

namespace ds::sw {

inline constexpr const char* kSwitchAsset = "Dualscreen.nro";

// `nro_path` is argv[0]; `staging_dir` where downloads wait to be put in place.
std::unique_ptr<update::Backend> make_switch_update_backend(const std::string& nro_path,
                                                            const std::string& staging_dir);

}  // namespace ds::sw
