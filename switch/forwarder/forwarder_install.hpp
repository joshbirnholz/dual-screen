// SPDX-License-Identifier: GPL-3.0-or-later
//
// Installs a forwarder on the Switch this runs on: packs it (forwarder_nca.hpp)
// with the console's own NCA header key, writes the NCAs to the SD card's
// content storage and tells the system it has a new title -- what Sphaira's
// owo.cpp does, adapted. See THIRD_PARTY.md.

#pragma once

#include <string>

#include "forwarder_nca.hpp"

namespace ds::forwarder {

// Everything in `contents` but the control data and hbloader, which this
// supplies: the .nro's own control data read from `contents.nro_path`, and the
// hbloader built into this program. Replaces a forwarder already made for the
// same .nro and arguments. Takes a second or two; not for the main thread.
bool install_forwarder(ForwarderContents contents, std::string* error);

}  // namespace ds::forwarder
