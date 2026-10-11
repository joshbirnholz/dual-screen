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

// For anything else here that uses libcurl -- the updater, switch_updater.cpp:
// whether this libcurl needs to be told which certificate authorities to
// trust, and a file of the ones the console trusts (empty when it would not
// give them).
bool curl_needs_ca_file();
std::string console_ca_file();

}  // namespace ds::sw
