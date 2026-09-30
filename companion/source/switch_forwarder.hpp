#pragma once

// What Forwarder Creation needs from a Switch: its keyboard, the network
// through libcurl, and the installer in switch/forwarder. Only on a Switch
// build that plays games here -- see local_play.hpp.

#include <memory>
#include <string>

#include "forwarder/forwarder_platform.hpp"

namespace ds::sw {

// `nro_path` is this program's own, argv[0]: what the forwarder will start.
std::unique_ptr<ForwarderPlatform> make_switch_forwarder_platform(const std::string& nro_path);

}  // namespace ds::sw
